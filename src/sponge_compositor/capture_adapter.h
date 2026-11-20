/* SPDX-License-Identifier: Apache-2.0
 *
 * capture_adapter.h — thin C++ 'Capture' session adapter (G1).
 *
 * Serves the public 'Capture::Session' interface over the Mojo-owned
 * compositor core through the C ABI of 'abi.h'. This adapter owns no
 * scene state: every pixel in the shared capture buffer comes from
 * 'sc_compose', and every affected rect reported by 'capture_at' comes
 * from 'sc_damage_fetch'. There is no second scene model in C++
 * (D16C.10) and no private nitpicker surface (D16C.7).
 *
 * Integration contract (Gui/main): 'Capture_root' is constructed as
 *
 *   Capture_root(Genode::Env &, Genode::Heap &,
 *                Genode::uint8_t *state, Genode::uint64_t state_size,
 *                Genode::int32_t screen_w, Genode::int32_t screen_h)
 *
 * with the shared Mojo scene-state block that 'sc_scene_init' was
 * called on (see 'abi.h'), followed by 'announce()'. Each Capture
 * session opens an SC_SESSION_CAPTURE Mojo session and closes it at
 * destruction. Capture buffers must match the configured screen size
 * (1024x768 by default); any other protocol corner that is not
 * implemented before G2 logs an explicit
 * 'Genode::warning("not implemented before G2: ...")' and never
 * pretends to have delivered the missing behavior.
 */

#pragma once

/* Genode includes */
#include <base/attached_ram_dataspace.h>
#include <base/env.h>
#include <base/heap.h>
#include <base/session_object.h>
#include <capture_session/capture_session.h>
#include <root/component.h>
#include <util/reconstructible.h>

/* local includes */
#include "abi.h"


namespace Sponge { struct Gui_root; }
struct Capture_root;


class Capture_session_component : public Genode::Session_object<Capture::Session>
{
	public:

		using Area          = Capture::Area;
		using Point         = Capture::Point;
		using Rect          = Capture::Rect;
		using Buffer_attr   = Capture::Session::Buffer_attr;
		using Buffer_result = Capture::Session::Buffer_result;
		using Affected_rects = Capture::Session::Affected_rects;

	private:

		Genode::Env &_env;
		Sponge::Gui_root &_gui_root;
		Capture_root &_capture_root;

		/*
		 * Shared Mojo scene-state block (first argument of every ABI
		 * call). The block is opaque to C++: its contents are never
		 * modeled or inspected here (abi.h state-storage contract).
		 */
		Genode::uint8_t  *_state      { nullptr };
		Genode::uint64_t  _state_size { 0 };

		Genode::int32_t _screen_w { 0 };
		Genode::int32_t _screen_h { 0 };

		/* Mojo session handle from 'sc_session_open', -1 when absent */
		Genode::int32_t _mojo_handle { -1 };

		/* capture buffer, accounted against the client's session quota */
		Genode::Accounted_ram_allocator _ram;

		Genode::Constructible<Genode::Attached_ram_dataspace> _buffer { };

		Buffer_attr _buffer_attr { };

		Genode::Signal_context_capability _screen_size_sigh { };
		Genode::Signal_context_capability _wakeup_sigh      { };

		Affected_rects _pending { };

		bool _stopped { false };

		/* warn at most once per session about the non-origin corner */
		bool _warned_anchor { false };

		bool _state_usable() const
		{
			return _state != nullptr
			    && _state_size >= (Genode::uint64_t)SC_SCENE_STORAGE_BYTES;
		}

		/**
		 * Non-destructive pending-damage probe
		 *
		 * Per the 'sc_damage_fetch' contract a call with 'max_rects = 0'
		 * and a non-null rect buffer writes nothing and never clears the
		 * damage: it returns -2 while damage is pending and 0 when the
		 * damage list is empty. Any other status is reported and treated
		 * as "no damage".
		 */
		bool _damage_pending();
		void _pending_add(Rect const &rect);

		void _wakeup_if_needed();

		void _close_mojo_session();

	public:

		Capture_session_component(Genode::Env     &env,
		                          Sponge::Gui_root &gui_root,
		                          Capture_root &capture_root,
		                          Genode::uint8_t *state,
		                          Genode::uint64_t state_size,
		                          Genode::int32_t  screen_w,
		                          Genode::int32_t  screen_h,
		                          Resources const &resources,
		                          Label     const &label);

		~Capture_session_component();

		Capture_session_component(Capture_session_component const &) = delete;
		Capture_session_component &operator=(Capture_session_component const &) = delete;


		/*******************************
		 ** Capture session interface **
		 *******************************/

		Area screen_size() const override;

		void screen_size_sigh(Genode::Signal_context_capability sigh) override;

		void wakeup_sigh(Genode::Signal_context_capability sigh) override;

		Buffer_result buffer(Buffer_attr attr) override;

		Genode::Dataspace_capability dataspace() override;

		Affected_rects capture_at(Point pos) override;

		void add_pending(Rect const &rect) { _pending_add(rect); }

		void capture_stopped() override;
};


/**
 * Root of the public 'Capture' service (Gui/main integration contract)
 */
struct Capture_root : Genode::Root_component<Capture_session_component>
{
	using Rect = Capture::Rect;

	/**
	 * G1 default screen size (run/compositor.inc configures
	 * 'width: 1024 | height: 768'); used when the supplied screen
	 * size is not positive.
	 */
	enum {
		DEFAULT_SCREEN_W = 1024,
		DEFAULT_SCREEN_H = 768
	};

	private:

		Genode::Env &_env;
		Sponge::Gui_root &_gui_root;

		Genode::uint8_t  *_state      { nullptr };
		Genode::uint64_t  _state_size { 0 };

		Genode::int32_t _screen_w { DEFAULT_SCREEN_W };
		Genode::int32_t _screen_h { DEFAULT_SCREEN_H };

		Capture_session_component *_sessions[SC_MAX_SESSIONS] { };

	protected:

		Create_result _create_session(const char *args,
		                              Genode::Affinity const &) override;

		void _upgrade_session(Capture_session_component &session,
		                      const char *args) override;

		void _destroy_session(Capture_session_component &session) override;

	public:

		Capture_root(Genode::Env     &env,
		             Genode::Heap    &heap,
		             Sponge::Gui_root &gui_root,
		             Genode::uint8_t *state,
		             Genode::uint64_t state_size,
		             Genode::int32_t  screen_w,
		             Genode::int32_t  screen_h);

		Capture_root(Capture_root const &) = delete;
		Capture_root &operator=(Capture_root const &) = delete;

		void announce();
		void _add_pending(Rect const &rect);
};
