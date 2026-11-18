/* SPDX-License-Identifier: Apache-2.0
 *
 * abi.h — C ABI of the Mojo-owned Sponge compositor core (G1).
 *
 * This header is the complete seam between the thin C++ Genode adapter
 * and the Mojo compositor core (`mojo/scene.mojo`, `mojo/painter.mojo`).
 * The Mojo side owns the scene/session/view stack, the command reducer,
 * clipping and damage, and the opaque painter (plan D16C.3); the C++
 * side allocates/maps pixel buffers, adapts Genode Gui/Capture/Event
 * sessions, and calls the entry points declared here. There is no second
 * scene model in C++ (D16C.10).
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
 *       out-of-range color component, bad kind, duplicate view id,
 *       non-positive or non-4-byte pitch, ...)
 *   -2  capacity exhausted (all 8 sessions in use, all 32 views in use,
 *       or the caller's rect buffer cannot hold the pending damage)
 *   -3  unknown handle or id (stale session handle, stale view id,
 *       unknown stacking neighbor). Stale ids never corrupt state.
 *
 * Two entry points carry data in the return value instead:
 * sc_session_open returns the new session handle (>= 0) or a negative
 * status, and sc_damage_fetch returns the number of rects written
 * (0..3) or a negative status. Both are documented below.
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
	SC_MAX_SESSIONS = 8,
	SC_MAX_VIEWS    = 32,
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
 * View buffer extent contract for sc_view_bind_pixels: the buffer
 * behind `pixels` must provide, for the view's current geometry and
 * buffer offset, at least `pitch` bytes per row over the rows
 * `off_y .. off_y + h - 1` and at least `off_x + w` pixels per such
 * row. The compositor never reads outside that extent. Rebind
 * (sc_view_bind_pixels) may be called on every buffer hand-off, e.g.
 * when alternating between two buffers.
 *
 * View ids. `view_id` and `neighbor` are 64-bit (Genode::uint64_t),
 * matching the public Gui `View_id` (`Id_space<View_ref>::Id`,
 * `unsigned long` in `os/include/gui_session/gui_session.h`). Ids are
 * caller-chosen per session and are never narrowed to 32 bits.
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
 * buffer hand-off; the binding is replaced wholesale. `pixels` points
 * at the pixel word for buffer coordinate (0, 0); see the buffer
 * extent contract above. NULL `pixels` or a bad pitch is -1.
 *
 * Returns 0, -1 for bad arguments (including a NULL state block), or
 * -3 for unknown session/view.
 */
Genode::int32_t sc_view_bind_pixels(Genode::uint8_t *state,
                                    Genode::int32_t session,
                                    Genode::uint64_t view_id,
                                    Genode::uint32_t const *pixels,
                                    Genode::int32_t pitch);

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
 * Compose the current scene into the caller-provided screen buffer
 * (`pitch` in bytes, see the pixel conventions). Only the pending
 * damage rects are repainted (clipped to view geometry and screen):
 * each damage rect is filled with the background color first, then
 * views are painted bottom-up (stack order) as opaque copies of their
 * bound buffers, so later (higher) views overwrite earlier ones where
 * they overlap. Views without a bound buffer are skipped (the
 * background shows through). With no pending damage nothing is
 * painted - add damage first (typically the full screen at startup).
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
