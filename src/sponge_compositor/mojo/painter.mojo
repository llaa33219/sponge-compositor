# SPDX-License-Identifier: Apache-2.0
#
# painter.mojo — opaque pixel painter primitives for the Sponge compositor.
#
# Part of the Mojo-owned G1 compositor core (plan D16C.3). This module is a
# pure, stateless, allocation-free pixel writer: it owns no scene state and
# performs no clipping of its own. Every rect passed in must be pre-clipped
# by the caller (scene.mojo clips each draw to view ∩ screen ∩ damage before
# calling in here).
#
# Pixel format: one 32-bit word per pixel in Genode `Pixel_rgb888` layout
# (public header `os/include/os/pixel_rgb888.h`):
#
#     word = (r << 16) | (g << 8) | b          # 0x00RRGGBB
#
# Strides are expressed in 32-bit WORDS (row pitch in bytes / 4). All
# arithmetic that forms buffer offsets runs in Int64 before conversion so
# hostile geometry cannot overflow the index computation.
#
# In-OS subset (plan §6): no heap, no List, no Python interop, no async, no
# POSIX I/O, no Genode-private headers. The only pointer spellings used are
# the pinned-toolchain set (`UnsafePointer.load/store`, `p[i]`, `p + i`).
#
# Build seam (plan D16C.4):
#   mojo build --emit object --relocation-model=pic -o painter.o painter.mojo
# The emitted object has an empty `nm -u` (no KGEN, no libc). The component
# links the scene object; this module's code is compiled into it through
# `from painter import ...` (a standalone painter.o is empty because these
# helpers carry no exported entry points).


def fill_rect(
    dst: UnsafePointer[UInt32, MutUntrackedOrigin],
    dst_stride: Int32,
    x: Int32,
    y: Int32,
    w: Int32,
    h: Int32,
    word: UInt32,
):
    """Fill a pre-clipped `w` x `h` rect at (`x`, `y`) with `word`.

    `dst_stride` is the row pitch in 32-bit words. The caller guarantees
    `0 <= x`, `0 <= y`, `w >= 0`, `h >= 0` and that the whole rect lies
    inside the destination buffer.
    """
    var row_offset = Int64(y) * Int64(dst_stride) + Int64(x)
    var row_step = Int64(dst_stride)
    var wy = Int32(0)
    while wy < h:
        var row = dst + Int(row_offset)
        var wx = Int32(0)
        while wx < w:
            (row + Int(wx)).store(word)
            wx += Int32(1)
        row_offset += row_step
        wy += Int32(1)


def blit_opaque(
    dst: UnsafePointer[UInt32, MutUntrackedOrigin],
    dst_stride: Int32,
    src: UnsafePointer[UInt32, MutUntrackedOrigin],
    src_stride: Int32,
    src_w: Int32,
    src_h: Int32,
    dst_x: Int32,
    dst_y: Int32,
    src_x: Int32,
    src_y: Int32,
    w: Int32,
    h: Int32,
):
    """Tile-copy a pre-clipped `w` x `h` block of opaque pixels.

    Source pixels are written to (`dst_x`, `dst_y`) in the `dst` buffer
    and read from the `src_w` x `src_h` texture extent of `src`, tiled
    across the block: the source coordinate of each destination pixel is
    the floor-mod of the phase (`src_x`, `src_y`) plus the in-block
    offset by (`src_w`, `src_h`) - the upstream nitpicker texture
    tiling (`os/src/server/nitpicker/view.cc`, frozen-size texture).
    Inside the extent the floor-mod is the identity, so in-extent reads
    are exactly the straight copy. A zero (or negative) extent samples
    nothing. Strides are row pitches in 32-bit words; the caller
    guarantees the destination rect lies inside the destination buffer.
    """
    if src_w <= 0 or src_h <= 0:
        return
    var tw = Int64(src_w)
    var th = Int64(src_h)
    var sx0 = Int64(src_x)
    var sy0 = Int64(src_y)
    var dst_offset = Int64(dst_y) * Int64(dst_stride) + Int64(dst_x)
    var dst_step = Int64(dst_stride)
    # In-extent fast path: the whole block lies inside the texture
    # extent, so the floor-mod is the identity and the blit is the
    # straight copy. The boundary check runs in Int64 so hostile
    # coordinates cannot overflow it.
    if sx0 >= 0 and sy0 >= 0 and sx0 + Int64(w) <= tw and sy0 + Int64(h) <= th:
        var wy = Int32(0)
        while wy < h:
            var drow = dst + Int(dst_offset)
            var srow = src + Int((sy0 + Int64(wy)) * Int64(src_stride))
            var wx = Int32(0)
            while wx < w:
                (drow + Int(wx)).store((srow + Int(sx0 + Int64(wx))).load())
                wx += Int32(1)
            dst_offset += dst_step
            wy += Int32(1)
        return
    var wy = Int32(0)
    while wy < h:
        var drow = dst + Int(dst_offset)
        var sy = ((Int64(src_y) + Int64(wy)) % th + th) % th
        var srow = src + Int(sy * Int64(src_stride))
        var wx = Int32(0)
        while wx < w:
            var sx = ((Int64(src_x) + Int64(wx)) % tw + tw) % tw
            (drow + Int(wx)).store((srow + Int(sx)).load())
            wx += Int32(1)
        dst_offset += dst_step
        wy += Int32(1)


def blit_alpha(
    dst: UnsafePointer[UInt32, MutUntrackedOrigin],
    dst_stride: Int32,
    src: UnsafePointer[UInt32, MutUntrackedOrigin],
    src_stride: Int32,
    src_w: Int32,
    src_h: Int32,
    alpha: UnsafePointer[UInt8, MutUntrackedOrigin],
    alpha_stride: Int32,
    dst_x: Int32,
    dst_y: Int32,
    src_x: Int32,
    src_y: Int32,
    w: Int32,
    h: Int32,
):
    """Blend a pre-clipped block with a straight-alpha source plane.

    Each output channel is `(src * a + dst * (255 - a) + 127) / 255`.
    Zero-alpha pixels leave the destination unchanged; full-alpha pixels
    are copied exactly. The alpha stride is in bytes, pixel strides in
    32-bit words; both source planes share coordinates and are tiled
    over the `src_w` x `src_h` extent exactly as blit_opaque (the
    floor-mod of the phase plus the in-block offset). A zero (or
    negative) extent samples nothing.
    """
    if src_w <= 0 or src_h <= 0:
        return
    var tw = Int64(src_w)
    var th = Int64(src_h)
    var sx0 = Int64(src_x)
    var sy0 = Int64(src_y)
    var dst_offset = Int64(dst_y) * Int64(dst_stride) + Int64(dst_x)
    var dst_step = Int64(dst_stride)
    # In-extent fast path: the whole block lies inside the texture
    # extent, so the floor-mod is the identity and both source planes
    # are sampled with straight coordinates. The boundary check runs in
    # Int64 so hostile coordinates cannot overflow it. The blend math
    # is exactly the tiled path's.
    if sx0 >= 0 and sy0 >= 0 and sx0 + Int64(w) <= tw and sy0 + Int64(h) <= th:
        var wy = Int32(0)
        while wy < h:
            var drow = dst + Int(dst_offset)
            var srow = src + Int((sy0 + Int64(wy)) * Int64(src_stride))
            var arow = alpha + Int((sy0 + Int64(wy)) * Int64(alpha_stride))
            var wx = Int32(0)
            while wx < w:
                var sx = sx0 + Int64(wx)
                var a = UInt32((arow + Int(sx)).load())
                if a != 0:
                    var s = (srow + Int(sx)).load()
                    if a == 255:
                        (drow + Int(wx)).store(s)
                    else:
                        var d = (drow + Int(wx)).load()
                        var inv = UInt32(255) - a
                        var sr = ((s >> 16) & 0xff) * a
                        var sg = ((s >> 8) & 0xff) * a
                        var sb = (s & 0xff) * a
                        var dr = ((d >> 16) & 0xff) * inv
                        var dg = ((d >> 8) & 0xff) * inv
                        var db = (d & 0xff) * inv
                        var r = (sr + dr + 127) / 255
                        var g = (sg + dg + 127) / 255
                        var b = (sb + db + 127) / 255
                        (drow + Int(wx)).store((r << 16) | (g << 8) | b)
                wx += Int32(1)
            dst_offset += dst_step
            wy += Int32(1)
        return
    var wy = Int32(0)
    while wy < h:
        var drow = dst + Int(dst_offset)
        var sy = ((Int64(src_y) + Int64(wy)) % th + th) % th
        var srow = src + Int(sy * Int64(src_stride))
        var arow = alpha + Int(sy * Int64(alpha_stride))
        var wx = Int32(0)
        while wx < w:
            var sx = ((Int64(src_x) + Int64(wx)) % tw + tw) % tw
            var a = UInt32((arow + Int(sx)).load())
            if a != 0:
                var s = (srow + Int(sx)).load()
                if a == 255:
                    (drow + Int(wx)).store(s)
                else:
                    var d = (drow + Int(wx)).load()
                    var inv = UInt32(255) - a
                    var sr = ((s >> 16) & 0xff) * a
                    var sg = ((s >> 8) & 0xff) * a
                    var sb = (s & 0xff) * a
                    var dr = ((d >> 16) & 0xff) * inv
                    var dg = ((d >> 8) & 0xff) * inv
                    var db = (d & 0xff) * inv
                    var r = (sr + dr + 127) / 255
                    var g = (sg + dg + 127) / 255
                    var b = (sb + db + 127) / 255
                    (drow + Int(wx)).store((r << 16) | (g << 8) | b)
            wx += Int32(1)
        dst_offset += dst_step
        wy += Int32(1)
