/* SPDX-License-Identifier: Apache-2.0
 *
 * input_adapter.h — per-Gui-session Input::Session glue for the Sponge
 * compositor (G1, plan D16C.3).
 *
 * 'Gui::Connection' eagerly constructs its 'Input::Session_client',
 * which calls the 'dataspace' RPC and attaches the event buffer during
 * construction. Every Gui session must therefore provide a real
 * 'Input::Session' with a real event dataspace from the very first
 * moment. This adapter owns one public 'Input::Session_component'
 * (os/include/input/component.h) per Gui session; that component
 * supplies the dataspace, the event queue, and the Input RPC surface.
 *
 * No scene state or logic lives here. Input *routing* into the scene
 * (pointer position, focus, Event delivery to the hit view) is
 * Mojo-owned and gated for G2; this adapter only keeps the public
 * Input session constructible. The exclusive-input hook degrades to an
 * explicit warning (not implemented before G2).
 */

#pragma once

#include <input/component.h>

namespace Sponge {


struct Input_adapter
{
	private:

		/*
		 * 'Input::Session_component::Action' hook: exclusive-input
		 * intent. Not supported before G2; the request is refused
		 * loudly instead of silently obeyed.
		 */
		struct Action : Input::Session_component::Action
		{
			void exclusive_input_requested(bool enabled) override;
		} _action { };

		Input::Session_component _component;

	public:

		/**
		 * Constructor
		 *
		 * \param env  provides the entrypoint (RPC dispatch of the
		 *             session object) and the RAM backing store of
		 *             the real input-event dataspace
		 *
		 * The session object is managed at 'env.ep()' and its
		 * event dataspace is a real 'Attached_ram_dataspace' as
		 * provided by 'Input::Session_component'.
		 */
		Input_adapter(Genode::Env &env)
		:
			_component(env.ep(), env.ram(), env.rm(), _action)
		{ }

		/**
		 * Capability of the Input session handed to 'Gui::Session::input()'
		 */
		Input::Session_capability cap() { return _component.cap(); }
};


}  /* namespace Sponge */
