/* SPDX-License-Identifier: Apache-2.0
 *
 * gui_adapter.cc — Gui service root and per-session Gui::Session
 * adapter for the Sponge compositor. See gui_adapter.h for the contract.
 *
 * Every scene operation is forwarded to the Mojo-owned core through the
 * C ABI of abi.h. The C++ side decodes and forwards; it never models or
 * inspects the scene.
 */

#include <base/session_label.h>

#include "gui_adapter.h"

using namespace Sponge;


/*
 * The C++ 'Gui::Session::Command::Opcode' values map 1:1 onto the
 * SC_* opcode constants of abi.h. The decode below relies on this.
 */
static_assert((int)Gui::Session::Command::GEOMETRY   == SC_GEOMETRY,   "opcode mapping");
static_assert((int)Gui::Session::Command::OFFSET     == SC_OFFSET,     "opcode mapping");
static_assert((int)Gui::Session::Command::FRONT      == SC_FRONT,      "opcode mapping");
static_assert((int)Gui::Session::Command::BACK       == SC_BACK,       "opcode mapping");
static_assert((int)Gui::Session::Command::FRONT_OF   == SC_FRONT_OF,   "opcode mapping");
static_assert((int)Gui::Session::Command::BEHIND_OF  == SC_BEHIND_OF,  "opcode mapping");
static_assert((int)Gui::Session::Command::BACKGROUND == SC_BACKGROUND, "opcode mapping");
static_assert((int)Gui::Session::Command::TITLE      == SC_TITLE,      "opcode mapping");
static_assert((int)Gui::Session::Command::NOP        == SC_NOP,        "opcode mapping");


static Genode::uint64_t view_id_value(Gui::View_id id)
{
	return (Genode::uint64_t)id.value;
}


/*******************************
 ** Gui_session_component    **
 *******************************/

Gui_session_component::Gui_session_component(
	Genode::Env                &env,
	Genode::uint8_t            *state,
	Input_pipeline             &pipeline,
	Genode::int32_t             scene_handle,
	Genode::Session::Resources  const &resources,
	Genode::Session::Label      const &label,
	Genode::int32_t             screen_w,
	Genode::int32_t             screen_h)
:
	Genode::Session_object<Gui::Session>(env.ep(), resources, label),
	_env(env), _state(state), _pipeline(pipeline), _handle(scene_handle),
	_screen_w(screen_w), _screen_h(screen_h),
	_fb(env, state, resources, label),
	_input(env, state)
{
	/*
	 * Register this session's Input component with the input pipeline
	 * (per-session delivery target + hover-report label). The
	 * registration is dropped again in the destructor before the
	 * scene session closes.
	 */
	_pipeline.register_session(_handle, &_input.component(), label);
}


Gui_session_component::~Gui_session_component()
{
	_pipeline.unregister_session(_handle);

	/*
	 * Close the scene session: all views of this session are removed
	 * from the scene and their pixel bindings are dropped before the
	 * member dataspaces are released.
	 */
	sc_session_close(_state, _handle);
}


void Gui_session_component::_register_view(Genode::uint64_t id)
{
	for (unsigned i = 0; i < MAX_VIEWS; i++) {
		if (_views[i].used && _views[i].id == id)
			return;
	}
	for (unsigned i = 0; i < MAX_VIEWS; i++) {
		if (!_views[i].used) {
			_views[i].used = true;
			_views[i].id   = id;
			return;
		}
	}
	Genode::warning("view registry full, view ", id, " stays unbound");
}


void Gui_session_component::_forget_view(Genode::uint64_t id)
{
	for (unsigned i = 0; i < MAX_VIEWS; i++) {
		if (_views[i].used && _views[i].id == id) {
			_views[i].used = false;
			_views[i].id   = 0;
			return;
		}
	}
}


void Gui_session_component::_bind_view(Genode::uint64_t id)
{
	Genode::uint32_t const *const pixels = _fb.pixels();

	/* nothing to bind before the client defined its buffer */
	if (!pixels)
		return;

	/*
	 * Bind the full surface set (abi.h): the pixel plane plus the
	 * alpha and input-mask planes of an 'alpha' buffer. Non-alpha
	 * buffers pass absent planes and keep the view hit-testing
	 * opaque; an 'alpha' buffer consults its input-mask plane, where
	 * a zero mask byte falls through to the views below.
	 */
	Genode::int32_t const r = sc_view_bind_surfaces(
		_state, _handle, id, pixels, _fb.pitch(),
		_fb.alpha(), _fb.byte_pitch(),
		_fb.input_mask(), _fb.byte_pitch());

	if (r != 0)
		Genode::warning("sc_view_bind_surfaces failed (", r, ") for view ", id);
}


void Gui_session_component::_bind_all_views()
{
	for (unsigned i = 0; i < MAX_VIEWS; i++)
		if (_views[i].used)
			_bind_view(_views[i].id);
}


void Gui_session_component::_execute_command(Gui::Session::Command const &cmd)
{
	using Command = Gui::Session::Command;

	switch (cmd.opcode) {

	case Command::GEOMETRY:
		sc_apply_command(_state, _handle, SC_GEOMETRY,
		                 view_id_value(cmd.geometry.view),
		                 (Genode::int32_t)cmd.geometry.rect.x1(),
		                 (Genode::int32_t)cmd.geometry.rect.y1(),
		                 (Genode::int32_t)cmd.geometry.rect.w(),
		                 (Genode::int32_t)cmd.geometry.rect.h(), 0);
		return;

	case Command::OFFSET:
		sc_apply_command(_state, _handle, SC_OFFSET,
		                 view_id_value(cmd.offset.view),
		                 (Genode::int32_t)cmd.offset.offset.x,
		                 (Genode::int32_t)cmd.offset.offset.y, 0, 0, 0);
		return;

	case Command::FRONT:
		sc_apply_command(_state, _handle, SC_FRONT,
		                 view_id_value(cmd.front.view), 0, 0, 0, 0, 0);
		return;

	case Command::BACK:
		sc_apply_command(_state, _handle, SC_BACK,
		                 view_id_value(cmd.back.view), 0, 0, 0, 0, 0);
		return;

	case Command::FRONT_OF:
		sc_apply_command(_state, _handle, SC_FRONT_OF,
		                 view_id_value(cmd.front_of.view), 0, 0, 0, 0,
		                 view_id_value(cmd.front_of.neighbor));
		return;

	case Command::BEHIND_OF:
		sc_apply_command(_state, _handle, SC_BEHIND_OF,
		                 view_id_value(cmd.behind_of.view), 0, 0, 0, 0,
		                 view_id_value(cmd.behind_of.neighbor));
		return;

	case Command::BACKGROUND:
		Genode::warning("not implemented before G2: background");
		sc_apply_command(_state, _handle, SC_BACKGROUND,
		                 view_id_value(cmd.background.view), 0, 0, 0, 0, 0);
		return;

	case Command::TITLE:
		Genode::warning("not implemented before G2: title");
		sc_apply_command(_state, _handle, SC_TITLE,
		                 view_id_value(cmd.title.view), 0, 0, 0, 0, 0);
		return;

	case Command::NOP:
		/* no operands to forward (safe no-op) */
		return;
	}
}


Framebuffer::Session_capability Gui_session_component::framebuffer()
{
	return _fb.cap();
}


Input::Session_capability Gui_session_component::input()
{
	return _input.cap();
}


Gui_session_component::Info_result Gui_session_component::info()
{
	if (!_info_rom.constructed()) {
		using Ram  = Genode::Ram_quota;
		using Caps = Genode::Cap_quota;

		if (!_ram_quota_guard().try_withdraw(Ram { 8*1024 }))
			return Info_error::OUT_OF_RAM;
		if (!_cap_quota_guard().try_withdraw(Caps { 2 }))
			return Info_error::OUT_OF_CAPS;

		_info_rom.construct(*this);
	}
	return Info_result { _info_rom->cap() };
}


Gui_session_component::View_result
Gui_session_component::view(Gui::View_id id, View_attr const &attr)
{
	Genode::int32_t const r = sc_view_create(
		_state, _handle, view_id_value(id),
		(Genode::int32_t)attr.rect.x1(), (Genode::int32_t)attr.rect.y1(),
		(Genode::int32_t)attr.rect.w(),  (Genode::int32_t)attr.rect.h(),
		attr.front ? 1 : 0);

	switch (r) {

	case 0:
		_register_view(view_id_value(id));

		/*
		 * When the client already defined its buffer, bind it to the
		 * new view right away (the buffer call may have happened
		 * before the view existed).
		 */
		_bind_view(view_id_value(id));
		return View_result::OK;

	case -2:
		Genode::warning("scene view capacity exhausted");
		return View_result::OUT_OF_RAM;

	default:
		/*
		 * Permanent errors (duplicate view id, bad geometry) must not
		 * surface as OUT_OF_*: the client-side retry loop would never
		 * terminate. Report the failure and consume the call.
		 */
		Genode::warning("sc_view_create failed (", r, ") for view ", id);
		return View_result::OK;
	}
}


Gui_session_component::Child_view_result
Gui_session_component::child_view(Gui::View_id, Gui::View_id, View_attr const &)
{
	Genode::warning("not implemented before G2: child_view");
	return Child_view_result::INVALID;
}


void Gui_session_component::destroy_view(Gui::View_id id)
{
	sc_view_destroy(_state, _handle, view_id_value(id));
	_forget_view(view_id_value(id));
}


Gui_session_component::Associate_result
Gui_session_component::associate(Gui::View_id, Gui::View_capability)
{
	Genode::warning("not implemented before G2: associate");
	return Associate_result::INVALID;
}


Gui_session_component::View_capability_result
Gui_session_component::view_capability(Gui::View_id /* id */)
{
	Genode::warning("not implemented before G2: view_capability");

	/*
	 * Safe null result: an invalid capability. Every error result of
	 * this RPC is retried by 'Gui::Connection' after a quota upgrade,
	 * so a permanent failure must be reported as a null capability
	 * instead of an error.
	 */
	return View_capability_result { Gui::View_capability() };
}


void Gui_session_component::release_view_id(Gui::View_id /* id */)
{
	Genode::warning("not implemented before G2: release_view_id");
}


Genode::Dataspace_capability Gui_session_component::command_dataspace()
{
	return _command_ds.cap();
}


void Gui_session_component::execute()
{
	unsigned const n = _command_buffer.num();

	for (unsigned i = 0; i < n; i++)
		_execute_command(_command_buffer.get(i));
}


Gui_session_component::Buffer_result
Gui_session_component::buffer(Framebuffer::Mode mode)
{
	if (!_fb.define_buffer(mode))
		return Buffer_result::OUT_OF_RAM;

	/*
	 * Map the real client dataspace into the scene through
	 * sc_view_bind_surfaces (pixel plane plus alpha and input-mask
	 * planes of an 'alpha' buffer) — on every buffer call, for every
	 * view of this session (buffer hand-off may swap the dataspace
	 * under existing views; abi.h rebind contract).
	 */
	_bind_all_views();
	return Buffer_result::OK;
}


void Gui_session_component::_forward_scene_focus()
{
	for (unsigned i = 0; i < MAX_VIEWS; i++) {
		if (_views[i].used) {
			_pipeline.focus_forward(_handle, _views[i].id);
			return;
		}
	}

	Genode::warning("focus forwarding: session '", label(),
	                "' has no view to focus");
}


void Gui_session_component::focus(Gui::Session_capability focused)
{
	if (!focused.valid()) {
		Genode::warning("not implemented: focus reset (no G2 focus policy)");
		return;
	}

	/*
	 * Resolve the session capability to its server-side session object
	 * (the public 'Object_pool::apply' seam, as upstream nitpicker's
	 * 'Gui_session::focus') and forward the input focus to that
	 * session.
	 */
	Gui_session_component *target = nullptr;

	_env.ep().rpc_ep().apply(focused,
		[&] (Gui_session_component *session) { target = session; });

	if (!target) {
		Genode::warning("focus: unknown session capability");
		return;
	}

	target->_forward_scene_focus();
}


/*******************************
 ** Info_rom                  **
 *******************************/

Gui_session_component::Info_rom::Info_rom(Gui_session_component &session)
:
	Genode::Dynamic_rom_session::Xml_producer("panorama"),
	Genode::Dynamic_rom_session(session._env.ep(), session._env.ram(),
	                            session._env.rm(), *this),
	_session(session)
{ }


void Gui_session_component::Info_rom::produce_xml(Genode::Xml_generator &xml)
{
	/*
	 * Root 'panorama' node: xpos/ypos/width/height attributes of the
	 * screen (public 'Rect::from_node' parsing surface of
	 * util/geometry.h as consumed by 'Gui::Connection::panorama').
	 */
	xml.attribute("xpos",   0);
	xml.attribute("ypos",   0);
	xml.attribute("width",  _session._screen_w);
	xml.attribute("height", _session._screen_h);

	/*
	 * Capture sub-node: the captured screen area ('Gui::Connection::
	 * window' compounds the capture sub-nodes).
	 */
	xml.node("capture", [&] {
		xml.attribute("xpos",   0);
		xml.attribute("ypos",   0);
		xml.attribute("width",  _session._screen_w);
		xml.attribute("height", _session._screen_h);
	});
}


/*******************************
 ** Gui_root                 **
 *******************************/

Gui_root::Gui_root(Genode::Env &env, Genode::Heap &heap,
                   Genode::uint8_t *state, Genode::uint64_t /* state_size */,
                   Genode::Node const &config,
                   Input_pipeline &pipeline)
:
	Genode::Root_component<Gui_session_component>(env.ep(), heap),
	_env(env), _state(state), _pipeline(pipeline),
	_screen_w(config.attribute_value("width",  1024)),
	_screen_h(config.attribute_value("height", 768))
{ }


Gui_root::Create_result
Gui_root::_create_session(char const *args, Genode::Affinity const &)
{
	Genode::Session::Label     const label     = Genode::label_from_args(args);
	Genode::Session::Resources const resources =
		Genode::session_resources_from_args(args);

	/*
	 * One scene session per Gui session ('sc_session_open'). A failed
	 * open (scene capacity) refuses the session before any object is
	 * allocated.
	 */
	Genode::int32_t const handle = sc_session_open(_state, SC_SESSION_GUI);

	if (handle < 0) {
		Genode::warning("scene session capacity exhausted, "
		                "refusing Gui session '", label, "'");
		return Genode::Session_error::DENIED;
	}

	/*
	 * On successful creation the scene handle is owned (and closed) by
	 * the session object; if the object cannot be allocated the handle
	 * is closed again right here.
	 */
	return _alloc_obj(_env, _state, _pipeline, handle, resources, label,
	                  _screen_w, _screen_h)
		.template convert<Create_result>(
			[&] (Gui_session_component &session) -> Gui_session_component & {
				return session; },
			[&] (Genode::Session_error error) -> Genode::Session_error {
				sc_session_close(_state, handle);
				return error;
			});
}


void Gui_root::_upgrade_session(Gui_session_component &session, char const *args)
{
	session.upgrade(Genode::ram_quota_from_args(args));
	session.upgrade(Genode::cap_quota_from_args(args));
}


void Gui_root::_destroy_session(Gui_session_component &session)
{
	Genode::destroy(md_alloc(), &session);
}


void Gui_root::announce()
{
	_env.parent().announce(_env.ep().manage(*this));
}
