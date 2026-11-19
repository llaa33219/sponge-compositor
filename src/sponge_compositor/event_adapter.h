/* SPDX-License-Identifier: Apache-2.0
 *
 * event_adapter.h — thin C++ 'Event' session adapter (G2).
 *
 * Serves the public 'Event::Session' interface over the Mojo-owned
 * compositor core through the C ABI of 'abi.h'. Each session owns a
 * real 4096-byte event dataspace (the size used by the established
 * public Event servers) so eager clients can attach and batch events.
 *
 * Submitted batches are real input: 'submit_batch' clamps the count to
 * the dataspace capacity (the buffer cannot hold more) and forwards the
 * events to the shared 'Input_pipeline' (input_pipeline.h), which
 * reduces them through the Mojo core and delivers the results to the
 * per-session Input queues - the same path as driver-side input. No
 * batch is dropped and no delivery is claimed that did not happen.
 * There is no C++ input or scene model here (D16C.10) and no private
 * nitpicker surface (D16C.7).
 *
 * Integration contract (Gui/main): 'Event_root' is constructed as
 *
 *   Event_root(Genode::Env &, Genode::Heap &, Input_pipeline &)
 *
 * with the pipeline that holds the shared Mojo scene-state block
 * 'sc_scene_init' was called on (see 'abi.h'), followed by 'announce()'.
 * Each Event session opens an SC_SESSION_EVENT Mojo session and closes
 * it at destruction.
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
#include "input_pipeline.h"


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
		 * Shared input pipeline (owns the Mojo scene-state block; the
		 * state stays opaque to C++ per the abi.h state-storage
		 * contract).
		 */
		Sponge::Input_pipeline &_pipeline;

		/* Mojo session handle from 'sc_session_open', -1 when absent */
		Genode::int32_t _mojo_handle { -1 };

		/* real event buffer, accounted against the client's quota */
		Genode::Accounted_ram_allocator _ram;

		Genode::Attached_ram_dataspace _ds;

		void _close_mojo_session();

	public:

		Event_session_component(Genode::Env     &env,
		                        Sponge::Input_pipeline &pipeline,
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

		Sponge::Input_pipeline &_pipeline;

	protected:

		Create_result _create_session(const char *args,
		                              Genode::Affinity const &) override;

		void _upgrade_session(Event_session_component &session,
		                      const char *args) override;

		void _destroy_session(Event_session_component &session) override;

	public:

		Event_root(Genode::Env     &env,
		           Genode::Heap    &heap,
		           Sponge::Input_pipeline &pipeline);

		Event_root(Event_root const &) = delete;
		Event_root &operator=(Event_root const &) = delete;

		void announce();
};
