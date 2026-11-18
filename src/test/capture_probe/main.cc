/* SPDX-License-Identifier: Apache-2.0
 *
 * compositor-capture-probe — G1 golden/differential capture probe for
 * Sponge OS.
 *
 * Two modes (config attribute 'mode'):
 *
 *   "golden"  Poll the real 'Capture::Connection::capture_at' output until
 *             it is byte-exact against the expected test-fixture raster,
 *             compute the FNV-1a-64 checksum, verify 'expect_checksum'
 *             when provided, and log:
 *               g1-capture: PASS checksum=0x...
 *             Failures log the first mismatch and the mismatch region.
 *
 *   "diff"    Open two Capture connections with the labels 'native' and
 *             'upstream', poll both real rasters and compare them against
 *             each other and against the expected test-fixture raster:
 *               g1-diff: MATCH checksum=0x...
 *             Failures log a loud mismatch per disagreeing pair.
 *
 * The expected test-fixture raster is pure oracle data for the G1 run
 * scenario (background #1e1e2e, back view B, front view A winning the
 * overlap). It is not a behavioral scene model: every compared raster is
 * read from a real Capture dataspace. Public Genode APIs only, see
 * genode/repos/os/src/test/capture/main.cc for the reference pattern.
 *
 * Example configuration:
 *
 *   <probe mode="golden" width="1024" height="768"
 *          expect_checksum="0x123456789abcdef0"/>
 */

#include <base/component.h>
#include <base/log.h>
#include <base/attached_rom_dataspace.h>
#include <base/attached_ram_dataspace.h>
#include <base/node.h>
#include <capture_session/connection.h>
#include <os/pixel_rgb888.h>
#include <timer_session/connection.h>


namespace G1 {

	using namespace Genode;

	using Pixel = Capture::Pixel;
	using Area  = Capture::Area;
	using Point = Capture::Point;
	using Rect  = Capture::Rect;

	/* default capture-buffer size of the G1 scenario */
	enum {
		FIXTURE_WIDTH  = 1024,
		FIXTURE_HEIGHT = 768,

		/* bounded polling */
		POLL_ATTEMPTS     = 500,
		POLL_INTERVAL_MS  = 20,
	};

	struct Settings;
	struct Raster_diff;
	struct Capture_target;
	struct Capture_probe;
}


/**
 * Settings parsed from the configuration node
 */
struct G1::Settings
{
	String<16> mode { "golden" };
	Area  area { FIXTURE_WIDTH, FIXTURE_HEIGHT };
	bool  has_expect_checksum { false };
	Genode::uint64_t expect_checksum { 0 };
};


/**
 * Result of a byte-exact raster comparison
 */
struct G1::Raster_diff
{
	bool found { false };

	Genode::size_t first_index { 0 };
	Pixel first_actual   { };
	Pixel first_expected { };

	/* bounding region of all mismatching pixels */
	Rect region { };

	Point first_at(Area area) const
	{
		return Point { (int)(first_index % area.w), (int)(first_index / area.w) };
	}

	static Raster_diff compare(Pixel const *actual, Pixel const *expected, Area area)
	{
		Raster_diff diff { };

		for (Genode::size_t i = 0; i < area.count(); i++) {

			if (actual[i].pixel == expected[i].pixel)
				continue;

			if (!diff.found) {
				diff.found         = true;
				diff.first_index   = i;
				diff.first_actual  = actual[i];
				diff.first_expected = expected[i];
			}

			Point const at { (int)(i % area.w), (int)(i / area.w) };
			Rect  const one { at, { 1, 1 } };

			diff.region = diff.region.valid() ? Rect::compound(diff.region, one)
			                                  : one;
		}

		return diff;
	}
};


/**
 * One real Capture session with its shared pixel buffer
 */
struct G1::Capture_target
{
	private:

		Env &_env;

		Capture::Connection _connection;

		bool const _buffer_init = false;

		Attached_dataspace _ds;

	public:

		Capture_target(Env &env, Capture::Connection::Label label, Area area)
		:
			_env(env), _connection(env, label),
			_buffer_init((_connection.buffer({ .px       = area,
			                                  .mm       = { },
			                                  .viewport = { { }, area } }), true)),
			_ds(env.rm(), _connection.dataspace())
		{ }

		/* update the shared buffer with the real composed content */
		void capture() { _connection.capture_at(Point(0, 0)); }

		Pixel *pixels() { return _ds.local_addr<Pixel>(); }
};


struct G1::Capture_probe
{
	private:

		Env &_env;

		Attached_rom_dataspace _config { _env, "config" };

		Settings const _settings { _parse_settings(_config.node()) };

		/* backing store of the expected test-fixture raster */
		Attached_ram_dataspace _expected_ds {
			_env.ram(), _env.rm(), _settings.area.count()*sizeof(Pixel) };

		Timer::Connection _timer { _env };

		/**
		 * Convert 0xRRGGBB value to a real 4-byte pixel
		 */
		static Pixel _pixel_from_rgb(Genode::uint32_t rgb)
		{
			return Pixel((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
		}

		static Settings _settings_from_node(Node const &node)
		{
			Settings settings { };

			settings.mode = node.attribute_value("mode", String<16>("golden"));

			settings.area = Area {
				node.attribute_value("width",  (unsigned)FIXTURE_WIDTH),
				node.attribute_value("height", (unsigned)FIXTURE_HEIGHT) };

			settings.has_expect_checksum = node.has_attribute("expect_checksum");
			settings.expect_checksum     =
				node.attribute_value("expect_checksum", 0ULL);

			if (settings.area.count() == 0)
				error("g1-capture: invalid or missing 'width'/'height' "
				      "config attributes");

			return settings;
		}

		static Settings _parse_settings(Node const &root)
		{
			Settings result { };
			bool found = false;

			root.for_each_sub_node("probe", [&] (Node const &node) {
				if (!found) {
					result = _settings_from_node(node);
					found  = true;
				}
			});

			return found ? result : _settings_from_node(root);
		}

		/**
		 * Paint a solid opaque rectangle with a 1 px contrasting border
		 */
		static void _paint_opaque_rect(Pixel *pixels, Area area, Rect rect,
		                               Pixel fill, Pixel border)
		{
			if (!rect.valid())
				return;

			Rect const clipped = Rect::intersect(rect, Rect { { }, area });

			for (int y = clipped.y1(); y <= clipped.y2(); y++)
				for (int x = clipped.x1(); x <= clipped.x2(); x++) {

					bool const at_border = (x == rect.x1()) || (y == rect.y1())
					                    || (x == rect.x2())
					                    || (y == rect.y2());

					pixels[(Genode::size_t)y*area.w + (Genode::size_t)x] =
						at_border ? border : fill;
				}
		}

		/**
		 * Generate the expected test-fixture raster (the oracle data of the
		 * G1 scenario; the only scene model in this probe):
		 *
		 *   background #1e1e2e
		 *   view B (300, 200, 400, 300) fill #cc3333, 1 px border #000000, Back
		 *   view A (100,  80, 400, 300) fill #3366cc, 1 px border #ffffff, Front
		 *
		 * Painted back-to-front so that front view A wins the overlap.
		 */
		static void _generate_expected(Pixel *pixels, Area area)
		{
			Pixel const background = _pixel_from_rgb(0x1e1e2e);

			for (Genode::size_t i = 0; i < area.count(); i++)
				pixels[i] = background;

			/* back view B */
			_paint_opaque_rect(pixels, area, Rect { { 300, 200 }, { 400, 300 } },
			                   _pixel_from_rgb(0xcc3333),
			                   _pixel_from_rgb(0x000000));

			/* front view A — wins the overlap */
			_paint_opaque_rect(pixels, area, Rect { { 100, 80 }, { 400, 300 } },
			                   _pixel_from_rgb(0x3366cc),
			                   _pixel_from_rgb(0xffffff));
		}

		/* FNV-1a-64 over the raw raster bytes */
		static Genode::uint64_t _fnv1a_64(void const *data,
		                                  Genode::size_t num_bytes)
		{
			Genode::uint64_t hash = 0xcbf29ce484222325ULL;

			Genode::uint8_t const *bytes =
				static_cast<Genode::uint8_t const *>(data);

			for (Genode::size_t i = 0; i < num_bytes; i++) {
				hash ^= (Genode::uint64_t)bytes[i];
				hash *= 0x100000001b3ULL;
			}
			return hash;
		}

		/* raw raster bytes of the expected test-fixture oracle */
		Pixel const *_expected() const
		{
			return _expected_ds.local_addr<Pixel>();
		}

		Genode::uint64_t _checksum() const
		{
			return _fnv1a_64(_expected(),
			                 _settings.area.count()*sizeof(Pixel));
		}

		bool _verify_checksum(char const *prefix, Genode::uint64_t checksum) const
		{
			if (!_settings.has_expect_checksum)
				return true;

			if (checksum == _settings.expect_checksum)
				return true;

			error(prefix, ": FAIL checksum mismatch expected=",
			      Hex(_settings.expect_checksum),
			      " computed=", Hex(checksum));
			return false;
		}

		void _report_mismatch(char const *prefix, char const *pair,
		                      Raster_diff const &diff) const
		{
			Point const at = diff.first_at(_settings.area);

			error(prefix, ": FAIL raster mismatch (", pair, ") first=(", at.x,
			      ",", at.y, ") actual=", Hex(diff.first_actual.pixel),
			      " expected=", Hex(diff.first_expected.pixel));
			error(prefix, ": FAIL mismatch region=", diff.region);
		}

		void _run_golden()
		{
			Capture_target capture(_env, Capture::Connection::Label { },
			                       _settings.area);

			bool matched = false;

			for (unsigned poll = 0; poll < POLL_ATTEMPTS; poll++) {

				capture.capture();

				if (!Raster_diff::compare(capture.pixels(), _expected(),
				                          _settings.area).found) {
					matched = true;
					break;
				}
				_timer.msleep(POLL_INTERVAL_MS);
			}

			if (!matched) {
			_report_mismatch("g1-capture", "capture vs expected",
			                 Raster_diff::compare(capture.pixels(),
			                                      _expected(),
			                                      _settings.area));
			return;

			}

			Genode::uint64_t const checksum = _checksum();

			if (!_verify_checksum("g1-capture", checksum))
				return;

			log("g1-capture: checksum=", Genode::Hex((unsigned long long)checksum));
		log("g1-capture: PASS");
		}

		void _run_diff()
		{
			Capture_target native(_env, "native", _settings.area);
			Capture_target upstream(_env, "upstream", _settings.area);

			bool matched = false;

			for (unsigned poll = 0; poll < POLL_ATTEMPTS; poll++) {

				native.capture();
				upstream.capture();

				bool const same_pair =
					!Raster_diff::compare(native.pixels(), upstream.pixels(),
					                      _settings.area).found;
				bool const same_expected =
					!Raster_diff::compare(native.pixels(), _expected(),
					                      _settings.area).found;

				if (same_pair && same_expected) {
					matched = true;
					break;
				}
				_timer.msleep(POLL_INTERVAL_MS);
			}

			if (matched) {

				Genode::uint64_t const checksum = _checksum();

				if (!_verify_checksum("g1-diff", checksum))
					return;

				log("g1-diff: checksum=", Genode::Hex((unsigned long long)checksum));
		log("g1-diff: MATCH");
				return;
			}

			/* loud mismatch: report every disagreeing pair */
			Raster_diff const native_vs_upstream =
				Raster_diff::compare(native.pixels(), upstream.pixels(),
				                     _settings.area);
			Raster_diff const native_vs_expected =
				Raster_diff::compare(native.pixels(), _expected(),
				                     _settings.area);
			Raster_diff const upstream_vs_expected =
				Raster_diff::compare(upstream.pixels(), _expected(),
				                     _settings.area);

			if (native_vs_upstream.found)
				_report_mismatch("g1-diff", "native vs upstream",
				                 native_vs_upstream);

			if (native_vs_expected.found)
				_report_mismatch("g1-diff", "native vs expected",
				                 native_vs_expected);

			if (upstream_vs_expected.found)
				_report_mismatch("g1-diff", "upstream vs expected",
				                 upstream_vs_expected);
		}

	public:

		Capture_probe(Env &env) : _env(env)
		{
			/* build the expected test-fixture raster (the oracle data) */
			_generate_expected(_expected_ds.local_addr<Pixel>(),
			                   _settings.area);

			if (_settings.mode == "golden")
				_run_golden();
			else if (_settings.mode == "diff")
				_run_diff();
			else
				error("g1-capture: FAIL unsupported mode=", _settings.mode);

			/* stay alive with Timer sleeps */
			for (;;)
				_timer.msleep(1000);
		}
};


void Component::construct(Genode::Env &env)
{
	static G1::Capture_probe capture_probe(env);
}
