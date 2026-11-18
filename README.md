# sponge-compositor

Native Mojo-first compositor for Sponge OS.

## Status: G1 complete

The repository implements the G1 vertical slice: two opaque views,
stacking, clipping/damage, and a real Capture consumer on seL4 QEMU.
Upstream nitpicker remains the shipped default. Native composition is used
only with the explicit `SPONGE_COMPOSITOR=native` opt-in.

## Architecture

- **Mojo core** (`src/sponge_compositor/mojo/`): scene/session/view
  tables, view stack, command reducer, clipping/damage, and opaque painter.
  Fixed-capacity `InlineArray` state, 64-bit View IDs, no heap on the hot
  path, and no C++ scene model.
- **Thin C++ adapter** (`src/sponge_compositor/`): public Genode
  Gui/Capture/Event/Framebuffer/Input session adapters, dataspace and
  signal glue, panorama ROM, and component entry.
- **Caller-provided state**: C++ supplies an 8192-byte raw state block and
  pixel dataspaces. Mojo owns every scene field and reducer/painter
  decision; no inline asm, module globals, or architecture-specific storage.
- **KGEN shim**: `kgen_shim.cc` provides the documented `Genode::Heap`
  AlignedAlloc/AlignedFree seam for future runtime requirements. Current
  scene and painter objects have empty `nm -u`.

WM and DE are independent: they use the same public Genode interfaces and
are not included in the G1 scenario.

## Build

From a Genode build directory with this repository registered:

```bash
make sponge_compositor
make test/view_client test/capture_probe
```

The target emits both Mojo objects with the repository-pinned Mojo 1.0.0b2
toolchain using `--emit object --relocation-model=pic --target-cpu=x86-64`.
`nm -u` must remain empty for `build/scene.o` and `build/painter.o`.

## Verification

```bash
make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-diff KERNEL=sel4 BOARD=pc
```

The first command is the upstream oracle, the second the native golden,
and the third the native-vs-upstream differential. Both rasters match
byte-for-byte with FNV-1a-64 checksum `0x670d6b5d9fc0ed7b`.
Evidence: `docs/evidence/phase16-compositor-index.md` in the umbrella.

## G1 limitations

Event injection, child views/associate, focus/title/background policy,
blit/panning, sync notifications, alpha/input masks, translucent blending,
WM/DE integration, GPU acceleration, and physical-hardware validation are
outside G1 and are not claimed. Unsupported protocol corners warn loudly or
return documented safe results. Capacities are 8 sessions, 32 views, and 3
merged damage rectangles. G2 requires separate approval.
