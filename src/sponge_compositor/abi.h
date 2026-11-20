/* SPDX-License-Identifier: Apache-2.0
 *
 * abi.h — C ABI of the Mojo-owned Sponge compositor core (G1/G2).
 *
 * This header is the complete seam between the thin C++ Genode adapter
 * and the Mojo compositor core (`mojo/scene.mojo`, `mojo/painter.mojo`,
 * `mojo/input.mojo`). The Mojo side owns the scene/session/view stack,
 * the command reducer, clipping and damage, the opaque painter
 * (plan D16C.3, G1), and the input state, alpha/input-mask hit testing,
 * pointer/focus state, and input-event reduction (G2); the C++ side
 * allocates/maps pixel buffers, adapts Genode Gui/Capture/Event/Input
 * sessions, and calls the entry points declared here. There is no
 * second scene model in C++ (D16C.10).
 *
 * State storage contract. The scene state lives in a raw byte block
 * owned by the caller: the C++ adapter allocates exactly
 * SC_SCENE_STORAGE_BYTES bytes (for example as dataspace glue) and hands
 * the block to sc_scene_init together with its size. sc_scene_init
 * rejects a null or undersized block and initializes the Mojo-owned
 * scene state in place. Every other entry point takes the same block as
 * its first argument. C++ merely provides the bytes and MUST NOT model
 * or inspect the scene contents: every scene field, reducer decision,
 * stacking rule, clipping/damage computation, and painter operation is
 * Mojo-owned. The block must stay valid and exclusively used through
 * this ABI for as long as the scene is used, and sc_scene_init must be
 * called on it before any other entry point.
 *
 * All entry points are ordinary C functions (`extern "C"`,
 * `@export`/`abi("C")` on the Mojo side) taking fixed-width scalars and
 * plain pointers only (D16C.3 ABI seam: no aggregates cross the
 * boundary). A NULL pointer argument is always a "bad args" error,
 * never a crash: the Mojo side rejects null state blocks and models
 * nullable buffer parameters as `Optional[UnsafePointer[...]]`, which
 * has the exact machine representation of a plain C pointer
 * (NULL = empty).
 *
 * ------------------------------------------------------------------ */
#ifndef _SPONGE_COMPOSITOR_ABI_H_
#define _SPONGE_COMPOSITOR_ABI_H_

#include <base/stdint.h>

extern "C" {

/* ------------------------------------------------------------------ */
/* Common status codes                                                  */
/* ------------------------------------------------------------------ */

/*
 * Most entry points return Genode::int32_t status:
 *
 *    0  OK
 *   -1  bad arguments (NULL state block, NULL buffer where a buffer is
 *       required, undersized state block, negative width/height,
 *       non-positive buffer width/height, out-of-range color component,
 *       bad kind, duplicate view id, non-positive or non-4-byte pitch,
 *       ...)
 *   -2  capacity exhausted (all 8 sessions in use, all 32 views in use,
 *       or the caller's rect buffer cannot hold the pending damage)
 *   -3  unknown handle or id (stale session handle, stale view id,
 *       unknown stacking neighbor). Stale ids never corrupt state.
 *
 * A few entry points carry data in the return value instead:
 * sc_session_open returns the new session handle (>= 0) or a negative
 * status, sc_damage_fetch and sc_input_step return the number of
 * written rects/tuples, sc_damage_count returns the pending damage
 * count, and sc_pointer_pos reports the position through out-params.
 * All are documented below.
 */

/* Size in bytes of the caller-provided scene state block. */
enum { SC_SCENE_STORAGE_BYTES = 8192 };

/* Session kinds for sc_session_open. */
enum {
	SC_SESSION_GUI     = 0,
	SC_SESSION_CAPTURE = 1,
	SC_SESSION_EVENT   = 2
};

/*
 * Command opcodes for sc_apply_command, matching the public Gui command
 * surface of `os/include/gui_session/gui_session.h`:
 *
 *   Command::Opcode { GEOMETRY, OFFSET, FRONT, BACK, FRONT_OF,
 *                     BEHIND_OF, BACKGROUND, TITLE, NOP }
 */
enum {
	SC_GEOMETRY   = 0,
	SC_OFFSET     = 1,
	SC_FRONT      = 2,
	SC_BACK       = 3,
	SC_FRONT_OF   = 4,
	SC_BEHIND_OF  = 5,
	SC_BACKGROUND = 6,
	SC_TITLE      = 7,
	SC_NOP        = 8
};

/*
 * Fixed capacities of the Mojo-owned scene (compile-time constants on
 * the Mojo side; InlineArray storage, no heap):
 */
enum {
	SC_MAX_SESSIONS = 32,
	SC_MAX_VIEWS    = 64,
	SC_MAX_DAMAGE   = 3
};

/* ------------------------------------------------------------------ */
/* Pixel and buffer conventions                                         */
/* ------------------------------------------------------------------ */

/*
 * Pixels are 32-bit words in Genode `Pixel_rgb888` layout (public
 * header `os/include/os/pixel_rgb888.h`):
 *
 *     word = (r << 16) | (g << 8) | b          (0x00RRGGBB)
 *
 * Every `pitch` argument is the row pitch in BYTES and must be a
 * positive multiple of 4 (the pixel word size). Row stride in words is
 * `pitch / 4`.
 *
 * View buffer extent contract for sc_view_bind_pixels and
 * sc_view_bind_surfaces: the caller reports the buffer's own pixel
 * dimensions (`buf_w` x `buf_h`, positive) at bind time - the extent of
 * the pixel (and alpha/input-mask) buffer itself, NOT the view's
 * current geometry. The buffer must provide `pitch` bytes per row over
 * `buf_h` rows and at least `buf_w` pixels per such row. The compositor
 * never reads outside that extent: composition and input-mask sampling
 * tile the bound extent across the view like upstream nitpicker
 * (`os/src/server/nitpicker/view.cc` tiling a frozen-size texture), so
 * a view that grows before its next buffer arrives (e.g. maximize)
 * wraps its source coordinates back into the previous extent instead of
 * reading past it. Inside the extent every pixel is read exactly as
 * before. Rebind (sc_view_bind_pixels / sc_view_bind_surfaces) may be
 * called on every buffer hand-off, e.g. when alternating between two
 * buffers.
 *
 * View ids. `view_id` and `neighbor` are 64-bit (Genode::uint64_t),
 * matching the public Gui `View_id` (`Id_space<View_ref>::Id`,
 * `unsigned long` in `os/include/gui_session/gui_session.h`). Ids are
 * caller-chosen per session and are never narrowed to 32 bits.
 *
 * Alpha and input-mask planes (sc_view_bind_surfaces). The plane layout
 * matches the public `os/include/framebuffer_session/framebuffer_session.h`
 * Mode contract in split form: the alpha plane is one byte per pixel and
 * the input-mask plane one byte per pixel. An input-mask byte of zero
 * passes input through the view to the views below; any other byte
 * (nominally one) consumes the input at that pixel. Byte-plane pitches
 * are row pitches in BYTES (no alignment requirement beyond being
 * positive); the buffer-extent contract above applies per plane: each
 * bound plane must provide `pitch` bytes per row over the `buf_h` rows
 * and at least `buf_w` bytes per such row, and is sampled with the same
 * tiling wrap as the pixel plane.
 */

/* ------------------------------------------------------------------ */
/* Scene and session lifetime                                           */
/* ------------------------------------------------------------------ */

/*
 * Initialize (or re-initialize) the caller-provided state block in
 * place: `state` points at at least `state_size` raw bytes, and
 * `state_size` must be >= SC_SCENE_STORAGE_BYTES. A NULL block or an
 * undersized block is rejected without touching memory. The call resets
 * all sessions, views, stacking, and pending damage, and sets the
 * screen size and background color (components 0..255).
 *
 * Returns 0, or -1 for a null/undersized block, bad dimensions, or an
 * out-of-range color component.
 */
Genode::int32_t sc_scene_init(Genode::uint8_t *state,
                              Genode::uint64_t state_size,
                              Genode::int32_t screen_w,
                              Genode::int32_t screen_h,
                              Genode::int32_t bg_r,
                              Genode::int32_t bg_g,
                              Genode::int32_t bg_b);

/*
 * Open a session of the given kind (SC_SESSION_*).
 *
 * Returns the session handle (0..SC_MAX_SESSIONS-1) on success, or
 * -1 for a NULL state block or an unknown kind, or -2 when all session
 * slots are in use. The handle addresses the session in every
 * session-taking call below.
 */
Genode::int32_t sc_session_open(Genode::uint8_t *state,
                                Genode::int32_t kind);

/*
 * Close a session. All views of the session are destroyed (removed
 * from the stack and their pixel bindings dropped).
 *
 * Returns 0, or -1 for a NULL state block, or -3 for an
 * unknown/inactive handle.
 */
Genode::int32_t sc_session_close(Genode::uint8_t *state,
                                 Genode::int32_t handle);

/* ------------------------------------------------------------------ */
/* Views                                                                */
/* ------------------------------------------------------------------ */

/*
 * Create a view with initial screen geometry (x, y, w, h) and caller
 * chosen per-session 64-bit view id. `front != 0` places the view at
 * the top of the stacking order, `front == 0` at the bottom.
 *
 * Returns 0; -1 for a NULL state block, negative w/h, or a view id
 * that already exists in the session; -3 for an unknown session; -2
 * when all view slots are in use. Geometry may lie (partly) off-screen;
 * painting clips.
 */
Genode::int32_t sc_view_create(Genode::uint8_t *state,
                               Genode::int32_t session,
                               Genode::uint64_t view_id,
                               Genode::int32_t x,
                               Genode::int32_t y,
                               Genode::int32_t w,
                               Genode::int32_t h,
                               Genode::int32_t front);

/*
 * Destroy a view (stack entry removed, pixel binding dropped).
 *
 * Returns 0, or -1 for a NULL state block, or -3 for an unknown
 * session or stale view id.
 */
Genode::int32_t sc_view_destroy(Genode::uint8_t *state,
                                Genode::int32_t session,
                                Genode::uint64_t view_id);

/*
 * Bind (or re-bind) the pixel buffer of a view. May be called on every
 * buffer hand-off; the binding is replaced wholesale, including any
 * alpha/input-mask planes bound with sc_view_bind_surfaces (the view
 * hit-tests as opaque again). `pixels` points at the pixel word for
 * buffer coordinate (0, 0); `buf_w` and `buf_h` are the buffer's pixel
 * dimensions (see the buffer extent contract above) - they describe the
 * bound buffer, not the view's current geometry, and are what
 * composition wraps source coordinates over while a geometry change
 * awaits its new buffer. NULL `pixels`, a bad pitch, or non-positive
 * `buf_w`/`buf_h` is -1.
 *
 * Returns 0, -1 for bad arguments (including a NULL state block), or
 * -3 for unknown session/view.
 */
Genode::int32_t sc_view_bind_pixels(Genode::uint8_t *state,
                                    Genode::int32_t session,
                                    Genode::uint64_t view_id,
                                    Genode::uint32_t const *pixels,
                                    Genode::int32_t pitch,
                                    Genode::int32_t buf_w,
                                    Genode::int32_t buf_h);

/*
 * Bind (or re-bind) the full surface set of a view: the pixel plane
 * (required, as for sc_view_bind_pixels) plus optional alpha and
 * input-mask planes (each NULL or a valid plane with a positive byte
 * pitch; NULL clears that plane). The binding is replaced wholesale on
 * every call. `buf_w`/`buf_h` are the buffer's pixel dimensions, shared
 * by all three planes (the plane layout of one Framebuffer::Mode - see
 * the buffer extent contract above); like sc_view_bind_pixels they
 * describe the bound buffer, not the view's current geometry. The
 * planes drive hit testing (see "Hit testing and input delivery" below):
 * a view with no alpha plane is opaque and consumes every input point
 * inside its rect; an alpha-enabled view consults its input-mask plane
 * where zero falls through to the views below and nonzero consumes; an
 * alpha-enabled view without a mask plane consumes inside its rect. A
 * mask plane bound without an alpha plane has no effect.
 *
 * Returns 0; -1 for a NULL state block, NULL pixels, a non-positive or
 * non-4-byte pixel pitch, non-positive `buf_w`/`buf_h`, or a
 * non-positive pitch of a bound byte plane; -3 for an unknown session
 * or stale view id.
 */
Genode::int32_t sc_view_bind_surfaces(Genode::uint8_t *state,
                                      Genode::int32_t session,
                                      Genode::uint64_t view_id,
                                      Genode::uint32_t const *pixels,
                                      Genode::int32_t pixel_pitch,
                                      Genode::int32_t buf_w,
                                      Genode::int32_t buf_h,
                                      Genode::uint8_t const *alpha,
                                      Genode::int32_t alpha_pitch,
                                      Genode::uint8_t const *input_mask,
                                      Genode::int32_t input_mask_pitch);

/*
 * Apply one Gui command to a view (command reducer). The arguments are
 * interpreted per opcode:
 *
 *   SC_GEOMETRY   sets the view's screen rect to (x, y, w, h)
 *                 (w, h >= 0; -1 otherwise)
 *   SC_OFFSET     sets the buffer offset to (x, y), i.e. the pixel
 *                 (x, y) of the bound buffer is shown at the view's
 *                 top-left corner (x, y >= 0; -1 otherwise)
 *   SC_FRONT      moves the view to the top of the stack
 *   SC_BACK       moves the view to the bottom of the stack
 *   SC_FRONT_OF   places the view directly above `neighbor`
 *   SC_BEHIND_OF  places the view directly below `neighbor`
 *   SC_BACKGROUND, SC_TITLE, SC_NOP and any other opcode are
 *                 state-safe no-ops (return 0)
 *
 * `neighbor` is a 64-bit view id in the same session; unknown
 * neighbors return -3 without changing anything (as does a stale
 * `view_id`). Stacking relative to the view itself is a no-op
 * (returns 0).
 *
 * Returns 0, -1 for bad geometry/offset arguments (including a NULL
 * state block), or -3 for an unknown session, stale view id, or
 * unknown neighbor.
 */
Genode::int32_t sc_apply_command(Genode::uint8_t *state,
                                 Genode::int32_t session,
                                 Genode::int32_t opcode,
                                 Genode::uint64_t view_id,
                                 Genode::int32_t x,
                                 Genode::int32_t y,
                                 Genode::int32_t w,
                                 Genode::int32_t h,
                                 Genode::uint64_t neighbor);

/* ------------------------------------------------------------------ */
/* Hit testing and input delivery (G2)                                  */
/* ------------------------------------------------------------------ */

/*
 * Event-type constants, matching Input::Event::Type of the public
 * `os/include/input/event.h` (declaration order is binding):
 *
 *   INVALID, PRESS, RELEASE, REL_MOTION, ABS_MOTION, WHEEL,
 *   FOCUS_ENTER, FOCUS_LEAVE, HOVER_LEAVE, TOUCH, TOUCH_RELEASE,
 *   SEQ_NUMBER, AXIS
 */
enum {
	SC_EV_INVALID       = 0,
	SC_EV_PRESS         = 1,
	SC_EV_RELEASE       = 2,
	SC_EV_REL_MOTION    = 3,
	SC_EV_ABS_MOTION    = 4,
	SC_EV_WHEEL         = 5,
	SC_EV_FOCUS_ENTER   = 6,
	SC_EV_FOCUS_LEAVE   = 7,
	SC_EV_HOVER_LEAVE   = 8,
	SC_EV_TOUCH         = 9,
	SC_EV_TOUCH_RELEASE = 10,
	SC_EV_SEQ_NUMBER    = 11,
	SC_EV_AXIS          = 12
};

/*
 * Keycode constants of the public `os/include/input/keycodes.h` used by
 * this ABI. Flattened event keys must lie in [0, SC_KEY_MAX).
 * SC_CODEPOINT_INVALID is `Codepoint::INVALID` of `os/include/util/utf8.h`.
 */
enum {
	SC_KEY_MAX           = 0x1ff,
	SC_BTN_LEFT          = 0x110,
	SC_BTN_RIGHT         = 0x111,
	SC_BTN_MIDDLE        = 0x112,
	SC_BTN_TOUCH         = 0x14a,
	SC_CODEPOINT_INVALID = 0xfffe
};

/*
 * Delivery tuples. sc_input_step reports delivered events as fixed-width
 * scalar tuples (no aggregates cross the ABI): each tuple is exactly
 * SC_EVENT_TUPLE_INTS consecutive Genode::int32_t words,
 *
 *     [0] session    target session handle
 *     [1] type       SC_EV_* value (the delivered event type)
 *     [2] x          motion/wheel x (screen coordinates for
 *                    SC_EV_ABS_MOTION, deltas for SC_EV_REL_MOTION and
 *                    SC_EV_WHEEL; the value for SC_EV_SEQ_NUMBER)
 *     [3] y          motion/wheel y
 *     [4] key        keycode for SC_EV_PRESS/SC_EV_RELEASE, else 0
 *     [5] codepoint  character code for SC_EV_PRESS, else 0
 *     [6] seq        delivery sequence number: 1-based, strictly
 *                    increasing in emission order per state block
 *     [7] reserved   0
 *
 * out_events buffers `max_out` tuples, i.e. max_out * SC_EVENT_TUPLE_INTS
 * words. Tuples are emitted oldest-first.
 */
enum { SC_EVENT_TUPLE_INTS = 8 };

/*
 * Hit testing runs top-down over the view stack (topmost view first) and
 * stops at the first consuming view: a point outside every view rect
 * delivers to no view; a view without an alpha plane consumes every
 * point inside its rect; an alpha-enabled view consults its input-mask
 * plane at buffer coordinate (floor-mod of (px - x + off_x) by buf_w,
 * floor-mod of (py - y + off_y) by buf_h - the same tiling wrap as
 * composition, so hit testing never reads outside the bound extent) -
 * mask byte 0 falls through to the views below, nonzero consumes; an
 * alpha-enabled view without a mask plane consumes inside its rect. A
 * zero-extent mask (buf_w or buf_h is 0) is not sampled and consumes
 * like a mask-free alpha view.
 */

/*
 * Consume one flattened Input::Event and report the resulting deliveries.
 *
 * ev_type is an SC_EV_* value; the attributes are flattened scalars:
 *
 *   SC_EV_PRESS         a = keycode, b = codepoint
 *                       (SC_CODEPOINT_INVALID for no character)
 *   SC_EV_RELEASE       a = keycode
 *   SC_EV_REL_MOTION    a = dx, b = dy
 *   SC_EV_ABS_MOTION    a = x, b = y (screen coordinates)
 *   SC_EV_WHEEL         a = x, b = y
 *   SC_EV_SEQ_NUMBER    a = sequence value
 *   SC_EV_INVALID       drain-only: consumes nothing, only flushes
 *                       queued tuples
 *   SC_EV_FOCUS_ENTER, SC_EV_FOCUS_LEAVE, SC_EV_HOVER_LEAVE are
 *   delivery-only types; SC_EV_TOUCH, SC_EV_TOUCH_RELEASE and SC_EV_AXIS
 *   are not modeled in G2. All are accepted with no state change and no
 *   delivery claimed. c and d are reserved attribute slots (0).
 *
 * Behavior (matches upstream nitpicker's public-server semantics where
 * G2 covers it):
 *   - SC_EV_ABS_MOTION clamps the pointer to the screen and delivers the
 *     clamped screen position. SC_EV_REL_MOTION converts to absolute
 *     motion the same way (from (0, 0) before the first motion) unless
 *     exclusive input is set, in which case it stays relative and is
 *     delivered to the focused view without moving the pointer.
 *   - Motion with no button held updates the hover: on change the old
 *     view receives SC_EV_HOVER_LEAVE and the new view the position
 *     twice (hover update plus forwarded motion, as upstream delivers).
 *   - The first held button starts a key sequence: a mouse button on a
 *     not-yet-focused view emits SC_EV_FOCUS_LEAVE to the old focus
 *     BEFORE SC_EV_ABS_MOTION + SC_EV_FOCUS_ENTER to the clicked view
 *     (then the press). While any button is held, motion stays with the
 *     press receiver ("drag to pressed"), hover updates are suppressed,
 *     and the receiver is told SC_EV_HOVER_LEAVE when the hover was
 *     pinned to a different view. The release ends the sequence with a
 *     hover update and, after a drag, an SC_EV_HOVER_LEAVE to a
 *     receiver that is no longer hovered (upstream parity).
 *   - SC_EV_WHEEL goes to the press receiver (the focused view outside
 *     sequences); SC_EV_SEQ_NUMBER goes to the hovered view, else the
 *     focused one.
 *   - Suspicious double presses and releases (same key twice without an
 *     opposite event in between) are dropped with no delivery.
 *
 * The event is consumed and its state effects applied on every return
 * except -1 and -2. Delivery tuples are queued FIFO and written to
 * out_events oldest-first; tuples that do not fit stay queued and are
 * written by later calls (including SC_EV_INVALID drains), so no
 * delivery is ever lost.
 *
 * Returns the number of tuples written (0..max_out), or:
 *   -1  bad arguments (NULL state block, ev_type outside 0..12, a
 *       keycode outside [0, SC_KEY_MAX), negative max_out, or NULL
 *       out_events),
 *   -2  a tuple-producing event found no room in the state's delivery
 *       queue for its worst-case tuples; the event is NOT consumed.
 *       Drain queued tuples first (SC_EV_INVALID with a sufficient
 *       max_out; drains are never rejected), then retry.
 */
Genode::int32_t sc_input_step(Genode::uint8_t *state,
                              Genode::int32_t ev_type,
                              Genode::int32_t a,
                              Genode::int32_t b,
                              Genode::int32_t c,
                              Genode::int32_t d,
                              Genode::int32_t *out_events,
                              Genode::int32_t max_out);

/*
 * Forward the input focus to the view `target` (64-bit view id) of
 * `session` - the external focus-policy seam (upstream nitpicker
 * 'User_state::focus'). On a change the old focus receives
 * SC_EV_FOCUS_LEAVE and the new one SC_EV_FOCUS_ENTER (no motion tuple;
 * click focus sends the position, external forwarding does not). The
 * tuples are queued like sc_input_step deliveries and drained by the
 * next sc_input_step call. Forwarding to the already-focused view is a
 * no-op. The press receiver of an in-flight key sequence is left alone,
 * so press/release pairs stay on one session (upstream applies focus
 * changes between drag operations; this call applies immediately and
 * the caller should serialize focus changes outside button sequences).
 *
 * Returns 0, or -1 for a NULL state block, or -3 for an unknown session
 * or stale view id, or -2 when the delivery queue has no room for the
 * focus tuples (nothing is changed; drain first).
 */
Genode::int32_t sc_focus_forward(Genode::uint8_t *state,
                                 Genode::int32_t session,
                                 Genode::uint64_t target);

/* Clear the focused view and deliver SC_EV_FOCUS_LEAVE to its previous owner. */
Genode::int32_t sc_focus_clear(Genode::uint8_t *state);

/*
 * Report the current pointer position. The position is clamped to the
 * screen and starts at (0, 0) after sc_scene_init; relative motion
 * materializes from there before the first absolute position.
 *
 * Returns 0, or -1 for a NULL state block or NULL out_x/out_y.
 */
Genode::int32_t sc_pointer_pos(Genode::uint8_t *state,
                               Genode::int32_t *out_x,
                               Genode::int32_t *out_y);

/*
 * Enable (enabled != 0) or disable exclusive input: while enabled,
 * relative-motion events are delivered as SC_EV_REL_MOTION to the
 * focused view without moving the pointer (upstream "exclusive input",
 * Input::Session::exclusive).
 *
 * Returns 0, or -1 for a NULL state block.
 */
Genode::int32_t sc_input_exclusive(Genode::uint8_t *state,
                                   Genode::int32_t enabled);

/* ------------------------------------------------------------------ */
/* Damage and composition                                               */
/* ------------------------------------------------------------------ */

/*
 * Add a damaged screen rect. Damage is clipped to the screen, absorbed
 * when contained in pending damage, merged into overlapping/touching
 * damage, and otherwise kept as one of at most SC_MAX_DAMAGE bounding
 * rects (when full, the new rect merges into the pending rect whose
 * bounding box grows the least). A pending damage list is what
 * sc_compose paints.
 *
 * Returns 0 (an empty rect - w == 0 or h == 0, or a rect fully outside
 * the screen - is a harmless no-op), or -1 for a NULL state block or
 * negative w/h.
 */
Genode::int32_t sc_damage_add(Genode::uint8_t *state,
                              Genode::int32_t x,
                              Genode::int32_t y,
                              Genode::int32_t w,
                              Genode::int32_t h);

/*
 * Report how many damage rects are pending (0..SC_MAX_DAMAGE) without
 * consuming them - the wakeup-signaling seam (an adapter can compare
 * the count and only wake Capture consumers when it changed). Unlike
 * sc_damage_fetch this never writes anything and never clears damage.
 *
 * Returns the pending count, or -1 for a NULL state block.
 */
Genode::int32_t sc_damage_count(Genode::uint8_t *state);

/*
 * Compose the current scene into the caller-provided screen buffer
 * (`pitch` in bytes, see the pixel conventions). Only the pending
 * damage rects are repainted (clipped to view geometry and screen):
 * each damage rect is filled with the background color first, then
 * views are painted bottom-up (stack order) as tiled copies of their
 * bound buffers (source coordinates floor-mod the bind-time buf_w/buf_h
 * extent - the buffer extent contract above), so later (higher) views
 * overwrite earlier ones where they overlap. Views without a bound
 * buffer are skipped (the background shows through), as are views
 * whose bound extent is zero (nothing to sample). With no pending
 * damage nothing is painted - add damage first (typically the full
 * screen at startup).
 * Damage is NOT cleared here; sc_damage_fetch clears it.
 *
 * Returns 0 (also when nothing is painted), or -1 for a NULL state
 * block, a NULL buffer, or a bad pitch.
 */
Genode::int32_t sc_compose(Genode::uint8_t *state,
                           Genode::uint32_t *screen_pixels,
                           Genode::int32_t pitch);

/*
 * Fetch and clear the pending damage. Up to SC_MAX_DAMAGE rects are
 * written to `rects_out` as consecutive (x, y, w, h) Int32 tuples,
 * i.e. rect i occupies rects_out[4*i .. 4*i + 3]; each rect is
 * non-empty and lies inside the screen. On success the pending damage
 * is cleared. With no pending damage nothing is written and the call
 * succeeds even for a NULL buffer.
 *
 * Returns the number of rects written (0..SC_MAX_DAMAGE), or:
 *   -1  bad arguments (NULL state block, negative max_rects, or NULL
 *       rects_out while damage would need to be written),
 *   -2  max_rects is smaller than the number of pending rects; in
 *       this case NOTHING is written and the damage is NOT cleared,
 *       so the caller can retry with a larger buffer.
 */
Genode::int32_t sc_damage_fetch(Genode::uint8_t *state,
                                Genode::int32_t *rects_out,
                                Genode::int32_t max_rects);

} /* extern "C" */

#endif /* _SPONGE_COMPOSITOR_ABI_H_ */
