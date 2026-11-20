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


/*
 * 'label_prefix' selector match (upstream 'Node_label_score' semantics:
 * the label starts with the selector; an empty selector matches).
 */
static bool label_starts_with(Genode::Session::Label const &label,
                              Genode::String<160>  const &prefix)
{
	return Genode::strcmp(label.string(), prefix.string(),
	                      Genode::strlen(prefix.string())) == 0;
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
	Domain_rect           const &domain_rect)
:
	Genode::Session_object<Gui::Session>(env.ep(), resources, label),
	_env(env), _state(state), _pipeline(pipeline), _handle(scene_handle),
	_domain_rect(domain_rect),
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
	 * alpha and input-mask planes of an 'alpha' buffer, together
	 * with the buffer's own pixel dimensions (_fb.mode().area.w/h).
	 * The extent describes the bound buffer, not the view's current
	 * geometry, so composition and hit testing can tile it safely
	 * while a geometry change (e.g. maximize) awaits its new buffer.
	 * Non-alpha buffers pass absent planes and keep the view
	 * hit-testing opaque; an 'alpha' buffer consults its input-mask
	 * plane, where a zero mask byte falls through to the views below.
	 */
	Genode::int32_t const r = sc_view_bind_surfaces(
		_state, _handle, id, pixels, _fb.pitch(),
		(Genode::int32_t)_fb.mode().area.w,
		(Genode::int32_t)_fb.mode().area.h,
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

	_command_buffer.reset();
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
		_pipeline.focus_clear();
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


void Gui_session_component::submit_sync()
{
	_fb.submit_sync();
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
	 * session's domain rect (public 'Rect::from_node' parsing surface
	 * of util/geometry.h as consumed by 'Gui::Connection::panorama').
	 */
	Domain_rect const &rect = _session._domain_rect;

	xml.attribute("xpos",   rect.x);
	xml.attribute("ypos",   rect.y);
	xml.attribute("width",  rect.w);
	xml.attribute("height", rect.h);

	/*
	 * Capture sub-node: the captured screen area ('Gui::Connection::
	 * window' compounds the capture sub-nodes), clipped to the
	 * session's domain rect like upstream 'gen_capture_attr'.
	 */
	xml.node("capture", [&] {
		xml.attribute("xpos",   rect.x);
		xml.attribute("ypos",   rect.y);
		xml.attribute("width",  rect.w);
		xml.attribute("height", rect.h);
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
{
	_parse_domains(config);
	_parse_policies(config);
}


Gui_root::Domain const *
Gui_root::_lookup_domain(Domain::Name const &name) const
{
	for (unsigned i = 0; i < _num_domains; i++)
		if (_domains[i].used && _domains[i].name == name)
			return &_domains[i];
	return nullptr;
}


Gui_root::Domain::Origin
Gui_root::_parse_origin(Genode::Node const &domain)
{
	using Value = Genode::String<32>;
	Value const value = domain.attribute_value("origin", Value("top_left"));

	if (value == "top_left")     return Domain::Origin::TOP_LEFT;
	if (value == "top_right")    return Domain::Origin::TOP_RIGHT;
	if (value == "bottom_left")  return Domain::Origin::BOTTOM_LEFT;
	if (value == "bottom_right") return Domain::Origin::BOTTOM_RIGHT;
	if (value == "pointer")      return Domain::Origin::POINTER;

	Genode::warning("invalid value of origin attribute in <domain>");
	return Domain::Origin::BOTTOM_LEFT;
}


void Gui_root::_parse_domains(Genode::Node const &config)
{
	config.for_each_sub_node("domain", [&] (Genode::Node const &node) {

		Domain::Name const name = node.attribute_value("name", Domain::Name());

		if (!name.valid()) {
			Genode::error("no valid domain name specified");
			return;
		}

		if (_lookup_domain(name)) {
			Genode::error("domain name \"", name, "\" is not unique");
			return;
		}

		if (_num_domains >= MAX_DOMAINS) {
			Genode::warning("domain table full (", (unsigned)MAX_DOMAINS,
			                " entries), ignoring domain \"", name, "\"");
			return;
		}

		Domain &domain = _domains[_num_domains++];

		domain.used   = true;
		domain.name   = name;
		domain.origin = _parse_origin(node);
		domain.xpos   = node.attribute_value("xpos",   (Genode::int32_t)0);
		domain.ypos   = node.attribute_value("ypos",   (Genode::int32_t)0);
		domain.width  = node.attribute_value("width",  (Genode::int32_t)0);
		domain.height = node.attribute_value("height", (Genode::int32_t)0);
	});
}


void Gui_root::_parse_policies(Genode::Node const &config)
{
	config.for_each_sub_node("policy", [&] (Genode::Node const &node) {

		Policy policy { };

		policy.has_label = node.has_attribute("label");
		if (policy.has_label)
			policy.label = node.attribute_value("label", Policy::Selector());

		policy.has_label_prefix = node.has_attribute("label_prefix");
		if (policy.has_label_prefix)
			policy.label_prefix =
				node.attribute_value("label_prefix", Policy::Selector());

		policy.has_label_last = node.has_attribute("label_last");
		if (policy.has_label_last)
			policy.label_last =
				node.attribute_value("label_last", Policy::Selector());

		if (!policy.has_label && !policy.has_label_prefix &&
		    !policy.has_label_last) {
			Genode::error("policy without label, label_prefix, or "
			              "label_last attribute ignored");
			return;
		}

		if (!node.has_attribute("domain")) {
			Genode::error("policy for label \"",
			              policy.has_label        ? policy.label :
			              policy.has_label_prefix ? policy.label_prefix :
			                                        policy.label_last,
			              "\" lacks domain declaration");
			return;
		}

		policy.domain = node.attribute_value("domain", Domain::Name());

		if (!_lookup_domain(policy.domain)) {
			Genode::error("policy for label \"",
			              policy.has_label        ? policy.label :
			              policy.has_label_prefix ? policy.label_prefix :
			                                        policy.label_last,
			              "\" specifies nonexistent domain \"",
			              policy.domain, "\"");
			return;
		}

		if (_num_policies >= MAX_POLICIES) {
			Genode::warning("policy table full (", (unsigned)MAX_POLICIES,
			                " entries), ignoring policy for domain \"",
			                policy.domain, "\"");
			return;
		}

		policy.used = true;
		_policies[_num_policies++] = policy;
	});

	config.with_sub_node("default-policy",
		[&] (Genode::Node const &node) {

			if (!node.has_attribute("domain")) {
				Genode::error("default-policy lacks domain declaration");
				return;
			}

			Domain::Name const name =
				node.attribute_value("domain", Domain::Name());

			if (!_lookup_domain(name)) {
				Genode::error("default-policy specifies nonexistent "
				              "domain \"", name, "\"");
				return;
			}

			_default_policy_domain = name;
			_has_default_policy    = true;
		},
		[&] { });
}


Domain_rect Gui_root::_domain_rect_for(Genode::Session::Label const &label) const
{
	/*
	 * Policy match order: exact 'label', then 'label_prefix'
	 * (longest selector wins), then 'label_last', then
	 * '<default-policy>', else the full screen. A policy with
	 * several selectors requires all of them to match and ranks by
	 * its strongest selector.
	 */
	enum { TIER_NONE = 0, TIER_LAST = 1, TIER_PREFIX = 2, TIER_EXACT = 3 };

	Policy const *best = nullptr;
	unsigned      best_tier  = TIER_NONE;
	Genode::size_t best_score = 0;

	for (unsigned i = 0; i < _num_policies; i++) {

		Policy const &policy = _policies[i];
		if (!policy.used)
			continue;

		if (policy.has_label && !(label == policy.label))
			continue;
		if (policy.has_label_prefix &&
		    !label_starts_with(label, policy.label_prefix))
			continue;
		if (policy.has_label_last &&
		    !(label.last_element() == policy.label_last))
			continue;

		unsigned const tier =
			policy.has_label        ? TIER_EXACT :
			policy.has_label_prefix ? TIER_PREFIX : TIER_LAST;

		Genode::size_t const score =
			policy.has_label_prefix
				? Genode::strlen(policy.label_prefix.string()) : 0;

		if (tier > best_tier || (tier == best_tier && score > best_score)) {
			best       = &policy;
			best_tier  = tier;
			best_score = score;
		}
	}

	Domain const *domain = nullptr;

	if (best) {
		domain = _lookup_domain(best->domain);
		if (!domain)
			Genode::warning("policy specifies nonexistent domain \"",
			                best->domain, "\"");
	} else if (_has_default_policy) {
		domain = _lookup_domain(_default_policy_domain);
		if (!domain)
			Genode::warning("default-policy specifies nonexistent "
			                "domain \"", _default_policy_domain, "\"");
	}

	if (!domain)
		return { 0, 0, _screen_w, _screen_h };

	/*
	 * 'Domain_registry::Entry::screen_rect' semantics: positive
	 * width/height give the value, nonpositive values count back
	 * from the screen edge (max(0, screen + delta)). The panorama
	 * origin is (0, 0), so xpos/ypos shift the rect directly.
	 */
	auto aligned = [&] (Genode::int32_t limit, Genode::int32_t v) {
		if (v > 0)
			return v;
		Genode::int32_t const sum = limit + v;
		return (sum > 0) ? sum : (Genode::int32_t)0;
	};

	return { domain->xpos, domain->ypos,
	         aligned(_screen_w, domain->width),
	         aligned(_screen_h, domain->height) };
}


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
	Domain_rect const rect = _domain_rect_for(label);

	return _alloc_obj(_env, _state, _pipeline, handle, resources, label, rect)
		.template convert<Create_result>(
			[&] (Gui_session_component &session) -> Gui_session_component & {
				for (unsigned i = 0; i < SC_MAX_SESSIONS; i++) {
					if (!_sessions[i]) {
						_sessions[i] = &session;
						break;
					}
				}
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
	for (unsigned i = 0; i < SC_MAX_SESSIONS; i++)
		if (_sessions[i] == &session)
			_sessions[i] = nullptr;

	Genode::destroy(md_alloc(), &session);
}


void Gui_root::submit_sync()
{
	for (unsigned i = 0; i < SC_MAX_SESSIONS; i++)
		if (_sessions[i])
			_sessions[i]->submit_sync();
}


void Gui_root::announce()
{
	_env.parent().announce(_env.ep().manage(*this));
}
