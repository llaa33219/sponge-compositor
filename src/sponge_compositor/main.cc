/* SPDX-License-Identifier: Apache-2.0
 *
 * main.cc — sponge_compositor entry glue (G1, plan D16C.3/D16C.4).
 *
 * Thin C++ entry adapter only (AGENTS.md §3.6): read the native config,
 * provide the raw scene-state block, initialize the Mojo-owned scene
 * exactly once through the C ABI (abi.h), and construct + announce the
 * three public roots (Gui, Capture, Event). No scene state or logic
 * exists in C++: every scene field, reducer decision, stacking rule,
 * clipping/damage computation, and painter operation is Mojo-owned.
 *
 * Config (run/compositor.inc native node):
 *
 *   <config width="1024" height="768">
 *     <background color="#1e1e2e"/>
 *   </config>
 */

#include <base/component.h>
#include <base/attached_rom_dataspace.h>
#include <base/heap.h>
#include <base/log.h>
#include <base/stdint.h>

#include "abi.h"
#include "capture_adapter.h"
#include "event_adapter.h"
#include "gui_adapter.h"

namespace Sponge {

	/* kgen_shim.cc: register the heap backing the KGEN allocation seam */
	void kgen_shim_init(Genode::Heap &heap);

}  /* namespace Sponge */


/*
 * KGEN seam (defined in kgen_shim.cc): aligned raw-block allocation on
 * the registered 'Genode::Heap'. Used for the caller-provided scene
 * state block of abi.h.
 */
extern "C" void *KGEN_CompilerRT_AlignedAlloc(Genode::size_t align,
                                              Genode::size_t size);
extern "C" void  KGEN_CompilerRT_AlignedFree(void *ptr);


namespace {

	/* screen defaults of the G1 scenario (run/compositor.inc) */
	constexpr Genode::int32_t  DEFAULT_WIDTH     = 1024;
	constexpr Genode::int32_t  DEFAULT_HEIGHT    = 768;
	constexpr Genode::uint32_t DEFAULT_BG_RGB    = 0x1e1e2e;
	constexpr Genode::size_t   SCENE_STATE_ALIGN = 16;

	/*
	 * Named, explicitly typed view of the abi.h scene-storage size:
	 * the unnamed enum constant must not reach 'Output::print' or
	 * 'ascii_to' template machinery.
	 */
	constexpr Genode::uint64_t SCENE_STORAGE_BYTES = SC_SCENE_STORAGE_BYTES;

	bool hex_nibble(char c, unsigned &value)
	{
		if (c >= '0' && c <= '9') { value = (unsigned)(c - '0');     return true; }
		if (c >= 'a' && c <= 'f') { value = (unsigned)(c - 'a') + 10; return true; }
		if (c >= 'A' && c <= 'F') { value = (unsigned)(c - 'A') + 10; return true; }
		return false;
	}

	/*
	 * Parse a "#rrggbb" (or "rrggbb") color into the 0xRRGGBB pixel
	 * word. Returns false on malformed input.
	 */
	bool parse_color(char const *text, Genode::uint32_t &rgb)
	{
		if (text[0] == '#')
			text++;

		unsigned digits[6] = { };

		for (unsigned i = 0; i < 6; i++)
			if (!hex_nibble(text[i], digits[i]))
				return false;

		if (text[6] != '\0')
			return false;

		rgb = (digits[0] << 20) | (digits[1] << 16) | (digits[2] << 12)
		    | (digits[3] <<  8) | (digits[4] <<  4) |  digits[5];
		return true;
	}
}


void Component::construct(Genode::Env &env)
{
	static Genode::Heap heap { env.ram(), env.rm() };

	Sponge::kgen_shim_init(heap);

	static Genode::Attached_rom_dataspace config_rom { env, "config" };

	Genode::Xml_node const config = config_rom.xml();

	Genode::int32_t const width  = config.attribute_value("width",  DEFAULT_WIDTH);
	Genode::int32_t const height = config.attribute_value("height", DEFAULT_HEIGHT);

	/* <background color="#1e1e2e"/> */
	Genode::uint32_t bg_rgb  = DEFAULT_BG_RGB;
	bool             bg_seen = false;

	config.for_each_sub_node("background", [&] (Genode::Xml_node const &node) {

		if (bg_seen)
			return;   /* first 'background' node wins */
		bg_seen = true;

		Genode::String<32> const color =
			node.attribute_value("color", Genode::String<32>());

		Genode::uint32_t parsed = 0;
		if (parse_color(color.string(), parsed))
			bg_rgb = parsed;
		else
			Genode::warning("invalid background color '", color,
			                "', using #1e1e2e");
	});

	/*
	 * Aligned raw block for the Mojo-owned scene state (exactly
	 * SCENE_STORAGE_BYTES bytes per abi.h).
	 */
	Genode::uint8_t *const state = static_cast<Genode::uint8_t *>(
		KGEN_CompilerRT_AlignedAlloc(SCENE_STATE_ALIGN,
		                            (Genode::size_t)SCENE_STORAGE_BYTES));

	if (!state) {
		Genode::error("cannot allocate ", SCENE_STORAGE_BYTES,
		              "-byte scene state block");
		env.parent().exit(1);
		return;
	}

	/* initialize the Mojo-owned scene exactly once */
	Genode::int32_t const init_status = sc_scene_init(
		state, SCENE_STORAGE_BYTES, width, height,
		(Genode::int32_t)((bg_rgb >> 16) & 0xff),
		(Genode::int32_t)((bg_rgb >>  8) & 0xff),
		(Genode::int32_t)( bg_rgb        & 0xff));

	if (init_status != 0) {
		Genode::error("sc_scene_init failed (", init_status, ") for screen ",
		              width, "x", height);
		env.parent().exit(1);
		return;
	}

	log("sponge_compositor: screen ", width, "x", height, " background #",
	    Genode::Hex(bg_rgb, Genode::Hex::OMIT_PREFIX));

	/*
	 * Public roots (never Static_root): construct and announce Gui,
	 * Capture, and Event. The state block is shared with the Mojo core
	 * and only ever passed through, never inspected. 'Gui_root' lives
	 * in namespace Sponge (gui_adapter.h); 'Capture_root' and
	 * 'Event_root' are the global-scope roots of the parallel
	 * capture/event adapters.
	 */
	static Sponge::Gui_root gui_root {
		env, heap, state, SCENE_STORAGE_BYTES, config };
	gui_root.announce();

	static Capture_root capture_root {
		env, heap, state, SCENE_STORAGE_BYTES, width, height };
	capture_root.announce();

	static Event_root event_root {
		env, heap, state, SCENE_STORAGE_BYTES };
	event_root.announce();
}


Genode::size_t Component::stack_size()
{
	return 32 * 1024 * sizeof(Genode::addr_t);
}
