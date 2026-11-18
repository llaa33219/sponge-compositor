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
    dst_x: Int32,
    dst_y: Int32,
    src_x: Int32,
    src_y: Int32,
    w: Int32,
    h: Int32,
):
    """Copy a pre-clipped `w` x `h` block of opaque pixels.

    Source pixels are read from (`src_x`, `src_y`) in the `src` buffer and
    written to (`dst_x`, `dst_y`) in the `dst` buffer. Strides are row
    pitches in 32-bit words. The caller guarantees both rects lie inside
    their buffers (scene.mojo clips to view geometry plus the buffer-offset
    contract recorded in abi.h before calling).
    """
    var dst_offset = Int64(dst_y) * Int64(dst_stride) + Int64(dst_x)
    var src_offset = Int64(src_y) * Int64(src_stride) + Int64(src_x)
    var dst_step = Int64(dst_stride)
    var src_step = Int64(src_stride)
    var wy = Int32(0)
    while wy < h:
        var drow = dst + Int(dst_offset)
        var srow = src + Int(src_offset)
        var wx = Int32(0)
        while wx < w:
            (drow + Int(wx)).store((srow + Int(wx)).load())
            wx += Int32(1)
        dst_offset += dst_step
        src_offset += src_step
        wy += Int32(1)
