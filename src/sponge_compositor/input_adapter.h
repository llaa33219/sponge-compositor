/* SPDX-License-Identifier: Apache-2.0
 *
 * input_adapter.h — per-Gui-session Input::Session glue for the Sponge
 * compositor (G2, plan D16C.3).
 *
 * 'Gui::Connection' eagerly constructs its 'Input::Session_client',
 * which calls the 'dataspace' RPC and attaches the event buffer during
 * construction. Every Gui session must therefore provide a real
 * 'Input::Session_component' (os/include/input/component.h) with a real
 * event dataspace from the very first moment. This adapter owns one
 * such component per Gui session; the component supplies the dataspace,
 * the event queue, and the Input RPC surface.
 *
 * The adapter is the delivery target of the input pipeline (G2): the
 * session registers its component with 'Input_pipeline::register_session'
 * and every routed event reaches the client through 'submit'. The
 * exclusive-input hook ('Input::Session::exclusive') is wired to the
 * 'sc_input_exclusive' seam of abi.h. No scene state or routing logic
 * lives here.
 */

#pragma once

#include <input/component.h>

#include "abi.h"

namespace Sponge {


struct Input_adapter
{
	private:

		/*
		 * 'Input::Session_component::Action' hook: exclusive-input
		 * intent ('Input::Session::exclusive'), forwarded to the
		 * Mojo-owned input state ('sc_input_exclusive').
		 */
		struct Action : Input::Session_component::Action
		{
			Genode::uint8_t *const _state;

			Action(Genode::uint8_t *state) : _state(state) { }

			Action(Action const &) = delete;
			Action &operator=(Action const &) = delete;

			void exclusive_input_requested(bool enabled) override;
		} _action;

		Input::Session_component _component;

	public:

		/**
		 * Constructor
		 *
		 * \param env    provides the entrypoint (RPC dispatch of the
		 *               session object) and the RAM backing store of
		 *               the real input-event dataspace
		 * \param state  Mojo scene state block shared with the Mojo
		 *               core (never inspected; used by the
		 *               exclusive-input hook)
		 *
		 * The session object is managed at 'env.ep()' and its
		 * event dataspace is a real 'Attached_ram_dataspace' as
		 * provided by 'Input::Session_component'. The event queue is
		 * enabled eagerly ('Input::Event_queue::add' drops events
		 * while disabled; the Input root of input/root.h enables it
		 * on session open, but Gui sessions hand their Input session
		 * out through 'Gui::Session::input()' without an Input root).
		 */
		Input_adapter(Genode::Env &env, Genode::uint8_t *state)
		:
			_action(state),
			_component(env.ep(), env.ram(), env.rm(), _action)
		{
			_component.event_queue().enabled(true);
		}

		/**
		 * Capability of the Input session handed to 'Gui::Session::input()'
		 */
		Input::Session_capability cap() { return _component.cap(); }

		/**
		 * Delivery target of the input pipeline
		 * ('Input_pipeline::register_session')
		 */
		Input::Session_component &component() { return _component; }

		/**
		 * Submit one routed input event to the session's event queue
		 */
		void submit(Input::Event event) { _component.submit(event); }

		/**
		 * Forward the client's event-wakeup signal registration
		 */
		void sigh(Genode::Signal_context_capability sigh)
		{
			_component.sigh(sigh);
		}
};


}  /* namespace Sponge */
