# sponge-compositor

Native Mojo-first compositor for Sponge OS.

## Status: G2 input verified

The repository implements the G1 vertical slice (two opaque views,
stacking, clipping/damage, and a real Capture consumer) plus the G2 input
battery on seL4 QEMU. G1 rasters match upstream nitpicker byte-for-byte.
G2 delivers real QMP clicks, alpha/input-mask fall-through, focus
transitions, deterministic Event injection, and pointer-report validation
through the public Genode surface. Upstream nitpicker remains the shipped
default. Native composition is used only with the explicit
`SPONGE_COMPOSITOR=native` opt-in; the native-default flip is a separate,
later commit and is not shipped here.

## Architecture

- **Mojo core** (`src/sponge_compositor/mojo/`): scene/session/view
  tables, view stack, command reducer, clipping/damage, opaque painter,
  and input hit-testing (pointer position, button state, focus, and Event
  delivery to the hit view). Alpha input masks pass clicks through to
  lower views where the mask says so. Fixed-capacity `InlineArray` state,
  64-bit View IDs, no heap on the hot path, and no C++ scene model.
- **Thin C++ adapter** (`src/sponge_compositor/`): public Genode
  Gui/Capture/Event/Framebuffer/Input session adapters, dataspace and
  signal glue, panorama ROM, and component entry.
- **Caller-provided state**: C++ supplies an 8192-byte raw state block and
  pixel dataspaces. Mojo owns every scene field and reducer/painter/input
  decision; no inline asm, module globals, or architecture-specific storage.
- **KGEN shim**: `kgen_shim.cc` provides the documented `Genode::Heap`
  AlignedAlloc/AlignedFree seam for future runtime requirements. Current
  scene, painter, and input objects have empty `nm -u`.

WM and DE are independent: they use the same public Genode interfaces and
are not included in the G1/G2 scenarios.

## Build

From a Genode build directory with this repository registered:

```bash
make sponge_compositor
make test/view_client test/capture_probe test/input_probe
```

The target emits the Mojo objects with the repository-pinned Mojo 1.0.0b2
toolchain using `--emit object --relocation-model=pic --target-cpu=x86-64`.
`nm -u` must remain empty for `build/scene.o`, `build/painter.o`, and
`build/input.o`.

## Verification

G1 (opaque raster):

```bash
make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-diff KERNEL=sel4 BOARD=pc
```

The first command is the upstream oracle, the second the native golden,
and the third the native-vs-upstream differential. Both rasters match
byte-for-byte with FNV-1a-64 checksum `0x670d6b5d9fc0ed7b`.

G2 (input battery):

```bash
make -C <build-dir> run/sponge-compositor-input KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-input KERNEL=sel4 BOARD=pc
```

Both Tier-0s pass all five phases and the overall `g2-input: PASS` marker.
The exact Event-injection differential `g2-input: trace-inject` matches on
both (`0xc50006d9e1676755`). The full-trace digest `g2-input: trace` is
diagnostic only, not a stability gate. A bounded record comparison showed
the documented attach-time hover-establishment divergence: upstream added
`abs_motion/top/511/383/-1` and `hover_leave/top/0/0/-1` before 536
otherwise identical records. That divergence is enumerated and allowed
under plan section 8.2; it is not hidden and is not a reason to synthesize
center hover in native.

Evidence: `docs/evidence/phase16-compositor-index.md` in the umbrella.

## G2 limits

Child views/associate, blit/panning, sync notifications, translucent
blending, keyboard/touch/global-key input, pointer-shape reporting, WM/DE
integration, GPU acceleration, and physical-hardware validation are outside
G1/G2 and are not claimed. Alpha input masks are consumed for hit testing
from G2 on, but blending translucent views is not implemented. Unsupported
protocol corners warn loudly or return documented safe results. Capacities
are 8 sessions, 32 views, and 3 merged damage rectangles. The default stays
upstream nitpicker; native-default selection ships only in a separate later
commit.
