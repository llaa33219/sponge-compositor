/* SPDX-License-Identifier: Apache-2.0
 *
 * input_pipeline.cc — C++ input pipeline and real Event delivery for
 * the Sponge compositor. See input_pipeline.h for the contract.
 *
 * Every input event is flattened into the scalar form of 'sc_input_step'
 * (abi.h), reduced by the Mojo core, and every resulting delivery tuple
 * is dispatched to the registered per-session 'Input::Session_component'
 * in FIFO order. The pipeline owns no scene state and performs no hit
 * testing: the Mojo core is the sole routing authority (D16C.10).
 */

#include "input_pipeline.h"

#include <util/xml_generator.h>
#include <util/color.h>

using namespace Sponge;


/*
 * One delivery-tuple dispatch buffer. The Mojo delivery queue holds at
 * most CAP_TUPLES = 32 tuples and one input event produces at most 8
 * tuples (mojo/input.mojo MAX_STEP_TUPLES); the pipeline drains the
 * queue completely after every event, so one buffer of 32 tuples always
 * holds complete per-event tuple groups and groups never straddle two
 * dispatch calls.
 */
static constexpr Genode::int32_t DRAIN_TUPLES = 32;


/*
 * Flatten one 'Input::Event' into the 'sc_input_step' scalars of abi.h
 * (ev_type, a, b). Returns true when the event should be passed to
 * 'sc_input_step'. 'unsupported' is set for G2 input corners that the
 * ABI accepts with no state change and no delivery claimed (touch,
 * axis, invalid keys) and for events with no flattened representation
 * (invalid events), which are skipped entirely.
 */
static bool flatten_event(Input::Event const &ev,
                          Genode::int32_t &ev_type,
                          Genode::int32_t &a,
                          Genode::int32_t &b,
                          bool &unsupported)
{
	a = 0;
	b = 0;

	if (ev.press()) {
		bool seen = false;
		ev.handle_press([&] (Input::Keycode key, Input::Codepoint codepoint) {
			ev_type = SC_EV_PRESS;
			a = (Genode::int32_t)key;
			b = (Genode::int32_t)codepoint.value;
			seen = true; });
		if (!seen) {
			unsupported = true;
			return false;
		}
		return true;
	}

	if (ev.release()) {
		bool seen = false;
		ev.handle_release([&] (Input::Keycode key) {
			ev_type = SC_EV_RELEASE;
			a = (Genode::int32_t)key;
			seen = true; });
		if (!seen) {
			unsupported = true;
			return false;
		}
		return true;
	}

	if (ev.relative_motion()) {
		ev_type = SC_EV_REL_MOTION;
		ev.handle_relative_motion([&] (int x, int y) { a = x; b = y; });
		return true;
	}

	if (ev.absolute_motion()) {
		ev_type = SC_EV_ABS_MOTION;
		ev.handle_absolute_motion([&] (int x, int y) { a = x; b = y; });
		return true;
	}

	if (ev.wheel()) {
		ev_type = SC_EV_WHEEL;
		ev.handle_wheel([&] (int x, int y) { a = x; b = y; });
		return true;
	}

	if (ev.seq_number()) {
		ev_type = SC_EV_SEQ_NUMBER;
		ev.handle_seq_number([&] (Input::Seq_number s) {
			a = (Genode::int32_t)s.value; });
		return true;
	}

	if (ev.focus_enter()) { ev_type = SC_EV_FOCUS_ENTER; return true; }
	if (ev.focus_leave()) { ev_type = SC_EV_FOCUS_LEAVE; return true; }
	if (ev.hover_leave()) { ev_type = SC_EV_HOVER_LEAVE; return true; }

	/*
	 * G2 input corners (abi.h): touch, touch-release, and axis events
	 * are accepted by 'sc_input_step' with no state change and no
	 * delivery claimed. They are still passed through so the acceptance
	 * is the ABI's documented behavior, not a silent drop here.
	 */
	unsupported = true;

	if (ev.touch())         { ev_type = SC_EV_TOUCH;         return true; }
	if (ev.touch_release()) { ev_type = SC_EV_TOUCH_RELEASE; return true; }
	if (ev.axis())          { ev_type = SC_EV_AXIS;          return true; }

	/* invalid event: no flattened representation at all */
	return false;
}


/*
 * Rebuild the delivered 'Input::Event' of one flattened delivery tuple
 * (tuple words [1..5]: type/x/y/key/codepoint). Returns an invalid
 * event for tuple types the G2 reducer never emits.
 */
static Input::Event event_from_tuple(Genode::int32_t type,
                                     Genode::int32_t x,
                                     Genode::int32_t y,
                                     Genode::int32_t key,
                                     Genode::int32_t codepoint)
{
	switch (type) {

	case SC_EV_PRESS:
		return Input::Event(Input::Press_char {
			(Input::Keycode)key,
			Input::Codepoint { (Genode::uint32_t)codepoint } });

	case SC_EV_RELEASE:
		return Input::Event(Input::Release { (Input::Keycode)key });

	case SC_EV_REL_MOTION:
		return Input::Event(Input::Relative_motion { x, y });

	case SC_EV_ABS_MOTION:
		return Input::Event(Input::Absolute_motion { x, y });

	case SC_EV_WHEEL:
		return Input::Event(Input::Wheel { x, y });

	case SC_EV_FOCUS_ENTER:
		return Input::Event(Input::Focus_enter());

	case SC_EV_FOCUS_LEAVE:
		return Input::Event(Input::Focus_leave());

	case SC_EV_HOVER_LEAVE:
		return Input::Event(Input::Hover_leave());

	case SC_EV_SEQ_NUMBER:
		return Input::Event(Input::Seq_number { (unsigned)x });

	default:
		return Input::Event();
	}
}


/****************************
 ** Input_pipeline::Driver_input **
 ****************************/

void Input_pipeline::Driver_input::_handle()
{
	Genode::size_t const max_events = _ev_ds.size() / sizeof(Input::Event);

	int const flushed = _connection.flush();

	Genode::size_t const n = (flushed <= 0) ? 0 : (Genode::size_t)flushed;

	unsigned const count = (unsigned)(n < max_events ? n : max_events);

	if (count)
		_pipeline.submit_batch(_ev_ds.local_addr<Input::Event>(), count);
}


/***************************
 ** Input_pipeline methods **
 ***************************/

Input_pipeline::Input_pipeline(Genode::Env           &env,
                               Genode::uint8_t       *state,
                               Genode::uint64_t       state_size,
                               Genode::Node const     &config)
:
	_env(env), _state(state), _state_size(state_size)
{
	if (!_state_usable())
		Genode::warning("input_pipeline: invalid scene state block "
		                "(state_size=", state_size,
		                "); input cannot reach the scene");

	/*
	 * '<report pointer="yes" hover="yes"/>', upstream nitpicker
	 * 'configure_reporter' shape: one 'report' node whose attribute
	 * names are the report names.
	 */
	auto configure_reporter = [&] (Genode::Reporter &reporter) {
		config.with_sub_node("report",
			[&] (Genode::Node const &node) {
				reporter.enabled(
					node.attribute_value(reporter.name().string(), false));
			},
			[&] {
				reporter.enabled(false);
			});
	};

	configure_reporter(_pointer_reporter);
	configure_reporter(_hover_reporter);

	/* driver-side input back end ('request_input="yes"') */
	if (config.attribute_value("request_input", false))
		_driver_input.construct(*this, env);
}


Input_pipeline::Registered_session *
Input_pipeline::_lookup(Genode::int32_t handle)
{
	if (handle < 0 || handle >= (Genode::int32_t)MAX_SESSIONS)
		return nullptr;

	Registered_session &slot = _sessions[handle];

	if (!slot.used || slot.handle != handle)
		return nullptr;

	return &slot;
}


void Input_pipeline::register_session(Genode::int32_t handle,
                                      Input::Session_component *component,
                                      Genode::Session::Label const &label)
{
	if (handle < 0 || handle >= (Genode::int32_t)MAX_SESSIONS || !component) {
		Genode::warning("input_pipeline: bad session registration (handle=",
		                handle, ")");
		return;
	}

	_sessions[handle].used      = true;
	_sessions[handle].handle    = handle;
	_sessions[handle].component = component;
	_sessions[handle].label     = label;
}


void Input_pipeline::unregister_session(Genode::int32_t handle)
{
	Registered_session *const slot = _lookup(handle);

	if (!slot)
		return;

	slot->used      = false;
	slot->handle    = -1;
	slot->component = nullptr;
	slot->label     = Genode::Session::Label();

	if (_hovered_handle == handle)
		_hovered_handle = HOVER_NONE;
}


void Input_pipeline::_dispatch_tuples(Genode::int32_t const *buf,
                                      Genode::int32_t n,
                                      unsigned &lost,
                                      unsigned &unsupported)
{
	for (Genode::int32_t i = 0; i < n; i++) {

		Genode::int32_t const *const t = buf + i * SC_EVENT_TUPLE_INTS;

		Genode::int32_t const session   = t[0];
		Genode::int32_t const type      = t[1];
		Genode::int32_t const x         = t[2];
		Genode::int32_t const y         = t[3];
		Genode::int32_t const key       = t[4];
		Genode::int32_t const codepoint = t[5];
		Genode::int32_t const seq       = t[6];
		/* t[7] is the reserved word (0) */

		/*
		 * Report glue (input_pipeline.h): the delivery sequence
		 * number is the Mojo state's delivery counter (tuple word
		 * [6]); it feeds the hover report so consumers can
		 * order-check deliveries.
		 */
		_have_seq = true;
		_last_seq = seq;

		/*
		 * Report glue (input_pipeline.h): hovered-session mark and
		 * button count derived from the tuple stream. The mark only
		 * feeds the hover report; routing stays Mojo-owned.
		 */
		if (type == SC_EV_PRESS) {
			_key_cnt++;
		} else if (type == SC_EV_RELEASE) {
			if (_key_cnt > 0)
				_key_cnt--;
		} else if (type == SC_EV_HOVER_LEAVE) {
			if (_hovered_handle == session)
				_hovered_handle = HOVER_NONE;
		} else if (type == SC_EV_ABS_MOTION && _key_cnt == 0) {

			/*
			 * A motion tuple that is immediately followed by the
			 * press tuple of the same session is the reducer's
			 * pre-press position update (the press receiver is not
			 * the hovered view; mojo/input.mojo press_step) and does
			 * not mark the session hovered. All other motion tuples
			 * with no button held follow the hover update and mark
			 * their session hovered. Tuple groups never straddle
			 * dispatch calls (see DRAIN_TUPLES), so one tuple of
			 * lookahead is exact.
			 */
			bool const pre_press_follows =
				(i + 1 < n) &&
				(buf[(i + 1) * SC_EVENT_TUPLE_INTS + 1] == SC_EV_PRESS) &&
				(buf[(i + 1) * SC_EVENT_TUPLE_INTS + 0] == session);

			if (!pre_press_follows)
				_hovered_handle = session;
		}

		Registered_session *const slot = _lookup(session);

		if (!slot || !slot->component) {
			lost++;
			continue;
		}

		Input::Event const ev =
			event_from_tuple(type, x, y, key, codepoint);

		if (!ev.valid()) {
			unsupported++;
			continue;
		}

		slot->component->submit(ev);
	}
}


void Input_pipeline::_drain_tuples(unsigned &lost, unsigned &unsupported)
{
	for (;;) {

		Genode::int32_t buf[SC_EVENT_TUPLE_INTS * DRAIN_TUPLES] { };

		Genode::int32_t const n = sc_input_step(
			_state, SC_EV_INVALID, 0, 0, 0, 0, buf, DRAIN_TUPLES);

		if (n <= 0)
			return;

		_dispatch_tuples(buf, n, lost, unsupported);

		if (n < DRAIN_TUPLES)
			return;
	}
}


void Input_pipeline::_submit_flattened(Genode::int32_t ev_type,
                                       Genode::int32_t a,
                                       Genode::int32_t b,
                                       Genode::int32_t c,
                                       Genode::int32_t d,
                                       unsigned &lost,
                                       unsigned &unsupported)
{
	for (;;) {


		Genode::int32_t buf[SC_EVENT_TUPLE_INTS * DRAIN_TUPLES] { };

		Genode::int32_t const n = sc_input_step(
			_state, ev_type, a, b, c, d, buf, DRAIN_TUPLES);

		if (n == -2) {
			/*
			 * Delivery queue cannot hold this event's worst-case
			 * tuples; the event was NOT consumed (abi.h). Drain the
			 * queued deliveries first (drains are never rejected),
			 * then retry - so no delivery is lost and none is
			 * reordered.
			 */
			_drain_tuples(lost, unsupported);
			continue;
		}

		if (n < 0) {
			Genode::warning("input_pipeline: sc_input_step rejected "
			                "event (", n, ") type ", ev_type);
			return;
		}

		_dispatch_tuples(buf, n, lost, unsupported);

		/* flush any queued remainder (FIFO) */
		_drain_tuples(lost, unsupported);
		return;
	}
}


void Input_pipeline::submit_batch(Input::Event const *events, unsigned count)
{
	if (!_state_usable())
		return;

	if (count && !events) {
		Genode::warning("input_pipeline: NULL event buffer (count=",
		                count, ")");
		return;
	}

	unsigned lost           = 0;
	unsigned unsupported_in = 0;
	unsigned unsupported    = 0;

	for (unsigned i = 0; i < count; i++) {

		Genode::int32_t ev_type = SC_EV_INVALID;
		Genode::int32_t a = 0, b = 0;

		bool unsupported_event = false;

		if (!flatten_event(events[i], ev_type, a, b, unsupported_event)) {
			if (unsupported_event)
				unsupported_in++;
			continue;
		}

		if (unsupported_event)
			unsupported_in++;

		_submit_flattened(ev_type, a, b, 0, 0, lost, unsupported);
	}

	_update_reports();

	if (unsupported_in)
		Genode::warning("input_pipeline: ", unsupported_in,
		                " input event(s) hit unsupported G2 corners "
		                "(touch/axis/invalid; accepted with no delivery)");

	if (lost)
		Genode::warning("input_pipeline: ", lost,
		                " delivery tuple(s) had no registered session");

	if (unsupported)
		Genode::warning("input_pipeline: ", unsupported,
		                " delivery tuple(s) had no event representation");
}


void Input_pipeline::focus_forward(Genode::int32_t session,
                                   Genode::uint64_t target)
{
	if (!_state_usable())
		return;

	unsigned lost        = 0;
	unsigned unsupported = 0;

	Genode::int32_t rc = sc_focus_forward(_state, session, target);

	if (rc == -2) {
		/* delivery queue full and nothing was changed: drain, retry */
		_drain_tuples(lost, unsupported);
		rc = sc_focus_forward(_state, session, target);
	}

	if (rc != 0)
		Genode::warning("input_pipeline: sc_focus_forward failed (", rc,
		                ") for session ", session, " view ", target);

	/*
	 * The queued leave-before-enter tuples are drained by the next
	 * sc_input_step call (abi.h). Drain now so the focus change is
	 * actually delivered - the pipeline never claims a delivery it did
	 * not make.
	 */
	_drain_tuples(lost, unsupported);

	_update_reports();

	if (lost)
		Genode::warning("input_pipeline: ", lost,
		                " delivery tuple(s) had no registered session");
}


void Input_pipeline::focus_clear()
{
	if (!_state_usable())
		return;

	unsigned lost = 0, unsupported = 0;
	Genode::int32_t rc = sc_focus_clear(_state);

	if (rc == -2) {
		_drain_tuples(lost, unsupported);
		rc = sc_focus_clear(_state);
	}

	if (rc != 0)
		Genode::warning("input_pipeline: sc_focus_clear failed (", rc, ")");

	_drain_tuples(lost, unsupported);
	_update_reports();
}


void Input_pipeline::_update_reports()
{
	bool const any_report = _pointer_reporter.enabled() ||
	                        _hover_reporter.enabled();

	Genode::int32_t px = 0, py = 0;

	if (any_report && sc_pointer_pos(_state, &px, &py) != 0)
		return;

	bool const pos_changed =
		!_have_reported_pos || px != _reported_x || py != _reported_y;

	bool const hover_changed =
		!_have_reported_hover || _reported_hover    != _hovered_handle ||
		_reported_have_seq != _have_seq || _reported_seq != _last_seq ||
		pos_changed;

	if (_pointer_reporter.enabled() && pos_changed) {

		(void)_pointer_reporter.generate_xml(
			[&] (Genode::Xml_generator &xml) {
				xml.attribute("xpos", px);
				xml.attribute("ypos", py);
			});
	}

	if (_hover_reporter.enabled() && hover_changed) {

		Genode::int32_t const hovered = _hovered_handle;

		Registered_session *const slot = _lookup(hovered);

		bool const have_seq = _have_seq;
		Genode::int32_t const seq = _last_seq;

		(void)_hover_reporter.generate_xml(
			[&] (Genode::Xml_generator &xml) {

				/*
				 * Attribute order as upstream nitpicker
				 * (User_state::report_hovered_view_owner +
				 * report_pointer_position). 'color' is the fixed
				 * color of the single implicit G2 domain
				 * (per-session/domain colors are not modeled in G2;
				 * FIXME: per-domain hover colors once a policy seam
				 * exists). 'active' marks a present hover (upstream
				 * gates 'active' on 500 ms motion activity, which
				 * needs a timer the G2 pipeline does not own).
				 */
				if (slot) {
					xml.attribute("label", slot->label);
					xml.attribute("color",
					              Genode::String<32>(Genode::Color::black()));
					xml.attribute("active", "yes");
				}

				if (have_seq)
					xml.attribute("seq_number", seq);

				xml.attribute("xpos", px);
				xml.attribute("ypos", py);
			});
	}

	_have_reported_pos   = true;
	_reported_x          = px;
	_reported_y          = py;
	_have_reported_hover = true;
	_reported_hover      = _hovered_handle;
	_reported_have_seq   = _have_seq;
	_reported_seq        = _last_seq;
}
