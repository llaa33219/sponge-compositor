# sponge-compositor

Native Mojo-first compositor for Sponge OS.

## Status: G1-G3 verified with documented intermittent close-time input failure

The repository implements the G1 vertical slice (two opaque views,
stacking, clipping/damage, and a real Capture consumer), the G2 input
battery, and the G3 Criterion-16 battery on seL4 QEMU. G1 rasters match
upstream nitpicker byte-for-byte (`g1-diff: MATCH`, checksum
`0x670d6b5d9fc0ed7b`). G2 delivers real QMP clicks, alpha/input-mask
fall-through, focus transitions, deterministic Event injection, and
pointer-report validation through the public Genode surface; the exact
Event-injection differential matches on both Tier-0s
(`trace-inject` `0xc50006d9e1676755`). G3 runs the full
move/resize/focus/close/minimize/maximize/tiling battery and the
>=20-cycle rules-drift soak with the Sponge WM trio and DE tasklist
above the native compositor: native QMP 16-step, tasks, and full
20-cycle tiling with close and reflow all PASS. One recorded tiling run
failed at close while multiple later runs passed; that intermittent
close-time input failure is documented (see Verification) and is never
relabeled, so the status is G1-G3 verified with a documented
intermittent close-time input failure, not a claim of perfect
stability. Alpha blending and WM/DE integration are proven on seL4
QEMU; G4-G5, product integration, and physical-hardware validation
remain uncompleted. Tier-0 selection is default-native: unset or
`SPONGE_COMPOSITOR=native` runs `sponge_compositor`, and
`SPONGE_COMPOSITOR=upstream` runs the explicit nitpicker fallback
(any other value fails loudly); both paths work. The user approved the
native default on 2026-11-19, after the green G2 receipts.

The G3 functional changes are correctness fixes, not changes to the
acceptance assertions: per-session `pressed_session` preserves input
across view recreation; bind-time texture extents add an in-extent fast
path and safe tiling; the per-session domain Info_rom reports the
session's domain (panel 1024x28 versus the full-screen default);
damage is distributed per Capture session with sync only on nonempty
damage and an idle composition fast return; DE static rules convert to
boundary-relative coordinates; and the tiling scenario restores either
parked window, focuses the master title, enforces pointer barriers,
recomputes close geometry after focus, and dispatches four absolute-tablet
markers through the W4 scale transform.

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
are not included in the G1/G2 scenarios. The G3 battery runs the Sponge
WM trio and DE tasklist above the compositor through those same
interfaces (WM/DE integration proven on seL4 QEMU).

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
SPONGE_COMPOSITOR=upstream make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
make -C <build-dir> run/sponge-compositor-opaque KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=native make -C <build-dir> run/sponge-compositor-diff KERNEL=sel4 BOARD=pc
```

The first command is the upstream oracle, pinned to the explicit
`SPONGE_COMPOSITOR=upstream` override because native is the default. The
second is the native golden (the default selection; the explicit
`SPONGE_COMPOSITOR=native` spelling is its equivalent), and the third is
the native-vs-upstream differential, which names its native arm with the
explicit override. Both rasters match byte-for-byte with FNV-1a-64
checksum `0x670d6b5d9fc0ed7b`.

G2 (input battery):

```bash
make -C <build-dir> run/sponge-compositor-input KERNEL=sel4 BOARD=pc
SPONGE_COMPOSITOR=upstream make -C <build-dir> run/sponge-compositor-input KERNEL=sel4 BOARD=pc
```

The first command is the default-native run, the second the explicit
upstream fallback. Both Tier-0s pass all five phases and the overall `g2-input: PASS` marker.
The exact Event-injection differential `g2-input: trace-inject` matches on
both (`0xc50006d9e1676755`). The full-trace digest `g2-input: trace` is
diagnostic only, not a stability gate. A bounded record comparison showed
the documented attach-time hover-establishment divergence: upstream added
`abs_motion/top/511/383/-1` and `hover_leave/top/0/0/-1` before 536
otherwise identical records. That divergence is enumerated and allowed
under plan section 8.2; it is not hidden and is not a reason to synthesize
center hover in native.

G3 (Criterion-16 battery):

```bash
make -C <build-dir> run/sponge-compositor-c16 KERNEL=sel4 BOARD=pc
```

Runs the QMP, tasks, and tiling arms in three sequential boots with
`tier0_kind=native`. Receipts: native QMP full 16-step PASS
(`/tmp/opencode/g3-c16-qmp-final4.log`), native tasks PASS
(`/tmp/opencode/g3-c16-tasks-fixed.log`), native full 20-cycle tiling
PASS with close and reflow PASS on the cleaned tree
(`/tmp/opencode/g3-c16-tiling-final-verified.log`); the upstream QMP and
tasks anchors pass (`/tmp/opencode/g3-upstream-qmp.log`,
`/tmp/opencode/g3-upstream-tasks.log`) and upstream tiling is not
claimed. Intermittent close-time input failure (explicit): one recorded
tiling run failed at close (`/tmp/opencode/g3-c16-tiling-final2.log`,
the master close did not destroy its window) while `final3`,
`restoretablet2`, `final-fixed3`, and `final-verified` logs passed the
same close and reflow gates. The failure is documented, never relabeled. Host
receiver and guard-page repros pass (the receiver repro covers input
across view recreation; the guard-page repro proves compose never reads
past the bound buffer).

Evidence: `docs/evidence/phase16-compositor-index.md` in the umbrella.

## Limits (G1-G3)

Child views/associate, blit/panning, sync notifications,
keyboard/touch/global-key input, and pointer-shape reporting are outside
G1-G3 and are not claimed. Unsupported protocol corners warn loudly or
return documented safe results. Alpha blending (straight-alpha
`blit_alpha`) and WM/DE integration are proven on seL4 QEMU. Capacities
are 32 sessions, 64 views, and 3 damage rectangles. GPU acceleration,
product integration (G4-G5), and physical-hardware validation remain
uncompleted. Native is the default selection;
`SPONGE_COMPOSITOR=upstream` keeps the nitpicker fallback reachable as
an explicit override.
