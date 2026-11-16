# sponge-compositor

**G0 scaffold only.** This repository contains nothing but a buildable,
explicitly unimplemented Genode component skeleton for the future
Sponge OS compositor. It does not implement any compositing behavior.

## Status

- The target builds `sponge_compositor` from `src/sponge_compositor/`
  (`SRC_CC := main.cc`, `LIBS := base`). The entry point implements
  only `Component::construct(Genode::Env &)` and logs
  `Genode::warning("not implemented: sponge_compositor")`.
- No Gui, Capture, or Event session handling exists here. Nothing
  draws, captures, or routes input. No runtime services, no run
  scenario.
- G1 will implement the two-opaque-view slice once repository wiring
  is green.

## Planned architecture (G1 and later)

- **Mojo-owned core**: the scene, painter, and input core will be
  written in Mojo, per the Sponge OS Mojo-first policy (AGENTS.md
  §3.5/§3.6).
- **Thin C++ adapter**: only the `Component::construct` entry glue and
  the `SRC_O` link seam stay C++ (AGENTS.md §3.6).
- **`Genode::Heap` KGEN shim**: in-OS Mojo runs behind the proven
  `Genode::Heap` KGEN shim recorded in
  `docs/evidence/mojo-ffi-probe.md` — not a full Mojo runtime.
- **No libc**: the component stays libc-free, requesting only
  minimum-privilege sessions (Genode philosophy, AGENTS.md §1.2).
- **Upstream nitpicker fallback**: where sponge_compositor is absent
  or inactive, the system keeps using upstream nitpicker unchanged.
- **No default flip**: sponge_compositor never becomes the default GUI
  backend behind the user's back; enabling it is always an explicit
  opt-in (AGENTS.md §1.1: automation = default, control = a door that
  is always open).

## Build

A standard Genode target. From a Genode build directory that has this
repository available as `repos/sponge-compositor`:

```
make sponge_compositor
```

No run scenario ships at G0; the scaffold is verified to compile and
link only.
