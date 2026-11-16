/* SPDX-License-Identifier: Apache-2.0
 *
 * sponge_compositor — G0 scaffold for the future Mojo-first compositor.
 *
 * This is an explicitly unimplemented skeleton. It implements only the
 * component entry point and logs a "not implemented" warning. It does
 * not provide Gui, Capture, or Event handling; the scene/painter/input
 * core will live in Mojo behind a thin C++ adapter (AGENTS.md §3.6)
 * and the Genode::Heap KGEN shim (docs/evidence/mojo-ffi-probe.md).
 */

#include <base/component.h>
#include <base/log.h>

void Component::construct(Genode::Env &)
{
	Genode::warning("not implemented: sponge_compositor");
}
