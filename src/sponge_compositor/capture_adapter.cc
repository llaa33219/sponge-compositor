/* SPDX-License-Identifier: Apache-2.0
 *
 * capture_adapter.cc — thin C++ 'Capture' session adapter (G1).
 *
 * See capture_adapter.h for the contract. Pixel content is composed by
 * the Mojo core ('sc_compose'); affected rects are fetched from the
 * Mojo damage list ('sc_damage_fetch') and translated to buffer
 * coordinates. Nothing here models the scene (D16C.10).
 */

#include "capture_adapter.h"
#include "gui_adapter.h"

#include <base/exception.h>
#include <base/signal.h>


/***************************************
 ** Capture_session_component methods **
 ***************************************/

Capture_session_component::Capture_session_component(
	Genode::Env     &env,
	Sponge::Gui_root &gui_root,
	Capture_root &capture_root,
	Genode::uint8_t *state,
	Genode::uint64_t state_size,
	Genode::int32_t  screen_w,
	Genode::int32_t  screen_h,
	Resources const &resources,
	Label     const &label)
:
	Genode::Session_object<Capture::Session>(env.ep(), resources, label),
	_env(env), _gui_root(gui_root), _capture_root(capture_root),
	_state(state), _state_size(state_size),
	_screen_w(screen_w), _screen_h(screen_h),
	_ram(env.ram(), _ram_quota_guard(), _cap_quota_guard())
{
	if (!_state_usable()) {
		Genode::warning("not implemented: Capture session without a usable "
		                "scene state block (state_size=", state_size,
		                "); 'capture_at' will not compose");
		return;
	}

	_mojo_handle = sc_session_open(_state, SC_SESSION_CAPTURE);

	if (_mojo_handle < 0) {
		Genode::warning("sc_session_open(SC_SESSION_CAPTURE) failed (",
		                _mojo_handle, ")");
		_mojo_handle = -1;
	}
}


Capture_session_component::~Capture_session_component()
{
	_close_mojo_session();
}


void Capture_session_component::_pending_add(Rect const &rect)
{
	if (!rect.valid())
		return;

	for (unsigned i = 0; i < Affected_rects::NUM_RECTS; i++) {
		if (!_pending.rects[i].valid()) {
			_pending.rects[i] = rect;
			return;
		}
	}

	Genode::int32_t x1 = rect.x1(), y1 = rect.y1();
	Genode::int32_t x2 = rect.x2(), y2 = rect.y2();

	for (unsigned i = 0; i < Affected_rects::NUM_RECTS; i++) {
		Rect const &current = _pending.rects[i];
		if (!current.valid())
			continue;
		if (current.x1() < x1) x1 = current.x1();
		if (current.y1() < y1) y1 = current.y1();
		if (current.x2() > x2) x2 = current.x2();
		if (current.y2() > y2) y2 = current.y2();
	}

	_pending.rects[0] = Rect {
		Point { x1, y1 },
		Area  { (unsigned)(x2 - x1), (unsigned)(y2 - y1) } };
	for (unsigned i = 1; i < Affected_rects::NUM_RECTS; i++)
		_pending.rects[i] = Rect { };
}


bool Capture_session_component::_damage_pending()
{
	if (!_state_usable())
		return false;

	/*
	 * Non-destructive probe (see the declaration in the header): with
	 * 'max_rects = 0' and a non-null rect buffer 'sc_damage_fetch'
	 * writes nothing and clears nothing; it returns -2 exactly while
	 * damage is pending and 0 when the damage list is empty.
	 */
	Genode::int32_t dummy { 0 };

	Genode::int32_t const rc = sc_damage_fetch(_state, &dummy, 0);

	if (rc == -2)
		return true;

	if (rc == 0)
		return false;

	Genode::warning("sc_damage_fetch (pending-damage probe) failed (", rc, ")");
	return false;
}


void Capture_session_component::_wakeup_if_needed()
{
	if (!_stopped || !_wakeup_sigh.valid())
		return;

	if (_damage_pending()) {
		Genode::Signal_transmitter(_wakeup_sigh).submit();
		_stopped = false;
		return;
	}

	/*
	 * Deferred to G2: the ABI has no damage-arrival hook, so damage
	 * added after this point cannot wake a stopped client. A client
	 * that relies on the wakeup must keep polling 'capture_at'.
	 */
	Genode::warning("not implemented before G2: wakeup on new damage after "
	                "'capture_stopped' (keep polling 'capture_at')");
}


void Capture_session_component::_close_mojo_session()
{
	if (_mojo_handle < 0 || !_state_usable())
		return;

	Genode::int32_t const rc = sc_session_close(_state, _mojo_handle);

	if (rc != 0)
		Genode::warning("sc_session_close failed (", rc, ")");

	_mojo_handle = -1;
}


Capture_session_component::Area
Capture_session_component::screen_size() const
{
	return Area { (unsigned)_screen_w, (unsigned)_screen_h };
}


void Capture_session_component::screen_size_sigh(
	Genode::Signal_context_capability sigh)
{
	_screen_size_sigh = sigh;

	/*
	 * The screen size is fixed at construction (the configured screen
	 * size), so no change can ever be announced.
	 */
	Genode::warning("not implemented before G2: screen-size change "
	                "notification (the screen size stays ",
	                _screen_w, "x", _screen_h, ")");
}


void Capture_session_component::wakeup_sigh(
	Genode::Signal_context_capability sigh)
{
	_wakeup_sigh = sigh;

	_wakeup_if_needed();
}


Capture_session_component::Buffer_result
Capture_session_component::buffer(Buffer_attr attr)
{
	_buffer_attr = { };

	/*
	 * Detach-before-realloc contract: the client detaches the previous
	 * dataspace before calling 'buffer' again. 'Constructible::construct'
	 * below destructs a previous allocation first, and an invalid
	 * 'attr.px' requests an explicit detach.
	 */
	if (!attr.px.valid()) {
		_buffer.destruct();
		return Buffer_result::OK;
	}

	if (attr.px.w != (unsigned)_screen_w ||
	    attr.px.h != (unsigned)_screen_h) {

		_buffer.destruct();

		Genode::warning("not implemented before G2: capture buffer must "
		                "match the screen size (requested ",
		                attr.px.w, "x", attr.px.h, ", screen ",
		                _screen_w, "x", _screen_h, ")");
		return Buffer_result::OK;
	}

	try {
		_buffer.construct(_ram, _env.rm(),
		                  Capture::Session::buffer_bytes(attr.px));
	}
	catch (Genode::Out_of_ram) {
		return Buffer_result::OUT_OF_RAM;
	}
	catch (Genode::Out_of_caps) {
		return Buffer_result::OUT_OF_CAPS;
	}

	_buffer_attr = attr;

	/*
	 * The fresh buffer has no content yet: request a full-screen
	 * repaint so the next 'capture_at' composes the whole scene.
	 */
	if (_state_usable()) {
		Genode::int32_t const rc =
			sc_damage_add(_state, 0, 0, _screen_w, _screen_h);

		if (rc != 0)
			Genode::warning("sc_damage_add failed (", rc, ")");
	}

	return Buffer_result::OK;
}


Genode::Dataspace_capability Capture_session_component::dataspace()
{
	if (_buffer.constructed())
		return _buffer->cap();

	return Genode::Dataspace_capability { };
}


Capture_session_component::Affected_rects
Capture_session_component::capture_at(Point pos)
{
	Affected_rects affected { };

	if (!_buffer.constructed() || !_state_usable())
		return affected;

	Genode::int32_t raw[Affected_rects::NUM_RECTS * 4] { };
	Genode::int32_t const count =
		sc_damage_fetch(_state, raw, (Genode::int32_t)Affected_rects::NUM_RECTS);

	if (count < 0) {
		Genode::warning("sc_damage_fetch failed (", count, ")");
		return affected;
	}

	/*
	 * Idle fast path: no fetched damage and no valid pending rect means
	 * there is no work. Return an empty result without re-adding pending
	 * damage, composing, or emitting a sync.
	 */
	if (count == 0) {
		bool pending_valid = false;

		for (unsigned i = 0; i < Affected_rects::NUM_RECTS; i++)
			if (_pending.rects[i].valid()) {
				pending_valid = true;
				break;
			}

		if (!pending_valid)
			return affected;
	}

	for (Genode::int32_t i = 0; i < count; i++) {
		if (raw[4*i + 2] <= 0 || raw[4*i + 3] <= 0)
			continue;
		_capture_root._add_pending(Rect {
			Point { raw[4*i + 0], raw[4*i + 1] },
			Area  { (unsigned)raw[4*i + 2], (unsigned)raw[4*i + 3] } });
	}

	for (unsigned i = 0; i < Affected_rects::NUM_RECTS; i++) {
		Rect const &r = _pending.rects[i];
		if (r.valid())
			sc_damage_add(_state, r.x1(), r.y1(), r.w(), r.h());
	}

	/* Buffer coordinate of the screen origin per the public Capture contract. */
	Point const anchor = pos - _buffer_attr.clipped_viewport().at;

	if (anchor != Point { 0, 0 } && !_warned_anchor) {
		_warned_anchor = true;
		Genode::warning("not implemented before G2: non-origin capture "
		                "point/viewport offset (affected rects are "
		                "translated; pixel content stays screen-aligned)");
	}

	Genode::uint32_t * const pixels =
		_buffer->local_addr<Genode::uint32_t>();

	Genode::int32_t const pitch =
		(Genode::int32_t)(_buffer_attr.px.w * 4U);

	Genode::int32_t const compose_rc = sc_compose(_state, pixels, pitch);

	if (compose_rc != 0) {
		Genode::warning("sc_compose failed (", compose_rc,
		                "); capture buffer not updated");
		return affected;
	}

	Rect const buffer_rect { Point { 0, 0 }, _buffer_attr.px };
	unsigned out = 0;

	for (unsigned i = 0;
	     i < Affected_rects::NUM_RECTS && out < Affected_rects::NUM_RECTS;
	     i++) {

		Rect const &screen_rect = _pending.rects[i];
		if (!screen_rect.valid())
			continue;

		Rect const translated { screen_rect.p1() - anchor, screen_rect.area };
		Rect const clipped = Rect::intersect(translated, buffer_rect);

		if (clipped.valid())
			affected.rects[out++] = clipped;
	}

	/* Sync only after real composition; idle polls must not broadcast. */
	if (out > 0)
		_gui_root.submit_sync();

	_pending = Affected_rects { };
	return affected;
}


void Capture_session_component::capture_stopped()
{
	_stopped = true;

	/*
	 * Dirty pixels may already be pending; the wakeup is scheduled
	 * immediately in that case (see '_wakeup_if_needed').
	 */
	_wakeup_if_needed();
}


/************************
 ** Capture_root methods **
 ************************/

Capture_root::Capture_root(Genode::Env     &env,
                           Genode::Heap    &heap,
                           Sponge::Gui_root &gui_root,
                           Genode::uint8_t *state,
                           Genode::uint64_t state_size,
                           Genode::int32_t  screen_w,
                           Genode::int32_t  screen_h)
:
	Genode::Root_component<Capture_session_component>(env.ep(), heap),
	_env(env), _gui_root(gui_root),
	_state(state), _state_size(state_size),
	_screen_w(screen_w > 0 ? screen_w : DEFAULT_SCREEN_W),
	_screen_h(screen_h > 0 ? screen_h : DEFAULT_SCREEN_H)
{
	if (screen_w <= 0 || screen_h <= 0)
		Genode::warning("capture_adapter: invalid screen size ",
		                screen_w, "x", screen_h, ", using ",
		                _screen_w, "x", _screen_h);

	if (_state == nullptr ||
	    _state_size < (Genode::uint64_t)SC_SCENE_STORAGE_BYTES)
		Genode::warning("capture_adapter: invalid scene state block "
		                "(state_size=", state_size,
		                "); Capture sessions will not compose");
}


void Capture_root::announce()
{
	_env.parent().announce(_env.ep().manage(*this));
}


void Capture_root::_add_pending(Rect const &rect)
{
	for (unsigned i = 0; i < SC_MAX_SESSIONS; i++)
		if (_sessions[i])
			_sessions[i]->add_pending(rect);
}


Capture_root::Create_result
Capture_root::_create_session(const char *args, Genode::Affinity const &)
{
	Capture_session_component *session = new (md_alloc())
		Capture_session_component(_env, _gui_root, *this, _state, _state_size,
		                          _screen_w, _screen_h,
		                          Genode::session_resources_from_args(args),
		                          Genode::session_label_from_args(args));

	for (unsigned i = 0; i < SC_MAX_SESSIONS; i++) {
		if (!_sessions[i]) {
			_sessions[i] = session;
			break;
		}
	}

	return *session;
}


void Capture_root::_upgrade_session(Capture_session_component &session,
                                    const char *args)
{
	session.upgrade(Genode::ram_quota_from_args(args));
	session.upgrade(Genode::cap_quota_from_args(args));
}


void Capture_root::_destroy_session(Capture_session_component &session)
{
	for (unsigned i = 0; i < SC_MAX_SESSIONS; i++)
		if (_sessions[i] == &session)
			_sessions[i] = nullptr;

	Genode::destroy(md_alloc(), &session);
}
