/* SPDX-License-Identifier: Apache-2.0
 *
 * compositor-input-probe — G2 input probe client for Sponge OS.
 *
 * The probe drives the G2 input battery against whichever Tier-0 the
 * run scenario selects (upstream nitpicker or the native Sponge
 * compositor, run/compositor.inc) and observes only PUBLIC Genode
 * surface: Gui sessions (views, buffers, per-session input), the Event
 * service (deterministic injection), Input connections, and the
 * 'pointer' report relayed through the report ROM.
 *
 * Scene (all geometry configurable, defaults below): two overlapping
 * top-level views on two separate Gui sessions.
 *
 *   bottom  opaque 'Pixel_rgb888' buffer, absolute Back   (200,150,500x400)
 *   top     alpha buffer [pixels][alpha][input-mask] with a rectangular
 *           hole (alpha=0, mask=0), absolute Front        (350,250,400x300)
 *   hole    top-relative (50,50,100x100) -> screen (400,300)-(499,399),
 *           fully inside the overlap so fall-through has a receiver
 *
 * The input mask is authoritative for hit testing on alpha views
 * (Framebuffer::Mode 'with_input_bytes': zero passes input through the
 * view to the lower stack, one consumes it). The probe never computes
 * hit results itself: every assertion observes the real delivery
 * outcome on the receiving session. This is not a second scene model
 * and no pixel contents are asserted.
 *
 * Phases and bounded PASS markers (each phase polls at most
 * POLL_ATTEMPTS x POLL_INTERVAL_MS and fails loudly with
 * 'g2-input: FAIL <phase>: ...'). Host clicks are driven through
 * closed-loop QMP pointer navigation: the probe emits 'QMP-TARGET
 * absmove' then 'QMP-TARGET move' to steer the pointer, confirms every
 * step against the 'pointer' ROM, and finishes with one 'QMP-TARGET
 * press' per click. The scenario rendezvouses the first target with
 * framebuffer and USB-tablet readiness before any dispatch. Unscripted
 * buttons or off-count press/release totals fail loudly. The three
 * host-click phases are:
 *
 *   1. click-top          closed-loop navigation to <top_pt> then one
 *                         press; the host QMP helper dispatches the PS/2
 *                         click at those coordinates. The click lands on
 *                         the top session (BTN_LEFT press+release seen
 *                         there, none on the bottom session).
 *                         -> 'g2-input: PASS click-top'
 *   2. mask-fallthrough   closed-loop navigation to <hole_pt> inside the
 *                         mask hole; the click falls through the top view
 *                         and lands on the bottom session.
 *                         -> 'g2-input: PASS mask-fallthrough'
 *   3. focus-transition   a third click back on the top view. The focus
 *                         event sequences across the three clicks must be
 *                         top=[Focus_enter, Focus_leave, Focus_enter] and
 *                         bottom=[Focus_enter, Focus_leave] in that order
 *                         (Focus_leave is delivered to the old holder
 *                         before Focus_enter reaches the new one).
 *                         -> 'g2-input: PASS focus-transition'
 *   4. event-inject       two deterministic batches injected through the
 *                         public 'Event::Connection': (abs-motion to the
 *                         top point + press + release) delivered to the
 *                         top session, and (abs-motion through the hole +
 *                         press + release) delivered to the bottom
 *                         session. Proves real Event injection reaches
 *                         the routing, including mask fall-through.
 *                         -> 'g2-input: PASS event-inject'
 *   5. pointer-report     the nitpicker 'pointer' report read through the
 *                         report ROM must parse and report xpos/ypos in
 *                         screen bounds and within 1 px of the last
 *                         injected motion target (abs injection is
 *                         exact). -> 'g2-input: PASS pointer-report'
 *
 * Finally the probe logs two FNV-1a-64 trace digests and, when every
 * phase passed, the overall marker:
 *
 *   g2-input: trace <fnv64>          full canonical trace (diagnostic)
 *   g2-input: trace-inject <fnv64>   injected Event batches only (exact)
 *   g2-input: PASS
 *
 * Canonical event trace: every input event received on the routed
 * per-session input channels ('Gui::Connection::input', one 'Input::'
 * session per Gui session), serialized as the five fields
 * 'type/session/x/y/key' separated by '/' and newline-terminated,
 * ordered by per-session delivery order ('top' records first, then
 * 'bottom'). Cross-session interleaving is timing-dependent and is
 * therefore deliberately excluded from the canonical order. x/y carry
 * the delivered motion coordinates (relative-motion deltas for
 * REL_MOTION), key carries the decimal keycode for press/release and -1
 * otherwise.
 *
 * 'trace-inject' is the exact native-vs-upstream differential: it hashes
 * only the deterministic injected Event batches (records from the phase-4
 * injection start markers onward) and matches exactly across both
 * Tier-0s (0xc50006d9e1676755). The full 'trace' digest is diagnostic
 * only: it also covers the host-QMP-driven clicks and attach-time hover
 * establishment, so native and upstream are expected to diverge by a
 * documented attach-time hover-establishment gap (upstream adds one
 * abs_motion and one hover_leave before the otherwise shared records;
 * plan section 8.2). Do not treat the full digest as an exact-match
 * gate, and do not synthesize a center hover in native to force a
 * match.
 *
 * Per-session 'Input::Connection' witnesses: the probe additionally
 * opens one public 'Input::Connection' per session ('top', 'bottom')
 * and consumes them in the test/input style. These observe the
 * driver-side stream (host QMP clicks entering the chain; injected
 * batches are not visible there) and co-evidence the host dispatch with
 * a loud warning when a host click produced no driver-side press. They
 * are opened through 'Genode::Env::try_session' so that a topology
 * without an 'Input' service - the documented G2 chain is
 * driver/ps2 -> server/event_filter -> Tier-0 'Event' root - degrades
 * to one warning instead of a startup failure. The routed per-session
 * events (the ones that carry the routing verdicts and the canonical
 * trace) are received on the Gui session input channels.
 *
 * Routing contract for the G2 scenario (run/sponge-compositor-input.run):
 *
 *   Gui   (labels "top", "bottom")  -> Tier-0
 *   Event (label  <probe>)          -> Tier-0 (injection entry)
 *   Input (labels "top", "bottom")  -> driver-side input source, optional
 *   ROM   "pointer"                 -> report_rom (Tier-0 'pointer' report)
 *
 * Tier-0 configuration requirements (documented so the scenario can
 * provide them; violations fail loudly instead of silently):
 *
 *   - domains with 'focus: click' (click-driven focus transitions) and
 *     'hover: always' or a single shared domain, 'content: client',
 *     'label: no' (no view frames that would skew coordinates)
 *   - the 'pointer' reporter enabled ('<report ... pointer="yes">')
 *
 * Explicit limits (not claimed, not tested): keyboard and global-key
 * sequences, touch, WM/DE integration, and pointer-shape reporting.
 * Only BTN_LEFT mouse clicks and absolute motion are exercised.
 *
 * Example configuration (every attribute optional):
 *
 *   <probe width="1024" height="768"
 *          top_x="350" top_y="250" top_w="400" top_h="300"
 *          bottom_x="200" bottom_y="150" bottom_w="500" bottom_h="400"
 *          hole_x="50" hole_y="50" hole_w="100" hole_h="100"
 *          top_color="0x3366cc" top_border="0xffffff"
 *          bottom_color="0xcc3333" bottom_border="0x000000"
 *          trace_dump="yes"/>
 *
 * Diagnostic trace dump (bounded, test-only, optional and disabled in
 * normal scenario runs): 'trace_dump="yes"' on the <probe> node makes
 * the probe log every canonical trace record as
 * 'g2-input: record <type>/<session>/<x>/<y>/<key>' in exactly the
 * order and serialization the digests hash, so native-vs-upstream
 * record diffs can be read off the serial log line by line. The dump is
 * capped by MAX_RECORDS per session (same bound as the digests) and
 * never feeds back into the digests or the phase assertions. It stays
 * off unless the scenario explicitly sets it, so ordinary runs emit no
 * per-record lines.
 */

#include <base/component.h>
#include <base/log.h>
#include <base/connection.h>
#include <base/attached_rom_dataspace.h>
#include <base/attached_dataspace.h>
#include <base/node.h>
#include <util/reconstructible.h>
#include <gui_session/connection.h>
#include <input_session/connection.h>
#include <input/event.h>
#include <event_session/connection.h>
#include <timer_session/connection.h>
#include <os/pixel_rgb888.h>


namespace G2 {

	using namespace Genode;

	using Pixel = Pixel_rgb888;

	enum {

		/* bounded polling (loud failure on exhaustion) */
		POLL_ATTEMPTS     = 400,
		POLL_INTERVAL_MS  = 25,

		/* bounded canonical-trace storage (loud overflow failure) */
		MAX_RECORDS       = 4096,
		MAX_FOCUS_SEQ     = 16,
	};

	struct Settings;
	struct Trace_record;
	struct Fnv1a_64;
	struct Session_channel;
	struct Driver_witness;
	struct Probe;
}


/**
 * Scene geometry parsed from the '<probe .../>' configuration node
 */
struct G2::Settings
{
	/* screen bounds (pointer-report sanity) */
	unsigned width  { 1024 };
	unsigned height { 768 };

	/* top view: alpha buffer with input-mask hole */
	int      top_x { 350 }, top_y { 250 };
	unsigned top_w { 400 }, top_h { 300 };

	/* bottom view: opaque */
	int      bottom_x { 200 }, bottom_y { 150 };
	unsigned bottom_w { 500 }, bottom_h { 400 };

	/* input-mask hole, top-view relative */
	int      hole_x { 50 }, hole_y { 50 };
	unsigned hole_w { 100 }, hole_h { 100 };

	uint32_t top_color     { 0x3366cc };
	uint32_t top_border    { 0xffffff };
	uint32_t bottom_color  { 0xcc3333 };
	uint32_t bottom_border { 0x000000 };

	/* bounded diagnostic dump of the canonical trace (test-only) */
	bool trace_dump { false };

	Gui::Area top_area() const    { return { top_w, top_h }; }
	Gui::Area bottom_area() const { return { bottom_w, bottom_h }; }

	Gui::Rect top_rect() const {
		return { { top_x, top_y }, top_area() }; }

	Gui::Rect bottom_rect() const {
		return { { bottom_x, bottom_y }, bottom_area() }; }

	Gui::Rect hole_rect() const {
		return { { top_x + hole_x, top_y + hole_y }, { hole_w, hole_h } }; }

	/* solid point of the top view, clear of the hole and the border */
	Gui::Point top_point() const {
		return { top_x + (int)(3*top_w/4), top_y + (int)(top_h/2) }; }

	/* center of the mask hole (fall-through target) */
	Gui::Point hole_point() const {
		return { top_x + hole_x + (int)(hole_w/2),
		         top_y + hole_y + (int)(hole_h/2) }; }

	static uint32_t _color(Node const &node, char const *attr, uint32_t dflt)
	{
		return node.attribute_value(attr, dflt);
	}
};


/**
 * One canonical trace record: the five digest fields
 */
struct G2::Trace_record
{
	enum Type : uint8_t {
		INVALID, PRESS, RELEASE, REL_MOTION, ABS_MOTION, WHEEL,
		FOCUS_ENTER, FOCUS_LEAVE, HOVER_LEAVE, TOUCH, TOUCH_RELEASE,
		SEQ_NUMBER, AXIS
	};

	Type type { INVALID };
	int  x { 0 }, y { 0 }, key { -1 };
};


/**
 * FNV-1a-64 over the canonical trace bytes
 */
struct G2::Fnv1a_64
{
	uint64_t h { 0xcbf29ce484222325ULL };

	void add_byte(uint8_t b) { h ^= (uint64_t)b; h *= 0x100000001b3ULL; }

	void add_str(char const *s)
	{
		while (*s)
			add_byte((uint8_t)*s++);
	}

	void add_dec(int v)
	{
		char    buf[16];
		unsigned n = 0;
		unsigned u = (unsigned)((v < 0) ? -v : v);

		do {
			buf[n++] = (char)('0' + u % 10);
			u /= 10;
		} while (u);

		if (v < 0)
			add_byte('-');

		while (n)
			add_byte((uint8_t)buf[--n]);
	}
};


/**
 * One Gui session: bound pixel buffer, top-level view, and the
 * per-session input channel that receives the routed events
 */
struct G2::Session_channel : Noncopyable
{
	char const * const tag;

	Session_channel(Session_channel const &) = delete;
	Session_channel &operator= (Session_channel const &) = delete;

	Env &_env;

	bool const top;

	Gui::Connection gui;

	Constructible<Attached_dataspace> fb { };

	Constructible<Gui::Top_level_view> view { };

	/* canonical event trace (bounded) */
	Trace_record records[MAX_RECORDS] { };
	unsigned     record_count { 0 };
	bool         overflow     { false };

	/* per-phase assertion counters (reset between phases) */
	unsigned btn_presses  { 0 };
	unsigned btn_releases { 0 };
	unsigned abs_motions  { 0 };

	/* cumulative scripted-vocabulary guards (never reset) */
	unsigned btn_presses_total { 0 };
	unsigned foreign_presses   { 0 };
	unsigned foreign_releases  { 0 };
	int      foreign_key       { -1 };

	/* cumulative focus-transition sequence ('E' enter, 'L' leave) */
	char     focus_seq[MAX_FOCUS_SEQ] { };
	unsigned focus_count { 0 };

	Session_channel(Env &env, Settings const &s, bool top_session)
	:
		tag(top_session ? "top" : "bottom"),
		_env(env), top(top_session),
		gui(env, Session_label(tag))
	{
		Gui::Area const area = top ? s.top_area() : s.bottom_area();

		/* bind the virtual framebuffer: alpha view carries the mask */
		gui.buffer(Framebuffer::Mode { .area = area, .alpha = top });

		fb.construct(env.rm(), gui.framebuffer.dataspace());

		_paint(s, area);

		gui.framebuffer.refresh(Gui::Rect { { 0, 0 }, area });

		Gui::Point const at = top ? Gui::Point(s.top_x, s.top_y)
		                          : Gui::Point(s.bottom_x, s.bottom_y);

		view.construct(gui, Gui::Rect { at, area });

		/*
		 * Absolute Front/Back through the public command buffer: the
		 * z order is deterministic regardless of session start order.
		 */
		if (top)
			gui.enqueue<Gui::Session::Command::Front>(view->id());
		else
			gui.enqueue<Gui::Session::Command::Back>(view->id());

		gui.execute();
	}

	void reset_phase()
	{
		btn_presses = btn_releases = abs_motions = 0;
	}

	void drain()
	{
		gui.input.for_each_event([&] (Input::Event const &ev) {
			_consume(_from_event(ev)); });
	}

	void append_focus(char c)
	{
		if (focus_count < MAX_FOCUS_SEQ)
			focus_seq[focus_count++] = c;
	}

	private:

		static Pixel _pixel(uint32_t rgb)
		{
			return Pixel((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
		}

		/*
		 * Paint a real 4-byte pixel buffer with a 1 px contrasting
		 * border. The alpha view additionally fills its alpha and
		 * input-mask planes: the hole is transparent to both (alpha 0,
		 * mask 0 => input passes through to lower views), everything
		 * else is opaque and input-consuming (alpha 255, mask 1).
		 */
		void _paint(Settings const &s, Gui::Area area)
		{
			Pixel const fill   = _pixel(top ? s.top_color     : s.bottom_color);
			Pixel const border = _pixel(top ? s.top_border    : s.bottom_border);

			Pixel * const px = fb->local_addr<Pixel>();

			for (unsigned y = 0; y < area.h; y++)
				for (unsigned x = 0; x < area.w; x++) {

					bool const at_border = (x == 0) || (y == 0)
					                    || (x + 1 == area.w) || (y + 1 == area.h);

					px[y*area.w + x] = at_border ? border : fill;
				}

			if (!top)
				return;

			uint8_t * const alpha = (uint8_t *)(px + area.count());
			uint8_t * const mask  = alpha + area.count();

			for (unsigned y = 0; y < area.h; y++)
				for (unsigned x = 0; x < area.w; x++) {

					bool const in_hole =
						((int)x >= s.hole_x) &&
						((int)x <  s.hole_x + (int)s.hole_w) &&
						((int)y >= s.hole_y) &&
						((int)y <  s.hole_y + (int)s.hole_h);

					alpha[y*area.w + x] = in_hole ? 0 : 255;
					mask [y*area.w + x] = in_hole ? 0 : 1;
				}
		}

		static Trace_record _from_event(Input::Event const &ev)
		{
			Trace_record r { };

			if (ev.press()) {
				r.type = Trace_record::PRESS;
				ev.handle_press([&] (Input::Keycode k, Input::Codepoint) {
					r.key = (int)k; });
			}
			else if (ev.release()) {
				r.type = Trace_record::RELEASE;
				ev.handle_release([&] (Input::Keycode k) {
					r.key = (int)k; });
			}
			else if (ev.absolute_motion()) {
				r.type = Trace_record::ABS_MOTION;
				ev.handle_absolute_motion([&] (int x, int y) {
					r.x = x; r.y = y; });
			}
			else if (ev.relative_motion()) {
				r.type = Trace_record::REL_MOTION;
				ev.handle_relative_motion([&] (int x, int y) {
					r.x = x; r.y = y; });
			}
			else if (ev.wheel()) {
				r.type = Trace_record::WHEEL;
				ev.handle_wheel([&] (int x, int y) {
					r.x = x; r.y = y; });
			}
			else if (ev.focus_enter())        r.type = Trace_record::FOCUS_ENTER;
			else if (ev.focus_leave())        r.type = Trace_record::FOCUS_LEAVE;
			else if (ev.hover_leave())        r.type = Trace_record::HOVER_LEAVE;
			else if (ev.touch_release())      r.type = Trace_record::TOUCH_RELEASE;
			else if (ev.touch()) {
				r.type = Trace_record::TOUCH;
				ev.handle_touch([&] (Input::Touch_id, float x, float y) {
					r.x = (int)x; r.y = (int)y; });
			}
			else if (ev.seq_number()) {
				r.type = Trace_record::SEQ_NUMBER;
				ev.handle_seq_number([&] (Input::Seq_number s) {
					r.x = (int)s.value; });
			}
			else if (ev.axis()) {
				r.type = Trace_record::AXIS;
				ev.handle_axis([&] (Input::Axis::Id id, float value) {
					r.x = (int)(unsigned)id; r.y = (int)value; });
			}

			return r;
		}

		void _consume(Trace_record const &r)
		{
			if (record_count < MAX_RECORDS)
				records[record_count++] = r;
			else
				overflow = true;

			switch (r.type) {

		case Trace_record::PRESS:
			if (r.key == (int)Input::BTN_LEFT) {
				btn_presses++;
				btn_presses_total++;
			} else {
				foreign_presses++;
				foreign_key = r.key;
			}
			break;

		case Trace_record::RELEASE:
			if (r.key == (int)Input::BTN_LEFT)
				btn_releases++;
			else {
				foreign_releases++;
				foreign_key = r.key;
			}
			break;


			case Trace_record::ABS_MOTION:
				abs_motions++;
				break;

			case Trace_record::FOCUS_ENTER:
				append_focus('E');
				break;

			case Trace_record::FOCUS_LEAVE:
				append_focus('L');
				break;

			default:
				break;
			}
		}
};


/**
 * Driver-side 'Input::Connection' witnesses (one per session)
 *
 * Opened through 'Genode::Env::try_session' so that a topology without
 * a routed 'Input' service degrades to one loud warning instead of
 * blocking or failing at startup. When present they observe the
 * driver-side stream and co-evidence host-click dispatch.
 */
struct G2::Driver_witness
{
	Env &_env;

	Parent::Client _client { };

	Id_space<Parent::Client>::Element _elem { _client, _env.id_space() };

	bool available { false };

	Constructible<Input::Connection> top    { };
	Constructible<Input::Connection> bottom { };

	unsigned presses { 0 };

	Driver_witness(Env &env) : _env(env)
	{
		_env.try_session(Input::Session::service_name(), _elem.id(),
		                 Parent::Session_args("ram_quota=20480, cap_quota=4"),
		                 Affinity()).with_result(
			[&] (Session_capability cap) {

				(void)cap;

				/* close the probe request, then open the real witnesses */
				_env.close(_elem.id());

				top.construct(_env, Session_label("top"));
				bottom.construct(_env, Session_label("bottom"));

				available = true;

				log("g2-input: Input::Connection witnesses ready (top, bottom)");
			},
			[&] (Session_error e) {
				warning("g2-input: no Input service for the per-session "
				        "Input::Connection witnesses (error ", (int)e,
				        ") - driver-side witness disabled; routed "
				        "per-session events are unaffected");
			});
	}

	void reset_phase() { presses = 0; }

	void drain()
	{
		if (!available)
			return;

		auto count = [&] (Input::Connection &c) {
			c.for_each_event([&] (Input::Event const &ev) {
				if (ev.key_press(Input::BTN_LEFT))
					presses++; }); };

		count(*top);
		count(*bottom);
	}
};


struct G2::Probe
{
	Env &_env;

	Attached_rom_dataspace _config { _env, "config" };

	Settings const _settings { _parse_settings(_config.node()) };

	Timer::Connection _timer { _env };

	Session_channel top    { _env, _settings, true  };
	Session_channel bottom { _env, _settings, false };

	Driver_witness witness { _env };

	Event::Connection event { _env };

	Attached_rom_dataspace pointer_rom { _env, "pointer" };

	bool _all_ok { true };
	bool _pointer_materialized { false };
	unsigned _inject_top_start { 0 };
	unsigned _inject_bottom_start { 0 };

	static char const *_type_name(Trace_record::Type t)
	{
		switch (t) {
		case Trace_record::PRESS:        return "press";
		case Trace_record::RELEASE:      return "release";
		case Trace_record::REL_MOTION:   return "rel_motion";
		case Trace_record::ABS_MOTION:   return "abs_motion";
		case Trace_record::WHEEL:        return "wheel";
		case Trace_record::FOCUS_ENTER:  return "focus_enter";
		case Trace_record::FOCUS_LEAVE:  return "focus_leave";
		case Trace_record::HOVER_LEAVE:  return "hover_leave";
		case Trace_record::TOUCH:        return "touch";
		case Trace_record::TOUCH_RELEASE:return "touch_release";
		case Trace_record::SEQ_NUMBER:   return "seq_number";
		case Trace_record::AXIS:         return "axis";
		case Trace_record::INVALID:      break;
		}
		return "invalid";
	}

	static Settings _settings_from_node(Node const &node)
	{
		Settings s { };

		s.width  = node.attribute_value("width",  (unsigned)1024);
		s.height = node.attribute_value("height", (unsigned)768);

		s.top_x = node.attribute_value("top_x", 350);
		s.top_y = node.attribute_value("top_y", 250);
		s.top_w = node.attribute_value("top_w", (unsigned)400);
		s.top_h = node.attribute_value("top_h", (unsigned)300);

		s.bottom_x = node.attribute_value("bottom_x", 200);
		s.bottom_y = node.attribute_value("bottom_y", 150);
		s.bottom_w = node.attribute_value("bottom_w", (unsigned)500);
		s.bottom_h = node.attribute_value("bottom_h", (unsigned)400);

		s.hole_x = node.attribute_value("hole_x", 50);
		s.hole_y = node.attribute_value("hole_y", 50);
		s.hole_w = node.attribute_value("hole_w", (unsigned)100);
		s.hole_h = node.attribute_value("hole_h", (unsigned)100);

		s.top_color     = Settings::_color(node, "top_color",     0x3366cc);
		s.top_border    = Settings::_color(node, "top_border",    0xffffff);
		s.bottom_color  = Settings::_color(node, "bottom_color",  0xcc3333);
		s.bottom_border = Settings::_color(node, "bottom_border", 0x000000);

		s.trace_dump = node.attribute_value("trace_dump", false);

		return s;
	}

	static Settings _parse_settings(Node const &root)
	{
		Settings result { };
		bool found = false;

		root.for_each_sub_node("probe", [&] (Node const &node) {
			if (!found) {
				result = _settings_from_node(node);
				found  = true;
			}});

		return found ? result : _settings_from_node(root);
	}

	/* probe-local geometry sanity only (not a scene model) */
	bool _validate_geometry()
	{
		Settings const &s = _settings;
		bool ok = true;

		if (s.top_w == 0 || s.top_h == 0 || s.bottom_w == 0 ||
		    s.bottom_h == 0 || s.hole_w == 0 || s.hole_h == 0) {
			error("g2-input: FAIL config: zero-sized view or hole");
			ok = false;
		}

		Gui::Rect const overlap =
			Gui::Rect::intersect(s.top_rect(), s.bottom_rect());

		if (!overlap.valid()) {
			error("g2-input: FAIL config: top and bottom views do not overlap");
			ok = false;
		}

		if (!s.top_rect().contains(s.top_point())) {
			error("g2-input: FAIL config: top click point outside the top view");
			ok = false;
		}

		if (s.hole_rect().contains(s.top_point())) {
			error("g2-input: FAIL config: top click point inside the mask hole");
			ok = false;
		}

		if (!overlap.contains(s.top_point())) {
			error("g2-input: FAIL config: top click point outside the overlap");
			ok = false;
		}

		if (!s.hole_rect().contains(s.hole_point())) {
			error("g2-input: FAIL config: hole click point outside the hole");
			ok = false;
		}

		if (!overlap.contains(s.hole_point())) {
			error("g2-input: FAIL config: hole click point outside the overlap");
			ok = false;
		}

		return ok;
	}

	void _log_geometry()
	{
		Settings const &s = _settings;

		log("g2-input: ready screen=", s.width, "x", s.height,
		    " top=(", s.top_x, ",", s.top_y, " ", s.top_w, "x", s.top_h,
		    ") bottom=(", s.bottom_x, ",", s.bottom_y, " ",
		    s.bottom_w, "x", s.bottom_h,
		    ") hole=(", s.hole_x, ",", s.hole_y, " ", s.hole_w, "x", s.hole_h,
		    ") top_point=(", s.top_point().x, ",", s.top_point().y,
		    ") hole_point=(", s.hole_point().x, ",", s.hole_point().y, ")");
	}

	void _drain_all()
	{
		top.drain();
		bottom.drain();
		witness.drain();
	}

	bool _poll_until(auto const &pred)
	{
		for (unsigned i = 0; i < POLL_ATTEMPTS; i++) {
			_drain_all();
			if (pred())
				return true;
			_timer.msleep(POLL_INTERVAL_MS);
		}
		_drain_all();
		return pred();
	}

	void _fail(char const *phase, char const *detail)
	{
		error("g2-input: FAIL ", phase, ": ", detail);
		_all_ok = false;
	}

	void _note_witness(char const *phase)
	{
		if (witness.available && witness.presses == 0)
			warning("g2-input: WARNING ", phase, ": driver-side "
			        "Input::Connection witnesses saw no BTN_LEFT press");
	}

	bool _pointer_pos(long &x, long &y)
	{
		pointer_rom.update();
		if (!pointer_rom.valid())
			return false;

		Node const &node = pointer_rom.node();
		if (!node.has_attribute("xpos") || !node.has_attribute("ypos"))
			return false;

		x = node.attribute_value("xpos", -1L);
		y = node.attribute_value("ypos", -1L);
		return x >= 0 && y >= 0;
	}

	bool _wait_pointer_expected(long tx, long ty)
	{
		long px = -1, py = -1;
		unsigned settled = 0;

		for (unsigned i = 0; i < 3000; i++) {
			if (_pointer_pos(px, py) &&
			    px >= tx - 3 && px <= tx + 3 &&
			    py >= ty - 3 && py <= ty + 3) {
				if (++settled == 5)
					return true;
			} else {
				settled = 0;
			}
			_timer.msleep(20);
		}

		error("g2-input: FAIL qmp-navigation: pointer did not settle at (",
		      tx, ",", ty, "); last position (", px, ",", py, ")");
		_all_ok = false;
		return false;
	}

	bool _goto_pos(Gui::Point at)
	{
		long const tx = at.x, ty = at.y;

		if (!_pointer_materialized) {
			log("QMP-TARGET absmove 512 384");
			if (!_wait_pointer_expected((long)_settings.width - 1,
			                            (long)_settings.height - 1))
				return false;
			_pointer_materialized = true;
		}

		long px = -1, py = -1;
		for (unsigned i = 0; i < 30; i++) {
			if (!_pointer_pos(px, py)) {
				_fail("qmp-navigation", "pointer report is unavailable");
				return false;
			}

			long dx = tx - px, dy = ty - py;
			if (dx >= -3 && dx <= 3 && dy >= -3 && dy <= 3) {
				log("QMP-TARGET move 1 1");
				if (!_wait_pointer_expected(px + 1, py + 1))
					return false;
				log("QMP-TARGET move -1 -1");
				return _wait_pointer_expected(px, py);
			}

			dx = dx > 100 ? 100 : (dx < -100 ? -100 : dx);
			dy = dy > 100 ? 100 : (dy < -100 ? -100 : dy);

			long expected_x = px + dx, expected_y = py + dy;
			expected_x = expected_x < 0 ? 0
			           : (expected_x > (long)_settings.width - 1
			              ? (long)_settings.width - 1 : expected_x);
			expected_y = expected_y < 0 ? 0
			           : (expected_y > (long)_settings.height - 1
			              ? (long)_settings.height - 1 : expected_y);

			log("QMP-TARGET move ", dx, " ", dy);
			if (!_wait_pointer_expected(expected_x, expected_y))
				return false;
		}

		_fail("qmp-navigation", "pointer did not converge within 30 moves");
		return false;
	}

	bool _host_click(Gui::Point at)
	{
		if (!_goto_pos(at))
			return false;
		log("QMP-TARGET press ", at.x, " ", at.y);
		return true;
	}

	/* phase 1: host click on the top view lands on the top session */
	void _phase_click_top()
	{
		top.reset_phase();
		bottom.reset_phase();
		witness.reset_phase();

		Gui::Point const at = _settings.top_point();

		if (!_host_click(at))
			return;

		bool const landed = _poll_until([&] {
			return top.btn_presses >= 1 && top.btn_releases >= 1; });

		if (!landed) {
			_fail("click-top", "no BTN_LEFT press/release on the top session "
			                  "within the poll bound");
			return;
		}

		if (bottom.btn_presses != 0) {
			_fail("click-top", "bottom session saw a stray BTN_LEFT press");
			return;
		}

		_note_witness("click-top");
		log("g2-input: PASS click-top");
	}

	/* phase 2: host click in the mask hole falls through to the bottom */
	void _phase_mask_fallthrough()
	{
		top.reset_phase();
		bottom.reset_phase();
		witness.reset_phase();

		Gui::Point const at = _settings.hole_point();

		if (!_host_click(at))
			return;

		bool const landed = _poll_until([&] {
			return bottom.btn_presses >= 1 && bottom.btn_releases >= 1; });

		if (!landed) {
			_fail("mask-fallthrough", "click in the mask hole never reached "
			                         "the bottom session");
			return;
		}

		if (top.btn_presses != 0) {
			_fail("mask-fallthrough", "top session consumed the hole click "
			                         "(input mask did not pass it through)");
			return;
		}

		_note_witness("mask-fallthrough");
		log("g2-input: PASS mask-fallthrough");
	}

	/*
	 * phase 3: focus transitions across the three host clicks occur in
	 * the expected order. Top: enter (click 1), leave (click 2),
	 * enter (click 3); bottom: enter (click 2), leave (click 3).
	 * The leave reaches the old holder before the enter reaches the new
	 * one (each session's own sequence proves the step pairing).
	 */
	void _phase_focus_transition()
	{
		top.reset_phase();
		bottom.reset_phase();
		witness.reset_phase();

		Gui::Point const at = _settings.top_point();

		if (!_host_click(at))
			return;

		bool const landed = _poll_until([&] {
			return top.btn_presses >= 1 && top.btn_releases >= 1; });

		if (!landed) {
			_fail("focus-transition", "third click never landed on the "
			                         "top session");
			return;
		}

		static char const top_expected[]    = { 'E', 'L', 'E' };
		static char const bottom_expected[] = { 'E', 'L' };

		auto seq_ok = [&] (Session_channel const &c,
		                   char const *expected, unsigned n) {
			if (c.focus_count != n)
				return false;
			for (unsigned i = 0; i < n; i++)
				if (c.focus_seq[i] != expected[i])
					return false;
			return true;
		};

		if (!seq_ok(top, top_expected, 3) || !seq_ok(bottom, bottom_expected, 2)) {

			struct Seq_print
			{
				Session_channel const &c;

				void print(Genode::Output &out) const {
					for (unsigned i = 0; i < c.focus_count; i++)
						Genode::print(out, c.focus_seq[i]); }
			};

			error("g2-input: FAIL focus-transition: unexpected focus order "
			      "top=", Seq_print { top }, " bottom=", Seq_print { bottom },
			      " (expected top=ELE bottom=EL)");
			_all_ok = false;
			return;
		}

		log("g2-input: PASS focus-transition");
	}

	/* phase 4: deterministic Event::Connection batches are delivered */
	void _phase_event_inject()
	{
		using Batch = Event::Session_client::Batch;

		top.reset_phase();
		bottom.reset_phase();

		_inject_top_start = top.record_count;
		_inject_bottom_start = bottom.record_count;

		Gui::Point const t = _settings.top_point();

		event.with_batch([&] (Batch &batch) {
			batch.submit(Input::Event(Input::Absolute_motion { t.x, t.y }));
			batch.submit(Input::Event(Input::Press   { Input::BTN_LEFT }));
			batch.submit(Input::Event(Input::Release { Input::BTN_LEFT }));
		});

		bool const top_done = _poll_until([&] {
			return top.btn_presses >= 1 && top.btn_releases >= 1; });

		if (!top_done) {
			_fail("event-inject", "injected top-view batch was not delivered "
			                     "to the top session");
			return;
		}

		if (top.abs_motions == 0) {
			_fail("event-inject", "injected Absolute_motion is missing on "
			                     "the top session");
			return;
		}

		if (bottom.btn_presses != 0) {
			_fail("event-inject", "bottom session saw the top-view batch");
			return;
		}

		top.reset_phase();
		bottom.reset_phase();

		Gui::Point const h = _settings.hole_point();

		event.with_batch([&] (Batch &batch) {
			batch.submit(Input::Event(Input::Absolute_motion { h.x, h.y }));
			batch.submit(Input::Event(Input::Press   { Input::BTN_LEFT }));
			batch.submit(Input::Event(Input::Release { Input::BTN_LEFT }));
		});

		bool const bottom_done = _poll_until([&] {
			return bottom.btn_presses >= 1 && bottom.btn_releases >= 1; });

		if (!bottom_done) {
			_fail("event-inject", "injected mask-hole batch did not fall "
			                     "through to the bottom session");
			return;
		}

		if (bottom.abs_motions == 0) {
			_fail("event-inject", "injected Absolute_motion is missing on "
			                     "the bottom session");
			return;
		}

		if (top.btn_presses != 0) {
			_fail("event-inject", "top session consumed the mask-hole batch");
			return;
		}

		log("g2-input: PASS event-inject");
	}

	/* phase 5: the pointer report is sane after the injected motion */
	void _phase_pointer_report()
	{
		Gui::Point const h = _settings.hole_point();

		int  px = -1, py = -1;
		bool parsed = false;

		bool const updated = _poll_until([&] {

			pointer_rom.update();

			if (!pointer_rom.valid())
				return false;

			Node const node(Const_byte_range_ptr(
				pointer_rom.local_addr<char>(), pointer_rom.size()));

			if (!node.has_attribute("xpos") || !node.has_attribute("ypos"))
				return false;

			px = node.attribute_value("xpos", -1);
			py = node.attribute_value("ypos", -1);

			parsed = true;
			return true;
		});

		if (!updated || !parsed) {
			_fail("pointer-report", "no parseable 'pointer' report within "
			                       "the poll bound");
			return;
		}

		if (px < 0 || py < 0 ||
		    px >= (int)_settings.width || py >= (int)_settings.height) {
			error("g2-input: FAIL pointer-report: xpos=", px, " ypos=", py,
			      " outside the screen (", _settings.width, "x",
			      _settings.height, ")");
			_all_ok = false;
			return;
		}

		/* the last injected motion targeted the hole point exactly */
		int dx = px - h.x, dy = py - h.y;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;

		if (dx > 1 || dy > 1) {
			error("g2-input: FAIL pointer-report: xpos=", px, " ypos=", py,
			      " not within 1 px of the injected target (", h.x, ",",
			      h.y, ")");
			_all_ok = false;
			return;
		}

		log("g2-input: pointer-report xpos=", px, " ypos=", py);
		log("g2-input: PASS pointer-report");
	}

	/*
	 * Canonical trace digest: per-session delivery order, 'top'
	 * records first, then 'bottom'; each record serialized as
	 * 'type/session/x/y/key\n' (see the file header).
	 */
	void _emit_digest()
	{
		if (top.foreign_presses || top.foreign_releases ||
		    bottom.foreign_presses || bottom.foreign_releases) {
			error("g2-input: FAIL button-vocabulary: unscripted button key=",
			      top.foreign_key != -1 ? top.foreign_key : bottom.foreign_key,
			      " top=(", top.foreign_presses, ",", top.foreign_releases,
			      ") bottom=(", bottom.foreign_presses, ",",
			      bottom.foreign_releases, ")");
			_all_ok = false;
		}

		if (top.btn_presses_total != 3 || bottom.btn_presses_total != 2) {
			error("g2-input: FAIL button-count: BTN_LEFT presses top=",
			      top.btn_presses_total, " bottom=", bottom.btn_presses_total,
			      " (expected top=3 bottom=2)");
			_all_ok = false;
		}

		if (top.overflow || bottom.overflow) {
			_fail("trace-overflow", "canonical trace exceeded MAX_RECORDS "
			                       "records per session");
			return;
		}

		auto hash_channel = [&] (Fnv1a_64 &digest, Session_channel const &c,
		                         unsigned first, bool dump) {
			for (unsigned i = first; i < c.record_count; i++) {

				Trace_record const &r = c.records[i];

				digest.add_str(_type_name(r.type));
				digest.add_byte('/');
				digest.add_str(c.tag);
				digest.add_byte('/');
				digest.add_dec(r.x);
				digest.add_byte('/');
				digest.add_dec(r.y);
				digest.add_byte('/');
				digest.add_dec(r.key);
				digest.add_byte('\n');

				if (dump)
					log("g2-input: record ", _type_name(r.type), "/",
					    c.tag, "/", r.x, "/", r.y, "/", r.key);
			}
		};

		Fnv1a_64 full, inject;
		hash_channel(full, top, 0, _settings.trace_dump);
		hash_channel(full, bottom, 0, _settings.trace_dump);
		hash_channel(inject, top, _inject_top_start, false);
		hash_channel(inject, bottom, _inject_bottom_start, false);

		log("g2-input: trace ", Hex(full.h));
		log("g2-input: trace-inject ", Hex(inject.h));
	}

	Probe(Env &env) : _env(env)
	{
		_log_geometry();

		if (!_validate_geometry()) {
			_all_ok = false;
		} else {

			_phase_click_top();
			_phase_mask_fallthrough();
			_phase_focus_transition();
			_phase_event_inject();
			_phase_pointer_report();
		}

		_emit_digest();

		if (_all_ok)
			log("g2-input: PASS");
		else
			error("g2-input: FAIL");

		/* stay alive with Timer sleeps so the scenario can collect output */
		for (;;)
			_timer.msleep(1000);
	}
};


void Component::construct(Genode::Env &env)
{
	static G2::Probe probe(env);
}
