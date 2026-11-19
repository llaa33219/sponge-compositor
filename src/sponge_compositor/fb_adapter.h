/* SPDX-License-Identifier: Apache-2.0
 *
 * fb_adapter.h — per-Gui-session Framebuffer::Session adapter for the
 * Sponge compositor (G1, plan D16C.3).
 *
 * Each Gui session owns one virtual framebuffer: the real 4-byte
 * 'Pixel_rgb888' pixel dataspace the client renders into. This adapter
 * speaks the public 'Framebuffer::Session' surface and nothing else:
 *
 *   dataspace()  real 'Attached_ram_dataspace' backing the client
 *                buffer (allocated on 'define_buffer', i.e. on the
 *                'Gui::Session::buffer' call)
 *   mode()       the mode of the current buffer
 *   refresh()    adds damage: the refreshed region enters the scene's
 *                pending damage through the public ABI (sc_damage_add)
 *
 * The pixel, alpha, and input-mask planes of the dataspace are handed
 * to the Mojo-owned scene through 'sc_view_bind_surfaces' by the Gui
 * session adapter (gui_adapter.cc) on every buffer call and on every
 * view creation. C++ owns only the dataspace and the session glue; it
 * does not model or inspect the scene (abi.h, D16C.10).
 *
 * The remaining Framebuffer RPCs (blit, panning, mode_sigh, sync_sigh,
 * sync_source) degrade to explicit warnings and safe no-op results
 * until G2; they never crash.
 */

#pragma once

#include <base/attached_ram_dataspace.h>
#include <base/session_object.h>
#include <framebuffer_session/framebuffer_session.h>
#include <util/reconstructible.h>

#include "abi.h"

namespace Sponge {


struct Fb_session : Genode::Session_object<Framebuffer::Session>
{
	private:

		Genode::Env  &_env;
		Genode::uint8_t *const _state;   /* Mojo scene state block (abi.h) */

		Framebuffer::Mode _mode { };

		/*
		 * Real pixel dataspace of the client buffer. The pixel layer
		 * is 4 bytes per pixel ('Pixel_rgb888'); a client may request
		 * 'alpha', which appends alpha and input-mask layers after the
		 * pixel layer ('Framebuffer::Mode'). The plane bases are
		 * exposed via 'pixels', 'alpha', and 'input_mask'; the
		 * allocation always covers the full 'mode.num_bytes()'.
		 */
		Genode::Constructible<Genode::Attached_ram_dataspace> _ds { };

	public:

		/* noncopyable (pointer member) */
		Fb_session(Fb_session const &) = delete;
		Fb_session &operator = (Fb_session const &) = delete;

		/**
		 * Constructor
		 *
		 * \param env      execution environment (RAM/entrypoint)
		 * \param state    Mojo scene state block shared with the
		 *                 Mojo core (never inspected here)
		 * \param resources  session resources of the owning Gui session
		 * \param label      session label of the owning Gui session
		 */
		Fb_session(Genode::Env &env, Genode::uint8_t *state,
		           Genode::Session::Resources const &resources,
		           Genode::Session::Label const &label)
		:
			Genode::Session_object<Framebuffer::Session>(
				env.ep(), resources, label),
			_env(env), _state(state)
		{ }

		/**
		 * (Re)allocate the client pixel buffer for 'mode'
		 *
		 * Called on every 'Gui::Session::buffer' call. The previous
		 * dataspace (if any) is replaced wholesale, matching the
		 * public realloc-on-'buffer' contract. Returns false when the
		 * dataspace cannot be allocated.
		 */
		bool define_buffer(Framebuffer::Mode mode);

		/**
		 * Base of the 4-byte pixel layer, or nullptr before the first
		 * successful 'define_buffer'
		 */
		Genode::uint32_t const *pixels() const;

		/**
		 * Row pitch of the pixel layer in bytes ('mode.area.w' * 4),
		 * or 0 before the first successful 'define_buffer'
		 */
		Genode::int32_t pitch() const;

		/**
		 * Base of the 1-byte alpha plane of an 'alpha' buffer
		 * ('base + area.count() * 4' in the 'Framebuffer::Mode'
		 * layout), or nullptr when the current buffer is not an
		 * 'alpha' buffer
		 */
		Genode::uint8_t const *alpha() const;

		/**
		 * Base of the 1-byte input-mask plane of an 'alpha' buffer
		 * (the alpha plane plus 'area.count()' bytes), or nullptr
		 * when the current buffer is not an 'alpha' buffer
		 */
		Genode::uint8_t const *input_mask() const;

		/**
		 * Row pitch of the alpha and input-mask planes in bytes
		 * ('mode.area.w'), or 0 when the current buffer is not an
		 * 'alpha' buffer
		 */
		Genode::int32_t byte_pitch() const;


		/*******************************
		 ** Framebuffer::Session API **
		 *******************************/

		Genode::Dataspace_capability dataspace() override;
		Framebuffer::Mode mode() const override;
		void mode_sigh(Genode::Signal_context_capability sigh) override;
		void refresh(Framebuffer::Rect rect) override;
		Blit_result blit(Framebuffer::Blit_batch const &batch) override;
		void panning(Framebuffer::Point pos) override;
		void sync_sigh(Genode::Signal_context_capability sigh) override;
		void sync_source(Genode::Session_label const &label) override;
};


}  /* namespace Sponge */
