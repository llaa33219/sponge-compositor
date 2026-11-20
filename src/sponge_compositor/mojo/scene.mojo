# SPDX-License-Identifier: Apache-2.0
#
# scene.mojo — G1/G2 scene model, view stack, command reducer,
# clipping/damage, surface binding, and composition driver for the Sponge
# compositor.
#
# Part of the Mojo-owned compositor core (plan D16C.3): the scene/session/
# view stack, the command reducer, clipping and damage computation, and the
# opaque painter driver live here in Mojo, not in C++. The C++ side (a later
# gate) adapts Genode Gui/Capture/Event sessions and calls the C ABI
# recorded in ../abi.h.
#
# G2 additions: per-view alpha/input-mask plane bindings (hit testing in
# input.mojo), the pointer/button/hover/focus/pressed/exclusive input state
# fields of Scene_state (reused by input.mojo; no module globals), and the
# fixed-width delivery-tuple queue drained by sc_input_step. State hygiene
# for input (dropping slot references on view destruction and queued
# deliveries on session close) lives here next to the tables it protects.
#
# State storage (caller-provided). The scene state lives in a raw byte
# block owned by the caller: the thin C++ Genode adapter allocates
# SC_SCENE_STORAGE_BYTES (8192) bytes of storage (e.g. as dataspace glue)
# and passes the block plus its size to sc_scene_init, which rejects a
# null or undersized block and initializes the Mojo-owned `Scene_state` in
# place. Every other exported entry point takes that same block as its
# first argument and interprets it as `Scene_state`. C++ merely provides
# the bytes and never models or inspects the scene: every scene field,
# reducer decision, stacking rule, clipping/damage computation, and
# painter operation remains Mojo-owned (D16C.3, D16C.10). This module
# references no storage symbols of its own, so the emitted objects keep an
# empty `nm -u` (no KGEN runtime, no libc, no heap, no List).
#
# Capacities are fixed: 8 sessions, 32 views, 3 damage rects, 32 queued
# delivery tuples (InlineArray only, no heap). Status codes follow ../abi.h:
#   0 = OK, -1 = bad args, -2 = capacity, -3 = unknown handle/id.
#
# View ids. `view_id` and `neighbor` are 64-bit end to end (UInt64 here,
# Genode::uint64_t in abi.h), matching the public Gui `View_id`
# (`Id_space<View_ref>::Id`, `unsigned long` in
# `os/include/gui_session/gui_session.h`). They are never narrowed.
#
# Indexing discipline. `InlineArray.__getitem__` carries a bounds assertion
# whose failure path drags in the format/print runtime (a non-empty `nm -u`).
# Every variable index into a table is therefore written in a form the
# optimizer can prove lies in `0 .. CAP-1`, which folds the assertion away.
# Power-of-two tables use `idx & (CAP - 1)` (bitwise AND: provably
# non-negative and below CAP); the 3-entry damage table uses floor-mod
# `(idx % CAP + CAP) % CAP` (plain `idx % CAP` is signed remainder and is
# NOT provably non-negative). Both forms are the identity on the valid
# range the surrounding logic establishes and keep a stray index inside
# the table instead of corrupting memory. Literal indices are left bare.
#
# In-OS subset (plan §6): no Python interop, no async, no POSIX I/O, no
# Genode-private headers, no `raises`, no inline asm, no module globals.
# Pointer spellings are the pinned toolchain set
# (`UnsafePointer.load/store`, `p[i]`, `p + i`).
#
# Build seam (plan D16C.4):
#   mojo build --emit object --relocation-model=pic -o scene.o scene.mojo

from std.sys import size_of

from painter import blit_alpha, blit_opaque, fill_rect

comptime CAP_SESSIONS = 32
comptime CAP_VIEWS = 64
comptime CAP_DAMAGE = 3
comptime STORAGE_BYTES = 8192

# G2 delivery-tuple queue (see ../abi.h "delivery tuples"): each queued
# event is SC_EVENT_TUPLE_INTS = 8 consecutive Int32 words, the queue
# holds CAP_TUPLES tuples flat (QUEUE_WORDS = CAP_TUPLES * TUPLE_INTS,
# power of two so the indexing discipline can use a bitwise AND).
comptime CAP_TUPLES = 32
comptime TUPLE_INTS = 8
comptime QUEUE_WORDS = 256

# Opcodes of the public Gui command surface
# (public header `os/include/gui_session/gui_session.h`:
#  GEOMETRY=0, OFFSET=1, FRONT=2, BACK=3, FRONT_OF=4, BEHIND_OF=5,
#  BACKGROUND=6, TITLE=7, NOP=8).
comptime OP_GEOMETRY = 0
comptime OP_OFFSET = 1
comptime OP_FRONT = 2
comptime OP_BACK = 3
comptime OP_FRONT_OF = 4
comptime OP_BEHIND_OF = 5


@fieldwise_init
struct Rect(Copyable, Movable):
    var x: Int32
    var y: Int32
    var w: Int32
    var h: Int32


@fieldwise_init
struct Session(Copyable, Movable):
    var active: Int32
    var kind: Int32


@fieldwise_init
struct View(Copyable, Movable):
    var active: Int32
    var session: Int32
    var view_id: UInt64
    var x: Int32
    var y: Int32
    var w: Int32
    var h: Int32
    var off_x: Int32
    var off_y: Int32
    var pitch: Int32
    # Bind-time texture extent: the bound buffer's own pixel dimensions,
    # set from the actual buffer at sc_view_bind_pixels /
    # sc_view_bind_surfaces (never from the view's current geometry) and
    # initialized to 0 with no buffer bound. Composition and input-mask
    # sampling floor-mod their source coordinates over this extent (the
    # upstream nitpicker frozen-size texture tiling), so a view that
    # outgrows its current buffer (maximize before the new buffer
    # arrives) wraps instead of reading outside the bound planes. A zero
    # extent is never sampled.
    var buf_w: Int32
    var buf_h: Int32
    var pixels: Optional[UnsafePointer[UInt32, MutUntrackedOrigin]]
    # G2 surface planes (see sc_view_bind_surfaces). The alpha plane makes
    # the view "alpha-enabled" for hit testing; the input-mask plane (byte
    # per pixel: nonzero = consume, 0 = fall through) is consulted only for
    # alpha-enabled views. Byte pitches, like the pixel pitch in bytes.
    var alpha: Optional[UnsafePointer[UInt8, MutUntrackedOrigin]]
    var alpha_pitch: Int32
    var mask: Optional[UnsafePointer[UInt8, MutUntrackedOrigin]]
    var mask_pitch: Int32


@fieldwise_init
struct Scene_state(Copyable, Movable):
    var screen_w: Int32
    var screen_h: Int32
    var bg: UInt32
    var sessions: InlineArray[Session, CAP_SESSIONS]
    var views: InlineArray[View, CAP_VIEWS]
    var stack: InlineArray[Int32, CAP_VIEWS]

    var stack_n: Int32
    var damage: InlineArray[Rect, 3]
    var damage_n: Int32
    # G2 input state (operated on by input.mojo; stored here so the
    # caller's single state block keeps holding every scene field).
    # Pointer position is clamped to the screen and starts at (0, 0).
    # `buttons` is the held mouse-button bitmask (bit 0 = BTN_LEFT,
    # 1 = BTN_RIGHT, 2 = BTN_MIDDLE); `key_cnt` counts all held keys.
    # `hovered`, `focused`, and `pressed` (the current input receiver)
    # are view-slot indices or -1. `pressed_session` is the session
    # captured when the press sequence started (the session-scoped
    # receiver fallback of input.mojo's emit_to_pressed; -1 when there
    # was no active pressed view). `exclusive` routes relative motion to
    # the focused view without moving the pointer (upstream "exclusive
    # input"). `drag` is set once a button-held sequence has seen motion.
    # `seq` is the delivery sequence counter (tuple slot 6). `evq` is the
    # flat delivery-tuple queue (evq_n tuples of TUPLE_INTS Int32 words).
    var ptr_x: Int32
    var ptr_y: Int32
    var buttons: Int32
    var key_cnt: Int32
    var hovered: Int32
    var focused: Int32
    var pressed: Int32
    var pressed_session: Int32
    var exclusive: Int32
    var drag: Int32
    var seq: Int32
    var evq_n: Int32
    var key_bits: InlineArray[UInt32, 16]
    var evq: InlineArray[Int32, 256]


# Scene_state fits under SC_SCENE_STORAGE_BYTES (32 sessions * 8 B +
# 64 views * 96 B + 64 * 4 B + 3 * 16 B + scalars + 16 * 4 B key bitmap +
# 256 * 4 B tuple queue ~= 7.7 KiB). C++ allocates the full 8192 bytes;
# the layout above is private to this module.


# ----------------------------------------------------------------------
# Caller-provided storage seam. The raw block is reinterpreted in place
# (same-address rebind; no copies, no aliases escaping the call) and a
# null block is rejected before any access.
# ----------------------------------------------------------------------


def is_null_state(p: UnsafePointer[UInt8, MutUntrackedOrigin]) -> Bool:
    var zero = Int(UInt64(1) - UInt64(1))
    var null_p = UnsafePointer[UInt8, MutUntrackedOrigin](unsafe_from_address=zero)
    return p == null_p


def state_of(raw: UnsafePointer[UInt8, MutUntrackedOrigin]) -> UnsafePointer[
    Scene_state, MutUntrackedOrigin
]:
    return rebind[UnsafePointer[Scene_state, MutUntrackedOrigin]](raw)


# ----------------------------------------------------------------------
# Clipping helpers (Int64 intermediates so hostile coordinates cannot
# overflow; every result is clamped back to the Int32 range).
# ----------------------------------------------------------------------


def min64(a: Int64, b: Int64) -> Int64:
    if a < b:
        return a
    return b


def max64(a: Int64, b: Int64) -> Int64:
    if a > b:
        return a
    return b


def clamp_i32(v: Int64) -> Int32:
    if v <= 0:
        return 0
    if v >= 2147483647:
        return 2147483647
    return Int32(v)


def rect_is_empty(r: Rect) -> Bool:
    return r.w <= 0 or r.h <= 0


def rect_intersect(a: Rect, b: Rect) -> Rect:
    var x1 = max64(Int64(a.x), Int64(b.x))
    var y1 = max64(Int64(a.y), Int64(b.y))
    var x2 = min64(Int64(a.x) + Int64(a.w), Int64(b.x) + Int64(b.w))
    var y2 = min64(Int64(a.y) + Int64(a.h), Int64(b.y) + Int64(b.h))
    return Rect(Int32(x1), Int32(y1), clamp_i32(x2 - x1), clamp_i32(y2 - y1))


def rect_contains(outer: Rect, inner: Rect) -> Bool:
    return Int64(inner.x) >= Int64(outer.x) and Int64(inner.y) >= Int64(
        outer.y
    ) and Int64(inner.x) + Int64(inner.w) <= Int64(outer.x) + Int64(
        outer.w
    ) and Int64(inner.y) + Int64(inner.h) <= Int64(outer.y) + Int64(outer.h)


def rect_touches(a: Rect, b: Rect) -> Bool:
    return Int64(a.x) <= Int64(b.x) + Int64(b.w) and Int64(b.x) <= Int64(
        a.x
    ) + Int64(a.w) and Int64(a.y) <= Int64(b.y) + Int64(b.h) and Int64(
        b.y
    ) <= Int64(a.y) + Int64(a.h)


def rect_union(a: Rect, b: Rect) -> Rect:
    var x1 = min64(Int64(a.x), Int64(b.x))
    var y1 = min64(Int64(a.y), Int64(b.y))
    var x2 = max64(Int64(a.x) + Int64(a.w), Int64(b.x) + Int64(b.w))
    var y2 = max64(Int64(a.y) + Int64(a.h), Int64(b.y) + Int64(b.h))
    return Rect(Int32(x1), Int32(y1), clamp_i32(x2 - x1), clamp_i32(y2 - y1))


def rect_area(r: Rect) -> Int64:
    return Int64(r.w) * Int64(r.h)


def rect_growth(a: Rect, r: Rect) -> Int64:
    return rect_area(rect_union(a, r)) - rect_area(a)


# ----------------------------------------------------------------------
# View lookup and stack maintenance. The stack holds view-slot indices
# bottom-up: stack[0] paints first (bottom), stack[stack_n - 1] paints
# last (top). All validation happens before any mutation so stale ids
# return -3 without corrupting state.
# ----------------------------------------------------------------------


def find_view(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], handle: Int32, view_id: UInt64
) -> Int32:
    var j = 0
    while j < CAP_VIEWS:
        var v = s[0].views[j & (CAP_VIEWS - 1)].copy()
        if v.active != 0 and v.session == handle and v.view_id == view_id:
            return Int32(j)
        j += 1
    return -1


def stack_find(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32
) -> Int32:
    var i = 0
    while i < Int(s[0].stack_n):
        if s[0].stack[i & (CAP_VIEWS - 1)] == slot:
            return Int32(i)
        i += 1
    return -1


def stack_remove(s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32):
    var idx = stack_find(s, slot)
    if idx < 0:
        return
    var k = Int(idx)
    while k + 1 < Int(s[0].stack_n):
        s[0].stack[k & (CAP_VIEWS - 1)] = s[0].stack[(k + 1) & (CAP_VIEWS - 1)]
        k += 1
    s[0].stack_n -= Int32(1)


def stack_insert_top(s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32):
    if s[0].stack_n >= Int32(CAP_VIEWS):
        return
    s[0].stack[Int(s[0].stack_n) & (CAP_VIEWS - 1)] = slot
    s[0].stack_n += Int32(1)


def stack_insert_bottom(s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32):
    if s[0].stack_n >= Int32(CAP_VIEWS):
        return
    var k = Int(s[0].stack_n)
    while k > 0:
        s[0].stack[k & (CAP_VIEWS - 1)] = s[0].stack[(k - 1) & (CAP_VIEWS - 1)]
        k -= 1
    s[0].stack[0] = slot
    s[0].stack_n += Int32(1)


def stack_insert_at(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], at: Int, slot: Int32
):
    if s[0].stack_n >= Int32(CAP_VIEWS):
        return
    var k = Int(s[0].stack_n)
    while k > at:
        s[0].stack[k & (CAP_VIEWS - 1)] = s[0].stack[(k - 1) & (CAP_VIEWS - 1)]
        k -= 1
    s[0].stack[at & (CAP_VIEWS - 1)] = slot
    s[0].stack_n += Int32(1)


def stack_insert_above(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32, nb: Int32
):
    var idx = stack_find(s, nb)
    if idx < 0:
        return
    stack_insert_at(s, Int(idx) + 1, slot)


def stack_insert_below(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32, nb: Int32
):
    var idx = stack_find(s, nb)
    if idx < 0:
        return
    stack_insert_at(s, Int(idx), slot)


# ----------------------------------------------------------------------
# Damage: at most CAP_DAMAGE bounding rects. New damage is clipped to the
# screen, absorbed when contained, merged into overlapping/touching
# damage, and otherwise kept as one of at most CAP_DAMAGE bounding rects
# (when full, the new rect merges into the pending rect whose bounding
# box grows the least). Merged rects are re-coalesced pairwise so the
# list stays at most three bounding boxes.
#
# Damage producers: explicit sc_damage_add calls plus every scene event
# that changes what is on screen - view creation, pixel (re)bind, view
# destruction, per-view session-close removal, GEOMETRY (old and new
# rect), OFFSET, and stacking changes (FRONT/BACK/FRONT_OF/BEHIND_OF,
# which repaint the moved view's area so the correct top view appears).
# Unsupported no-op opcodes produce no damage. This keeps Capture
# consumers correct even when they consume damage before a view exists
# or before its buffer is bound.
# ----------------------------------------------------------------------


def damage_coalesce(s: UnsafePointer[Scene_state, MutUntrackedOrigin]):
    var merged = 1
    while merged != 0:
        merged = 0
        var i = 0
        while i < Int(s[0].damage_n):
            var j = i + 1
            while j < Int(s[0].damage_n):
                if rect_touches(
                    s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE],
                    s[0].damage[(j % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE],
                ):
                    s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = rect_union(
                        s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE],
                        s[0].damage[(j % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE],
                    )
                    var k = j
                    while k + 1 < Int(s[0].damage_n):
                        s[0].damage[(k % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = s[
                            0
                        ].damage[
                            ((k + 1) % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE
                        ].copy()
                        k += 1
                    s[0].damage_n -= Int32(1)
                    merged = 1
                else:
                    j += 1
            i += 1


def damage_add_rect(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin],
    x: Int32,
    y: Int32,
    w: Int32,
    h: Int32,
) -> Int32:
    if w < 0 or h < 0:
        return -1
    if w == 0 or h == 0:
        return 0
    var screen = Rect(0, 0, s[0].screen_w, s[0].screen_h)
    var r = rect_intersect(Rect(x, y, w, h), screen)
    if rect_is_empty(r):
        return 0
    var i = 0
    while i < Int(s[0].damage_n):
        if rect_contains(s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], r):
            return 0
        i += 1
    var j = 0
    while j < Int(s[0].damage_n):
        if rect_touches(s[0].damage[(j % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], r):
            s[0].damage[(j % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = rect_union(
                s[0].damage[(j % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], r
            )
            damage_coalesce(s)
            return 0
        j += 1
    if s[0].damage_n < Int32(CAP_DAMAGE):
        s[0].damage[(Int(s[0].damage_n) % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = (
            r.copy()
        )
        s[0].damage_n += Int32(1)
        return 0
    var best = 0
    var best_growth = rect_growth(s[0].damage[0], r)
    var k = 1
    while k < CAP_DAMAGE:
        var g = rect_growth(s[0].damage[(k % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], r)
        if g < best_growth:
            best = k
            best_growth = g
        k += 1
    s[0].damage[(best % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = rect_union(
        s[0].damage[(best % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], r
    )
    damage_coalesce(s)
    return 0


# Damage the screen area of a view slot (its current rect, clipped to
# the screen by damage_add_rect). Lifecycle events (create, bind/rebind,
# destroy, session-close removal) and view-changing commands (GEOMETRY,
# OFFSET, stacking) route through this so a Capture that consumed damage
# before such an event still repaints the affected area on the next
# capture.
def damage_view(s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32):
    _ = damage_add_rect(
        s,
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].x,
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].y,
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].w,
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].h,
    )


# ----------------------------------------------------------------------
# G2 input-state hygiene. The input machine (input.mojo) addresses views
# by slot, so destroying a view silently clears any hovered/focused/
# pressed reference to its slot (no delivery is claimed: the session is
# going away). Closing a session additionally drops queued delivery
# tuples addressed to that session so input can never leak to a stale
# session handle. Tuple layout: TUPLE_INTS words, session handle at
# word 0 (see ../abi.h "delivery tuples").
# ----------------------------------------------------------------------


def clear_slot_refs(s: UnsafePointer[Scene_state, MutUntrackedOrigin], slot: Int32):
    if s[0].hovered == slot:
        s[0].hovered = -1
    if s[0].focused == slot:
        s[0].focused = -1
    if s[0].pressed == slot:
        s[0].pressed = -1
    # View destruction clears the slot references above but
    # intentionally preserves `pressed_session`: emit_to_pressed's
    # session-scoped receiver fallback must survive a decorator view
    # destroy+recreate (upstream keeps a session-scoped View_owner
    # receiver). sc_session_close clears it when the session goes away.


def drop_queue_for_session(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], handle: Int32
):
    var w = 0
    var r = 0
    while r < Int(s[0].evq_n):
        if s[0].evq[(r * TUPLE_INTS) & (QUEUE_WORDS - 1)] != handle:
            if w != r:
                var k = 0
                while k < TUPLE_INTS:
                    s[0].evq[((w * TUPLE_INTS) + k) & (QUEUE_WORDS - 1)] = s[0].evq[
                        ((r * TUPLE_INTS) + k) & (QUEUE_WORDS - 1)
                    ]
                    k += 1
            w += 1
        r += 1
    s[0].evq_n = Int32(w)


# ----------------------------------------------------------------------
# Exported C ABI (contract: ../abi.h). Every entry point receives the
# caller-provided state block as its first argument and rejects a null
# block before touching it.
# ----------------------------------------------------------------------


@export("sc_scene_init")
def sc_scene_init(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    state_size: UInt64,
    screen_w: Int32,
    screen_h: Int32,
    bg_r: Int32,
    bg_g: Int32,
    bg_b: Int32,
) abi("C") -> Int32:
    comptime assert (
        size_of[Scene_state]() <= STORAGE_BYTES
    ), "Scene_state must fit SC_SCENE_STORAGE_BYTES"
    if is_null_state(state):
        return -1
    if state_size < UInt64(STORAGE_BYTES):
        return -1
    if screen_w <= 0 or screen_h <= 0:
        return -1
    if (
        bg_r < 0
        or bg_r > 255
        or bg_g < 0
        or bg_g > 255
        or bg_b < 0
        or bg_b > 255
    ):
        return -1
    var s = state_of(state)
    s[0].screen_w = screen_w
    s[0].screen_h = screen_h
    s[0].bg = (UInt32(bg_r) << 16) | (UInt32(bg_g) << 8) | UInt32(bg_b)
    s[0].stack_n = 0
    s[0].damage_n = 0
    s[0].ptr_x = 0
    s[0].ptr_y = 0
    s[0].buttons = 0
    s[0].key_cnt = 0
    s[0].hovered = -1
    s[0].focused = -1
    s[0].pressed = -1
    s[0].pressed_session = -1
    s[0].exclusive = 0
    s[0].drag = 0
    s[0].seq = 0
    s[0].evq_n = 0
    var i = 0
    while i < CAP_SESSIONS:
        s[0].sessions[i & (CAP_SESSIONS - 1)].active = 0
        s[0].sessions[i & (CAP_SESSIONS - 1)].kind = 0
        i += 1
    var j = 0
    while j < CAP_VIEWS:
        s[0].views[j & (CAP_VIEWS - 1)].active = 0
        s[0].views[j & (CAP_VIEWS - 1)].session = 0
        s[0].views[j & (CAP_VIEWS - 1)].view_id = UInt64(0)
        s[0].views[j & (CAP_VIEWS - 1)].x = 0
        s[0].views[j & (CAP_VIEWS - 1)].y = 0
        s[0].views[j & (CAP_VIEWS - 1)].w = 0
        s[0].views[j & (CAP_VIEWS - 1)].h = 0
        s[0].views[j & (CAP_VIEWS - 1)].off_x = 0
        s[0].views[j & (CAP_VIEWS - 1)].off_y = 0
        s[0].views[j & (CAP_VIEWS - 1)].pitch = 0
        s[0].views[j & (CAP_VIEWS - 1)].buf_w = 0
        s[0].views[j & (CAP_VIEWS - 1)].buf_h = 0
        s[0].views[j & (CAP_VIEWS - 1)].pixels = None
        s[0].views[j & (CAP_VIEWS - 1)].alpha = None
        s[0].views[j & (CAP_VIEWS - 1)].alpha_pitch = 0
        s[0].views[j & (CAP_VIEWS - 1)].mask = None
        s[0].views[j & (CAP_VIEWS - 1)].mask_pitch = 0
        j += 1
    var kb = 0
    while kb < 16:
        s[0].key_bits[kb & 15] = UInt32(0)
        kb += 1
    var k = 0
    while k < CAP_VIEWS:
        s[0].stack[k & (CAP_VIEWS - 1)] = 0
        k += 1
    var m = 0
    while m < CAP_DAMAGE:
        s[0].damage[(m % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE] = Rect(0, 0, 0, 0)
        m += 1
    return 0


@export("sc_session_open")
def sc_session_open(state: UnsafePointer[UInt8, MutUntrackedOrigin], kind: Int32) abi(
    "C"
) -> Int32:
    if is_null_state(state):
        return -1
    if kind < 0 or kind > 2:
        return -1
    var s = state_of(state)
    var i = 0
    while i < CAP_SESSIONS:
        if s[0].sessions[i & (CAP_SESSIONS - 1)].active == 0:
            s[0].sessions[i & (CAP_SESSIONS - 1)].active = 1
            s[0].sessions[i & (CAP_SESSIONS - 1)].kind = kind
            return Int32(i)
        i += 1
    return -2


@export("sc_session_close")
def sc_session_close(state: UnsafePointer[UInt8, MutUntrackedOrigin], handle: Int32) abi(
    "C"
) -> Int32:
    if is_null_state(state):
        return -1
    if handle < 0 or handle >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(handle) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var j = 0
    while j < CAP_VIEWS:
        var v = s[0].views[j & (CAP_VIEWS - 1)].copy()
        if v.active != 0 and v.session == handle:
            stack_remove(s, Int32(j))
            s[0].views[j & (CAP_VIEWS - 1)].active = 0
            s[0].views[j & (CAP_VIEWS - 1)].pixels = None
            s[0].views[j & (CAP_VIEWS - 1)].buf_w = 0
            s[0].views[j & (CAP_VIEWS - 1)].buf_h = 0
            s[0].views[j & (CAP_VIEWS - 1)].alpha = None
            s[0].views[j & (CAP_VIEWS - 1)].mask = None
            clear_slot_refs(s, Int32(j))
            _ = damage_add_rect(s, v.x, v.y, v.w, v.h)
        j += 1
    s[0].sessions[Int(handle) & (CAP_SESSIONS - 1)].active = 0
    # The session fallback dies with the session (view destruction
    # preserves it; closing the session does not).
    if s[0].pressed_session == handle:
        s[0].pressed_session = -1
    drop_queue_for_session(s, handle)
    return 0


@export("sc_view_create")
def sc_view_create(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    session: Int32,
    view_id: UInt64,
    x: Int32,
    y: Int32,
    w: Int32,
    h: Int32,
    front: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    if w < 0 or h < 0:
        return -1
    var dup = find_view(s, session, view_id)
    if dup >= 0:
        return -1
    var slot: Int32 = -1
    var k = 0
    while k < CAP_VIEWS:
        if s[0].views[k & (CAP_VIEWS - 1)].active == 0:
            slot = Int32(k)
            break
        k += 1
    if slot < 0:
        return -2
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].active = 1
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].session = session
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].view_id = view_id
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].x = x
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].y = y
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].w = w
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].h = h
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].off_x = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].off_y = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pitch = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_w = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_h = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pixels = None
    if front != 0:
        stack_insert_top(s, slot)
    else:
        stack_insert_bottom(s, slot)
    _ = damage_add_rect(s, x, y, w, h)
    return 0



@export("sc_view_destroy")
def sc_view_destroy(
    state: UnsafePointer[UInt8, MutUntrackedOrigin], session: Int32, view_id: UInt64
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var slot = find_view(s, session, view_id)
    if slot < 0:
        return -3
    var v = s[0].views[Int(slot) & (CAP_VIEWS - 1)].copy()
    stack_remove(s, slot)
    _ = damage_add_rect(s, v.x, v.y, v.w, v.h)
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].active = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pixels = None
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_w = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_h = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha = None
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask = None
    clear_slot_refs(s, slot)
    return 0



@export("sc_view_bind_pixels")
def sc_view_bind_pixels(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    session: Int32,
    view_id: UInt64,
    pixels: Optional[UnsafePointer[UInt32, MutUntrackedOrigin]],
    pitch: Int32,
    buf_w: Int32,
    buf_h: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var slot = find_view(s, session, view_id)
    if slot < 0:
        return -3
    if not pixels:
        return -1
    if pitch <= 0 or (pitch & 3) != 0:
        return -1
    # The extent describes the bound buffer (not the view's geometry)
    # and must be positive: composition and mask sampling wrap over it.
    if buf_w <= 0 or buf_h <= 0:
        return -1
    var px = pixels.value()
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pixels = px
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pitch = pitch
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_w = buf_w
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_h = buf_h
    # Compatibility binding: replaces the whole surface set, so any
    # alpha/input-mask planes from sc_view_bind_surfaces are dropped and
    # the view hit-tests as opaque again.
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha = None
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha_pitch = 0
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask = None
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask_pitch = 0
    var v = s[0].views[Int(slot) & (CAP_VIEWS - 1)].copy()
    _ = damage_add_rect(s, v.x, v.y, v.w, v.h)
    return 0



@export("sc_view_bind_surfaces")
def sc_view_bind_surfaces(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    session: Int32,
    view_id: UInt64,
    pixels: Optional[UnsafePointer[UInt32, MutUntrackedOrigin]],
    pixel_pitch: Int32,
    buf_w: Int32,
    buf_h: Int32,
    alpha: Optional[UnsafePointer[UInt8, MutUntrackedOrigin]],
    alpha_pitch: Int32,
    input_mask: Optional[UnsafePointer[UInt8, MutUntrackedOrigin]],
    input_mask_pitch: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var slot = find_view(s, session, view_id)
    if slot < 0:
        return -3
    if not pixels:
        return -1
    if pixel_pitch <= 0 or (pixel_pitch & 3) != 0:
        return -1
    # The extent describes the bound buffer (shared by all planes, not
    # the view's geometry) and must be positive: composition and mask
    # sampling wrap over it.
    if buf_w <= 0 or buf_h <= 0:
        return -1
    # Optional planes are all-or-nothing: a bound byte plane needs a
    # positive pitch; an unbound plane is stored as absent.
    if alpha:
        if alpha_pitch <= 0:
            return -1
    if input_mask:
        if input_mask_pitch <= 0:
            return -1
    var px = pixels.value()
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pixels = px
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].pitch = pixel_pitch
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_w = buf_w
    s[0].views[Int(slot) & (CAP_VIEWS - 1)].buf_h = buf_h
    if alpha:
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha = alpha.value()
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha_pitch = alpha_pitch
    else:
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha = None
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].alpha_pitch = 0
    if input_mask:
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask = input_mask.value()
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask_pitch = input_mask_pitch
    else:
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask = None
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].mask_pitch = 0
    var v = s[0].views[Int(slot) & (CAP_VIEWS - 1)].copy()
    _ = damage_add_rect(s, v.x, v.y, v.w, v.h)
    return 0



@export("sc_apply_command")
def sc_apply_command(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    session: Int32,
    opcode: Int32,
    view_id: UInt64,
    x: Int32,
    y: Int32,
    w: Int32,
    h: Int32,
    neighbor: UInt64,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var slot = find_view(s, session, view_id)
    if slot < 0:
        return -3
    if opcode == Int32(OP_GEOMETRY):
        if w < 0 or h < 0:
            return -1
        var old_x = s[0].views[Int(slot) & (CAP_VIEWS - 1)].x
        var old_y = s[0].views[Int(slot) & (CAP_VIEWS - 1)].y
        var old_w = s[0].views[Int(slot) & (CAP_VIEWS - 1)].w
        var old_h = s[0].views[Int(slot) & (CAP_VIEWS - 1)].h
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].x = x
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].y = y
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].w = w
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].h = h
        _ = damage_add_rect(s, old_x, old_y, old_w, old_h)
        damage_view(s, slot)
        return 0
    if opcode == Int32(OP_OFFSET):
        if x < 0 or y < 0:
            return -1
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].off_x = x
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].off_y = y
        damage_view(s, slot)
        return 0
    if opcode == Int32(OP_FRONT):
        stack_remove(s, slot)
        stack_insert_top(s, slot)
        damage_view(s, slot)
        return 0
    if opcode == Int32(OP_BACK):
        stack_remove(s, slot)
        stack_insert_bottom(s, slot)
        damage_view(s, slot)
        return 0
    if opcode == Int32(OP_FRONT_OF):
        if neighbor == view_id:
            return 0
        var nb = find_view(s, session, neighbor)
        if nb < 0:
            return -3
        stack_remove(s, slot)
        stack_insert_above(s, slot, nb)
        damage_view(s, slot)
        return 0
    if opcode == Int32(OP_BEHIND_OF):
        if neighbor == view_id:
            return 0
        var nb = find_view(s, session, neighbor)
        if nb < 0:
            return -3
        stack_remove(s, slot)
        stack_insert_below(s, slot, nb)
        damage_view(s, slot)
        return 0
    # BACKGROUND, TITLE, NOP and any unknown opcode: state-safe no-op.
    return 0


@export("sc_damage_add")
def sc_damage_add(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    x: Int32,
    y: Int32,
    w: Int32,
    h: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    var s = state_of(state)
    return damage_add_rect(s, x, y, w, h)


@export("sc_damage_count")
def sc_damage_count(state: UnsafePointer[UInt8, MutUntrackedOrigin]) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    var s = state_of(state)
    return s[0].damage_n


@export("sc_compose")
def sc_compose(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    screen_pixels: Optional[UnsafePointer[UInt32, MutUntrackedOrigin]],
    pitch: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if not screen_pixels:
        return -1
    if pitch <= 0 or (pitch & 3) != 0:
        return -1
    var dst = screen_pixels.value()
    var s = state_of(state)
    if s[0].screen_w <= 0 or s[0].screen_h <= 0:
        return 0
    if s[0].damage_n == 0:
        return 0
    var stride = pitch / Int32(4)
    var screen = Rect(0, 0, s[0].screen_w, s[0].screen_h)
    var d = 0
    while d < Int(s[0].damage_n):
        var r = rect_intersect(
            s[0].damage[(d % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE], screen
        )
        if not rect_is_empty(r):
            fill_rect(dst, stride, r.x, r.y, r.w, r.h, s[0].bg)
            var k = 0
            while k < Int(s[0].stack_n):
                var slot = s[0].stack[k & (CAP_VIEWS - 1)]
                var v = s[0].views[Int(slot) & (CAP_VIEWS - 1)].copy()
                if v.active != 0:
                    if v.pixels and v.buf_w > 0 and v.buf_h > 0:
                        var vp = v.pixels.value()
                        var vr = rect_intersect(Rect(v.x, v.y, v.w, v.h), r)
                        if not rect_is_empty(vr):
                            var vstride = v.pitch / Int32(4)
                            var sx = vr.x - v.x + v.off_x
                            var sy = vr.y - v.y + v.off_y
                            if v.alpha:
                                blit_alpha(
                                    dst,
                                    stride,
                                    vp,
                                    vstride,
                                    v.buf_w,
                                    v.buf_h,
                                    v.alpha.value(),
                                    v.alpha_pitch,
                                    vr.x,
                                    vr.y,
                                    sx,
                                    sy,
                                    vr.w,
                                    vr.h,
                                )
                            else:
                                blit_opaque(
                                    dst,
                                    stride,
                                    vp,
                                    vstride,
                                    v.buf_w,
                                    v.buf_h,
                                    vr.x,
                                    vr.y,
                                    sx,
                                    sy,
                                    vr.w,
                                    vr.h,
                                )
                k += 1
        d += 1
    return 0


@export("sc_damage_fetch")
def sc_damage_fetch(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    rects_out: Optional[UnsafePointer[Int32, MutUntrackedOrigin]],
    max_rects: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if max_rects < 0:
        return -1
    var s = state_of(state)
    var n = s[0].damage_n
    if n == 0:
        return 0
    if not rects_out:
        return -1
    var out = rects_out.value()
    if max_rects < n:
        return -2
    var i = 0
    while i < Int(n):
        (out + Int(4 * i)).store(
            s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE].x
        )
        (out + Int(4 * i) + 1).store(
            s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE].y
        )
        (out + Int(4 * i) + 2).store(
            s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE].w
        )
        (out + Int(4 * i) + 3).store(
            s[0].damage[(i % CAP_DAMAGE + CAP_DAMAGE) % CAP_DAMAGE].h
        )
        i += 1
    s[0].damage_n = 0
    return n
