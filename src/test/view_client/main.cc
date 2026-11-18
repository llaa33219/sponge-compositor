/* SPDX-License-Identifier: Apache-2.0
 *
 * compositor-view-client — G1 opaque-view probe client for Sponge OS.
 *
 * Reads a `<client .../>` style configuration node, opens a public
 * 'Gui::Connection', allocates the virtual framebuffer with alpha=false,
 * paints a real 4-byte 'Pixel_rgb888' solid fill with a 1 px contrasting
 * border, refreshes, creates a top-level view with the configured
 * geometry, and applies absolute 'Front' or 'Back' stacking through the
 * public GUI command buffer. The component then stays alive with Timer
 * sleeps so a run scenario can capture the composed output
 * deterministically.
 *
 * Public Genode APIs only. Reference patterns:
 * genode/repos/os/src/test/nitpicker/main.cc (buffer/pixel drawing),
 * genode/repos/os/src/test/capture/main.cc (top-level views, config).
 *
 * Example configuration:
 *
 *   <client label="view_a" x="100" y="80" width="400" height="300"
 *           color="0x3366cc" border="0xffffff" stack="front"/>
 */

#include <base/component.h>
#include <base/log.h>
#include <base/attached_rom_dataspace.h>
#include <util/reconstructible.h>
#include <base/node.h>
#include <gui_session/connection.h>
#include <os/pixel_rgb888.h>
#include <timer_session/connection.h>


namespace G1 {

	using namespace Genode;

	struct Settings;
	struct View_client;
}


/**
 * Settings parsed from the `<client .../>` configuration node
 */
struct G1::Settings
{
	String<64> label  { "view" };
	Gui::Point at     { };
	Gui::Area  size   { };
	Genode::uint32_t   color  { 0x3366cc };
	Genode::uint32_t   border { 0xffffff };
	bool       front  { true };
};


struct G1::View_client
{
	private:

		using Pixel = Pixel_rgb888;

		Env &_env;

		Attached_rom_dataspace _config { _env, "config" };

		Settings const _settings { _parse_settings(_config.node()) };

		Gui::Connection _gui { _env, _settings.label };

		Framebuffer::Mode const _mode { .area = _settings.size, .alpha = false };

		bool const _buffer_init = ( _gui.buffer(_mode), true );

		Attached_dataspace _fb_ds { _env.rm(), _gui.framebuffer.dataspace() };

		Constructible<Gui::Top_level_view> _view { };

		Timer::Connection _timer { _env };

		/**
		 * Convert 0xRRGGBB attribute value to a real 4-byte pixel
		 */
		static Pixel _pixel_from_rgb(Genode::uint32_t rgb)
		{
			return Pixel((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
		}

		static Settings _settings_from_node(Node const &node)
		{
			Settings settings { };

			settings.label  = node.attribute_value("label", String<64>("view"));
			settings.at     = Gui::Point { node.attribute_value("x", 0),
			                               node.attribute_value("y", 0) };
			settings.size   = Gui::Area::from_node(node);
			settings.color  = node.attribute_value("color",  0x3366ccU);
			settings.border = node.attribute_value("border", 0xffffffU);

			String<8> const stack = node.attribute_value("stack", String<8>("front"));

			if (stack == "front")
				settings.front = true;
			else if (stack == "back")
				settings.front = false;
			else
				warning("g1-view: unknown stack=\"", stack, "\", assuming \"front\"");

			if (settings.size.count() == 0)
				error("g1-view: invalid or missing 'width'/'height' config attributes");

			return settings;
		}

		/**
		 * Return the `<client .../>` node of the configuration
		 *
		 * A dedicated 'client' sub node is preferred. If absent, the
		 * configuration root node itself carries the client attributes.
		 */
		static Settings _parse_settings(Node const &root)
		{
			Settings result { };
			bool found = false;

			root.for_each_sub_node("client", [&] (Node const &node) {
				if (!found) {
					result = _settings_from_node(node);
					found  = true;
				}});

			if (!found)
				result = _settings_from_node(root);

			return result;
		}

		/**
		 * Paint solid fill with 1 px contrasting border into the buffer
		 */
		void _paint()
		{
			Gui::Area const size = _settings.size;

			Pixel const fill   = _pixel_from_rgb(_settings.color);
			Pixel const border = _pixel_from_rgb(_settings.border);

			Pixel * const pixels = _fb_ds.local_addr<Pixel>();

			for (unsigned y = 0; y < size.h; y++)
				for (unsigned x = 0; x < size.w; x++) {

					bool const at_border = (x == 0) || (y == 0)
					                    || (x + 1 == size.w) || (y + 1 == size.h);

					pixels[y*size.w + x] = at_border ? border : fill;
				}
		}

	public:

		View_client(Env &env) : _env(env)
		{
			_paint();

			_gui.framebuffer.refresh(Gui::Rect { { }, _settings.size });

			/* create top-level view with the configured geometry */
			_view.construct(_gui, Gui::Rect { _settings.at, _settings.size });

			/*
			 * Apply absolute stacking through the public command buffer.
			 * Absolute 'Front'/'Back' makes the z order deterministic
			 * regardless of the order in which the independent clients
			 * happen to start.
			 */
			if (_settings.front)
				_gui.enqueue<Gui::Session::Command::Front>(_view->id());
			else
				_gui.enqueue<Gui::Session::Command::Back>(_view->id());

			_gui.execute();

			log("g1-view: ", _settings.label, ": ready");

			/* stay alive with Timer sleeps */
			for (;;)
				_timer.msleep(1000);
		}
};


void Component::construct(Genode::Env &env)
{
	static G1::View_client view_client(env);
}
