/* SPDX-License-Identifier: Apache-2.0
 *
 * input_adapter.cc — Input::Session glue for the Sponge compositor.
 * See input_adapter.h for the contract.
 */

#include "input_adapter.h"

using namespace Sponge;


void Input_adapter::Action::exclusive_input_requested(bool /* enabled */)
{
	Genode::warning("not implemented before G2: exclusive input");
}
