# SPDX-License-Identifier: Apache-2.0
#
# input.mojo — G2 input state, alpha/input-mask hit testing, pointer and
# focus state, and the flattened input-event reducer for the Sponge
# compositor.
#
# Part of the Mojo-owned compositor core (plan D16C.3). This module owns
# every input decision: top-down hit testing over the view stack (bounds
# plus alpha/input-mask semantics from the public
# `framebuffer_session/framebuffer_session.h` layout), the pointer,
# button, hovered/focused/pressed, and exclusive state (stored in the
# caller-provided Scene_state block of scene.mojo; no module globals, no
# inline asm), and the reduction of one flattened Input::Event into
# fixed-width delivery tuples (../abi.h "delivery tuples"). A C++
# input_pipeline calls the exports below and dispatches the tuples to
# per-session Input::Session_component queues.
#
# State storage. All state lives in the caller-provided 8192-byte block
# initialized by sc_scene_init; every entry point here receives that
# block and rejects a null one before touching it (same seam as G1).
#
# Delivery tuples. Each delivered event is 8 consecutive Int32 words
# (SC_EVENT_TUPLE_INTS): session, type, x, y, key, codepoint, seq,
# reserved. `type` uses the Input::Event::Type values of the public
# `os/include/input/event.h`. `seq` is a per-state delivery sequence
# number (1-based, strictly increasing in emission order) so G2 probes
# can order-check deliveries; `reserved` is 0. Tuples are queued FIFO in
# the state block and drained by sc_input_step into the caller's buffer.
#
# Input semantics follow upstream nitpicker's public-server behavior
# (`os/src/server/nitpicker/user_state.cc`) where G2 covers it:
#   - relative motion converts to absolute (origin (0, 0) before the
#     first motion) and is clamped to the screen; the delivered motion
#     carries the clamped screen position,
#   - click focus emits FOCUS_LEAVE to the old focus before
#     Absolute_motion + Focus_enter on the newly clicked view,
#   - hover changes emit HOVER_LEAVE then the pointer position to the
#     new view both from the hover update and from the forwarded event
#     (two identical motion tuples, exactly as upstream delivers them),
#   - while a button is held the stream stays with the press receiver
#     ("pressed") and hover updates are suppressed; a release ends the
#     sequence with a hover update plus upstream's extra Hover_leave to
#     a drag receiver that is no longer hovered,
#   - a mouse-button press resets the receiver to the focused view
#     (click focus), wheel goes to the receiver, and SEQ_NUMBER events
#     go to the hovered view or the focused view.
#
# Documented G2 simplifications (not silently claimed otherwise):
# pointer position starts at (0, 0) instead of upstream's "nowhere",
# domain/hover-always/global-key/transient-focus/touch policies are not
# modeled (single implicit domain; TOUCH/TOUCH_RELEASE/AXIS events are
# accepted with no delivery), non-button keys are tracked uniformly
# (upstream's invalid-key corner is not modeled), and sc_focus_forward
# applies immediately (upstream defers during drag; the pipeline should
# serialize focus changes outside button sequences).
#
# In-OS subset (plan §6): no heap, no List, no Python interop, no async,
# no POSIX I/O, no Genode-private headers, no `raises`, no inline asm,
# no module globals. Pointer spellings are the pinned toolchain set
# (`UnsafePointer.load/store`, `p[i]`, `p + i`). Indexing discipline
# (scene.mojo): every variable table index is written with a bitwise AND
# mask so the bounds assertion folds away (no format/print runtime).
#
# Build seam (plan D16C.4):
#   mojo build --emit object --relocation-model=pic -o input.o input.mojo

from scene import (
    Scene_state,
    CAP_SESSIONS,
    CAP_VIEWS,
    CAP_TUPLES,
    TUPLE_INTS,
    QUEUE_WORDS,
    state_of,
    is_null_state,
    find_view,
)

# Flattened Input::Event::Type values (public header
# `os/include/input/event.h`: INVALID, PRESS, RELEASE, REL_MOTION,
# ABS_MOTION, WHEEL, FOCUS_ENTER, FOCUS_LEAVE, HOVER_LEAVE, TOUCH,
# TOUCH_RELEASE, SEQ_NUMBER, AXIS).
comptime EV_INVALID = 0
comptime EV_PRESS = 1
comptime EV_RELEASE = 2
comptime EV_REL_MOTION = 3
comptime EV_ABS_MOTION = 4
comptime EV_WHEEL = 5
comptime EV_FOCUS_ENTER = 6
comptime EV_FOCUS_LEAVE = 7
comptime EV_HOVER_LEAVE = 8
comptime EV_TOUCH = 9
comptime EV_TOUCH_RELEASE = 10
comptime EV_SEQ_NUMBER = 11
comptime EV_AXIS = 12

# Keycode window and mouse-button codes of the public
# `os/include/input/keycodes.h` (KEY_MAX = 0x1ff; BTN_LEFT..BTN_MIDDLE =
# 0x110..0x112 steer click focus and the button bitmask).
comptime KEY_MAX = 512
comptime BTN_LEFT = 272
comptime BTN_RIGHT = 273
comptime BTN_MIDDLE = 274

# One input event produces at most this many delivery tuples (a
# click-focus press: FOCUS_LEAVE + Absolute_motion + Focus_enter +
# PRESS). sc_input_step reserves this many queue slots before consuming
# an event, so an event is never lost to a full queue.
comptime MAX_STEP_TUPLES = 8


def is_mouse_button(key: Int32) -> Bool:
    return key >= Int32(BTN_LEFT) and key <= Int32(BTN_MIDDLE)


def key_word(key: Int32) -> Int:
    # 32 keys per word; the & 15 keeps the index provably 0..15.
    return Int((key >> 5) & 15)


def key_bit(key: Int32) -> UInt32:
    return UInt32(1) << UInt32(key & 31)


# ----------------------------------------------------------------------
# Delivery-tuple queue (flat Int32 ring in Scene_state; FIFO). Tuple
# word 0 is the target session handle - scene.mojo drops queued tuples
# of a session when it closes.
# ----------------------------------------------------------------------


def queue_tuple(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin],
    session: Int32,
    ev_type: Int32,
    x: Int32,
    y: Int32,
    key: Int32,
    codepoint: Int32,
):
    # Callers reserved queue slots (sc_input_step: MAX_STEP_TUPLES per
    # event; sc_focus_forward: 2), so evq_n < CAP_TUPLES holds here.
    var base = Int(s[0].evq_n) * TUPLE_INTS
    s[0].evq[(base + 0) & (QUEUE_WORDS - 1)] = session
    s[0].evq[(base + 1) & (QUEUE_WORDS - 1)] = ev_type
    s[0].evq[(base + 2) & (QUEUE_WORDS - 1)] = x
    s[0].evq[(base + 3) & (QUEUE_WORDS - 1)] = y
    s[0].evq[(base + 4) & (QUEUE_WORDS - 1)] = key
    s[0].evq[(base + 5) & (QUEUE_WORDS - 1)] = codepoint
    s[0].evq[(base + 6) & (QUEUE_WORDS - 1)] = s[0].seq + 1
    s[0].evq[(base + 7) & (QUEUE_WORDS - 1)] = 0
    s[0].seq += 1
    s[0].evq_n += 1


def queue_flush(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin],
    dst: UnsafePointer[Int32, MutUntrackedOrigin],
    max_out: Int32,
) -> Int32:
    # Write the oldest n tuples to `out` and shift the rest to the
    # front. Tuples that do not fit stay queued and are emitted by later
    # calls (including SC_EV_INVALID drains).
    var n = s[0].evq_n
    if n > max_out:
        n = max_out
    if n <= 0:
        return 0
    var words = Int(n) * TUPLE_INTS
    var i = 0
    while i < words:
        (dst + i).store(s[0].evq[i & (QUEUE_WORDS - 1)])
        i += 1
    var remaining = Int(s[0].evq_n) - Int(n)
    var k = 0
    while k < remaining * TUPLE_INTS:
        s[0].evq[k & (QUEUE_WORDS - 1)] = s[0].evq[
            (k + Int(n) * TUPLE_INTS) & (QUEUE_WORDS - 1)
        ]
        k += 1
    s[0].evq_n = Int32(remaining)
    return n


# ----------------------------------------------------------------------
# Hit testing. Top-down over the view stack (stack[stack_n - 1] is the
# top view). Bounds first: the point must lie inside the view's screen
# rect. Then surface semantics (public framebuffer_session mask layout):
# a view without an alpha plane is opaque and consumes every point
# inside its rect; an alpha-enabled view consults its input-mask plane
# at the buffer coordinate (screen - view origin + buffer offset, the
# same mapping sc_compose uses) wrapped by the view's bind-time
# (buf_w, buf_h) extent - the same tiling wrap as composition, so hit
# testing never reads outside the bound mask plane while a geometry
# change awaits its new buffer - where mask byte 0 falls through to the
# views below and any other byte consumes. An alpha-enabled view without
# a mask plane consumes (there is no hole to consult). A mask with a
# zero extent is not sampled (nothing bound to sample) and consumes like
# a mask-free alpha view.
# ----------------------------------------------------------------------


def hit_test(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], px: Int32, py: Int32
) -> Int32:
    var i = Int(s[0].stack_n)
    while i > 0:
        i -= 1
        var slot = s[0].stack[i & (CAP_VIEWS - 1)]
        if slot < 0 or slot >= Int32(CAP_VIEWS):
            continue
        var v = s[0].views[Int(slot) & (CAP_VIEWS - 1)].copy()
        if v.active == 0:
            continue
        if Int64(px) < Int64(v.x) or Int64(py) < Int64(v.y):
            continue
        if Int64(px) >= Int64(v.x) + Int64(v.w):
            continue
        if Int64(py) >= Int64(v.y) + Int64(v.h):
            continue
        if v.alpha:
            if v.mask:
                # Zero extent: nothing bound to sample - do not read.
                if v.buf_w <= 0 or v.buf_h <= 0:
                    return slot
                var lx = Int64(px) - Int64(v.x) + Int64(v.off_x)
                var ly = Int64(py) - Int64(v.y) + Int64(v.off_y)
                # Tiling wrap (identical to composition's blits): the
                # sample lands inside the bound mask extent even when
                # the view geometry outgrew the current buffer.
                var mx = ((lx % Int64(v.buf_w)) + Int64(v.buf_w)) % Int64(
                    v.buf_w
                )
                var my = ((ly % Int64(v.buf_h)) + Int64(v.buf_h)) % Int64(
                    v.buf_h
                )
                var m = v.mask.value()
                var sample = (m + Int(my) * Int(v.mask_pitch) + Int(mx)).load()
                if sample == 0:
                    continue
            return slot
        return slot
    return -1


def emit_to_slot(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin],
    slot: Int32,
    ev_type: Int32,
    x: Int32,
    y: Int32,
    key: Int32,
    codepoint: Int32,
):
    if slot < 0 or slot >= Int32(CAP_VIEWS):
        return
    if s[0].views[Int(slot) & (CAP_VIEWS - 1)].active == 0:
        return
    queue_tuple(
        s,
        s[0].views[Int(slot) & (CAP_VIEWS - 1)].session,
        ev_type,
        x,
        y,
        key,
        codepoint,
    )


def emit_to_pressed(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin],
    ev_type: Int32,
    x: Int32,
    y: Int32,
    key: Int32,
    codepoint: Int32,
):
    # Session-scoped receiver fallback (upstream nitpicker keeps a
    # session-scoped View_owner receiver). The pressed slot is the
    # primary target; when a decorator view destroy+recreate has
    # cleared it, the delivery falls back to the session captured at
    # press time (pressed_session). Exactly one target per call: the
    # slot when it is valid and active, else the session fallback when
    # it is in range and its session is active, never both. The slot
    # clearing and the session preservation are scene.mojo's
    # clear_slot_refs; sc_session_close clears the fallback.
    var slot = s[0].pressed
    if slot >= 0 and slot < Int32(CAP_VIEWS):
        if s[0].views[Int(slot) & (CAP_VIEWS - 1)].active != 0:
            queue_tuple(
                s,
                s[0].views[Int(slot) & (CAP_VIEWS - 1)].session,
                ev_type,
                x,
                y,
                key,
                codepoint,
            )
            return
    var ps = s[0].pressed_session
    if ps >= 0 and ps < Int32(CAP_SESSIONS):
        if s[0].sessions[Int(ps) & (CAP_SESSIONS - 1)].active != 0:
            queue_tuple(s, ps, ev_type, x, y, key, codepoint)


def clamp_ptr(
    s: UnsafePointer[Scene_state, MutUntrackedOrigin], x: Int64, y: Int64
):
    # Int64 intermediates so hostile coordinates cannot overflow; the
    # result is clamped to the screen (screen_w/h >= 1 after init).
    var cx = x
    var cy = y
    if cx < 0:
        cx = 0
    if cx > Int64(s[0].screen_w) - 1:
        cx = Int64(s[0].screen_w) - 1
    if cy < 0:
        cy = 0
    if cy > Int64(s[0].screen_h) - 1:
        cy = Int64(s[0].screen_h) - 1
    s[0].ptr_x = Int32(cx)
    s[0].ptr_y = Int32(cy)


def motion_step(s: UnsafePointer[Scene_state, MutUntrackedOrigin]):
    if s[0].key_cnt > 0:
        # Drag: the stream stays with the press receiver and hover
        # updates are suppressed (upstream user_state.cc: motion while a
        # key is held leaves the originally hovered client when it is
        # not the receiver, and pins the stream to the receiver).
        s[0].drag = 1
        if s[0].hovered >= 0 and s[0].hovered != s[0].pressed:
            emit_to_slot(s, s[0].hovered, EV_HOVER_LEAVE, 0, 0, 0, 0)
            s[0].hovered = -1
        emit_to_pressed(s, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0)
        return
    # Hover update plus forwarded event (upstream update_hover + the
    # forward in _handle_input_event). On a hover change the new view
    # gets the position twice - once from the hover update, once from
    # the forwarded motion - exactly as upstream delivers them.
    var target = hit_test(s, s[0].ptr_x, s[0].ptr_y)
    if target != s[0].hovered:
        emit_to_slot(s, s[0].hovered, EV_HOVER_LEAVE, 0, 0, 0, 0)
        emit_to_slot(s, target, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0)
        s[0].hovered = target
    emit_to_slot(s, s[0].hovered, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0)


def press_step(s: UnsafePointer[Scene_state, MutUntrackedOrigin], key: Int32, codepoint: Int32):
    if s[0].key_bits[key_word(key)] & key_bit(key) != 0:
        # Suspicious double press: dropped entirely (upstream parity).
        return
    s[0].key_bits[key_word(key)] = s[0].key_bits[key_word(key)] | key_bit(key)
    s[0].key_cnt += 1
    if is_mouse_button(key):
        s[0].buttons |= Int32(1) << (key - Int32(BTN_LEFT))
    if s[0].key_cnt == 1:
        # Sequence start. Mouse buttons steer click focus: FOCUS_LEAVE
        # to the old focus before Absolute_motion + Focus_enter on the
        # hovered view (upstream user_state.cc). Afterwards the receiver
        # resets to the focused view (upstream
        # '_input_receiver = _focused').
        var switch_target = Int32(-1)
        if is_mouse_button(key):
            switch_target = s[0].hovered
        if switch_target >= 0 and switch_target != s[0].focused:
            emit_to_slot(s, s[0].focused, EV_FOCUS_LEAVE, 0, 0, 0, 0)
            emit_to_slot(
                s, switch_target, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0
            )
            emit_to_slot(s, switch_target, EV_FOCUS_ENTER, 0, 0, 0, 0)
            s[0].focused = switch_target
        s[0].pressed = s[0].focused
        # Session-scoped receiver fallback, captured atomically with
        # the receiver reset: the session of the active pressed view,
        # or -1 when there is no active pressed view.
        var ps = Int32(-1)
        if s[0].pressed >= 0 and s[0].pressed < Int32(CAP_VIEWS):
            if s[0].views[Int(s[0].pressed) & (CAP_VIEWS - 1)].active != 0:
                ps = s[0].views[Int(s[0].pressed) & (CAP_VIEWS - 1)].session
        s[0].pressed_session = ps
    # On a mouse-button press without prior drag motion the receiver
    # gets the current pointer position when it is not the hovered view
    # (e.g. a click outside every view goes to the focused view).
    if is_mouse_button(key):
        if s[0].drag == 0 and s[0].pressed >= 0 and s[0].pressed != s[0].hovered:
            emit_to_slot(
                s, s[0].pressed, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0
            )
    emit_to_pressed(s, EV_PRESS, 0, 0, key, codepoint)


def release_step(s: UnsafePointer[Scene_state, MutUntrackedOrigin], key: Int32):
    if s[0].key_bits[key_word(key)] & key_bit(key) == 0:
        # Suspicious double release: dropped entirely (upstream parity).
        return
    s[0].key_bits[key_word(key)] = s[0].key_bits[key_word(key)] & ~key_bit(key)
    s[0].key_cnt -= 1
    if is_mouse_button(key):
        s[0].buttons &= ~(Int32(1) << (key - Int32(BTN_LEFT)))
    emit_to_pressed(s, EV_RELEASE, 0, 0, key, 0)
    if s[0].key_cnt == 0:
        # Sequence end (upstream): the hover update runs now that no key
        # is held, then a drag receiver that is no longer hovered gets
        # its Hover_leave (upstream emits both the hover-update leave
        # and this one when the drag ended elsewhere).
        var target = hit_test(s, s[0].ptr_x, s[0].ptr_y)
        if target != s[0].hovered:
            emit_to_slot(s, s[0].hovered, EV_HOVER_LEAVE, 0, 0, 0, 0)
            emit_to_slot(s, target, EV_ABS_MOTION, s[0].ptr_x, s[0].ptr_y, 0, 0)
            s[0].hovered = target
        if s[0].drag != 0 and s[0].pressed >= 0 and s[0].pressed != s[0].hovered:
            emit_to_slot(s, s[0].pressed, EV_HOVER_LEAVE, 0, 0, 0, 0)
        s[0].drag = 0


# ----------------------------------------------------------------------
# Exported C ABI (contract: ../abi.h). Every entry point receives the
# caller-provided state block as its first argument and rejects a null
# block before touching it.
# ----------------------------------------------------------------------


@export("sc_input_step")
def sc_input_step(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    ev_type: Int32,
    a: Int32,
    b: Int32,
    c: Int32,
    d: Int32,
    out_events: Optional[UnsafePointer[Int32, MutUntrackedOrigin]],
    max_out: Int32,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if ev_type < 0 or ev_type > Int32(EV_AXIS):
        return -1
    if max_out < 0:
        return -1
    if not out_events:
        return -1
    var s = state_of(state)
    # Capacity contract: a tuple-producing event is rejected (NOT
    # consumed) while the queue cannot hold its worst-case tuples, so no
    # delivery is ever lost to a full queue. Drain-only and no-delivery
    # types are never rejected: SC_EV_INVALID always flushes.
    var producing = 0
    if ev_type == Int32(EV_PRESS):
        producing = 1
    elif ev_type == Int32(EV_RELEASE):
        producing = 1
    elif ev_type == Int32(EV_REL_MOTION):
        producing = 1
    elif ev_type == Int32(EV_ABS_MOTION):
        producing = 1
    elif ev_type == Int32(EV_WHEEL):
        producing = 1
    elif ev_type == Int32(EV_SEQ_NUMBER):
        producing = 1
    if producing != 0:
        if s[0].evq_n + Int32(MAX_STEP_TUPLES) > Int32(CAP_TUPLES):
            return -2
    if ev_type == Int32(EV_ABS_MOTION):
        clamp_ptr(s, Int64(a), Int64(b))
        motion_step(s)
    elif ev_type == Int32(EV_REL_MOTION):
        if s[0].exclusive != 0:
            # Exclusive input: relative motion stays relative and goes
            # to the focused view (upstream: receiver = _focused); the
            # pointer does not move.
            var recv = s[0].pressed
            if s[0].focused >= 0:
                recv = s[0].focused
            emit_to_slot(s, recv, EV_REL_MOTION, a, b, 0, 0)
        else:
            # Relative motion converts to clamped absolute motion,
            # materializing from (0, 0) before the first motion.
            clamp_ptr(s, Int64(s[0].ptr_x) + Int64(a), Int64(s[0].ptr_y) + Int64(b))
            motion_step(s)
    elif ev_type == Int32(EV_PRESS):
        if a < 0 or a >= Int32(KEY_MAX):
            return -1
        press_step(s, a, b)
    elif ev_type == Int32(EV_RELEASE):
        if a < 0 or a >= Int32(KEY_MAX):
            return -1
        release_step(s, a)
    elif ev_type == Int32(EV_WHEEL):
        # Wheel goes to the current input receiver (upstream routes it
        # to _input_receiver, not to the hovered view).
        emit_to_pressed(s, EV_WHEEL, a, b, 0, 0)
    elif ev_type == Int32(EV_SEQ_NUMBER):
        # Sequence numbers go to the hovered view, else the focused one
        # (upstream seq_number routing); the value travels in `x`.
        var recv = s[0].focused
        if s[0].hovered >= 0:
            recv = s[0].hovered
        emit_to_slot(s, recv, EV_SEQ_NUMBER, a, 0, 0, 0)
    # SC_EV_INVALID drains queued tuples only. FOCUS_ENTER, FOCUS_LEAVE
    # and HOVER_LEAVE are delivery-only types; TOUCH, TOUCH_RELEASE and
    # AXIS are not modeled in G2. All are accepted as input with no
    # state change and no delivery claimed.
    return queue_flush(s, out_events.value(), max_out)


@export("sc_focus_forward")
def sc_focus_forward(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    session: Int32,
    target: UInt64,
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if session < 0 or session >= Int32(CAP_SESSIONS):
        return -3
    var s = state_of(state)
    if s[0].sessions[Int(session) & (CAP_SESSIONS - 1)].active == 0:
        return -3
    var slot = find_view(s, session, target)
    if slot < 0:
        return -3
    if slot == s[0].focused:
        return 0
    if s[0].evq_n + 2 > Int32(CAP_TUPLES):
        return -2
    # External focus forwarding (upstream '_apply_pending_focus_change'
    # without its drag deferral): FOCUS_LEAVE to the old focus, then
    # FOCUS_ENTER to the new one. No motion tuple here (click focus,
    # not external forwarding, sends the pointer position). The queued
    # tuples are drained by the next sc_input_step call.
    emit_to_slot(s, s[0].focused, EV_FOCUS_LEAVE, 0, 0, 0, 0)
    s[0].focused = slot
    emit_to_slot(s, s[0].focused, EV_FOCUS_ENTER, 0, 0, 0, 0)
    return 0


@export("sc_focus_clear")
def sc_focus_clear(state: UnsafePointer[UInt8, MutUntrackedOrigin]) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    var s = state_of(state)
    if s[0].focused < 0:
        return 0
    if s[0].evq_n + 1 > Int32(CAP_TUPLES):
        return -2
    emit_to_slot(s, s[0].focused, EV_FOCUS_LEAVE, 0, 0, 0, 0)
    s[0].focused = -1
    return 0


@export("sc_pointer_pos")
def sc_pointer_pos(
    state: UnsafePointer[UInt8, MutUntrackedOrigin],
    out_x: Optional[UnsafePointer[Int32, MutUntrackedOrigin]],
    out_y: Optional[UnsafePointer[Int32, MutUntrackedOrigin]],
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    if not out_x or not out_y:
        return -1
    var s = state_of(state)
    out_x.value().store(s[0].ptr_x)
    out_y.value().store(s[0].ptr_y)
    return 0


@export("sc_input_exclusive")
def sc_input_exclusive(
    state: UnsafePointer[UInt8, MutUntrackedOrigin], enabled: Int32
) abi("C") -> Int32:
    if is_null_state(state):
        return -1
    var s = state_of(state)
    if enabled != 0:
        s[0].exclusive = 1
    else:
        s[0].exclusive = 0
    return 0
