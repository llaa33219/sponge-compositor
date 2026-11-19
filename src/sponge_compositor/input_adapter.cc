/* SPDX-License-Identifier: Apache-2.0
 *
 * input_adapter.cc — Input::Session glue for the Sponge compositor.
 * See input_adapter.h for the contract.
 */

#include "input_adapter.h"

using namespace Sponge;


void Input_adapter::Action::exclusive_input_requested(bool enabled)
{
	Genode::int32_t const rc = sc_input_exclusive(_state, enabled ? 1 : 0);

	if (rc != 0)
		Genode::warning("sc_input_exclusive failed (", rc, ")");
}
