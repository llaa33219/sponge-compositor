/* SPDX-License-Identifier: Apache-2.0
 *
 * event_adapter.h — thin C++ 'Event' session adapter (G1).
 *
 * Serves the public 'Event::Session' interface over the Mojo-owned
 * compositor core through the C ABI of 'abi.h'. Each session owns a
 * real 4096-byte event dataspace (the size used by the established
 * public Event servers) so eager clients can attach and batch events
 * today. Event injection into the scene is not implemented before G2:
 * 'submit_batch' logs an explicit
 * 'Genode::warning("not implemented before G2: ...")' and drops the
 * batch without claiming delivery. There is no C++ input or scene
 * model here (D16C.10) and no private nitpicker surface (D16C.7).
 *
 * Integration contract (Gui/main): 'Event_root' is constructed as
 *
 *   Event_root(Genode::Env &, Genode::Heap &,
 *              Genode::uint8_t *state, Genode::uint64_t state_size)
 *
 * with the shared Mojo scene-state block that 'sc_scene_init' was
 * called on (see 'abi.h'), followed by 'announce()'. Each Event
 * session opens an SC_SESSION_EVENT Mojo session and closes it at
 * destruction.
 */

#pragma once

/* Genode includes */
#include <base/attached_ram_dataspace.h>
#include <base/env.h>
#include <base/heap.h>
#include <base/session_object.h>
#include <event_session/event_session.h>
#include <root/component.h>

/* local includes */
#include "abi.h"


/*
 * 'Event::Session' declares no virtual members (unlike 'Capture::Session'):
 * the public server-side pattern passes the implementing class as the
 * second 'Session_object' argument, as the established public Event
 * servers do.
 */
class Event_session_component
:
	public Genode::Session_object<Event::Session, Event_session_component>
{
	public:

		/**
		 * Size of the shared event dataspace in bytes (the size used
		 * by the established public Event servers; clients size their
		 * batches from the dataspace).
		 */
		enum { EVENT_DS_SIZE = 4096 };

	private:

		Genode::Env &_env;

		/*
		 * Shared Mojo scene-state block (first argument of every ABI
		 * call). Opaque to C++ (abi.h state-storage contract).
		 */
		Genode::uint8_t  *_state      { nullptr };
		Genode::uint64_t  _state_size { 0 };

		/* Mojo session handle from 'sc_session_open', -1 when absent */
		Genode::int32_t _mojo_handle { -1 };

		/* real event buffer, accounted against the client's quota */
		Genode::Accounted_ram_allocator _ram;

		Genode::Attached_ram_dataspace _ds;

		bool _state_usable() const
		{
			return _state != nullptr
			    && _state_size >= (Genode::uint64_t)SC_SCENE_STORAGE_BYTES;
		}

		void _close_mojo_session();

	public:

		Event_session_component(Genode::Env     &env,
		                        Genode::uint8_t *state,
		                        Genode::uint64_t state_size,
		                        Resources const &resources,
		                        Label     const &label);

		~Event_session_component();

		Event_session_component(Event_session_component const &) = delete;
		Event_session_component &operator=(Event_session_component const &) = delete;


		/*****************************
		 ** Event session interface **
		 *****************************/

		Genode::Dataspace_capability dataspace();

		void submit_batch(unsigned count);
};


/**
 * Root of the public 'Event' service (Gui/main integration contract)
 */
struct Event_root : Genode::Root_component<Event_session_component>
{
	private:

		Genode::Env &_env;

		Genode::uint8_t  *_state      { nullptr };
		Genode::uint64_t  _state_size { 0 };

	protected:

		Create_result _create_session(const char *args,
		                              Genode::Affinity const &) override;

		void _upgrade_session(Event_session_component &session,
		                      const char *args) override;

		void _destroy_session(Event_session_component &session) override;

	public:

		Event_root(Genode::Env     &env,
		           Genode::Heap    &heap,
		           Genode::uint8_t *state,
		           Genode::uint64_t state_size);

		Event_root(Event_root const &) = delete;
		Event_root &operator=(Event_root const &) = delete;

		void announce();
};
