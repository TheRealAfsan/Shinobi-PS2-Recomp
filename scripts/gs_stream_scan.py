"""What screen effects does the game draw? Reads a recorded PCSX2 drawing stream (PS2X_PCSX2_STREAM, captures/gs_stream.bin)
and reports, per frame and in total:

  fog        primitives drawn with fog on (PRIM/PRMODE FGE), the fog colours (FOGCOL) and how strong (FOG 255 = none)
  feedback   draws whose texture is a buffer drawn earlier in the same frame or the frame shown: how PS2 games make
             blur, glow, haze, depth of field and motion blur (render to texture, then draw it back blended)
  fullscreen sprites covering (nearly) the whole screen: colour washes, flashes, fades, the second half of such effects
  blended    primitives with alpha blending (trails, particles, smoke, glow)

usage: python -I gs_stream_scan.py <gs_stream.bin> [--every N] [--from FRAME] [--to FRAME]
Record format (gs_pcsx2.cpp recordStream): u32 type, u32 bytes, data. Types 1..3 = GIF packet (PATH1..3),
10 = vsync (19 privileged registers + field index), 11 = GS reset.
"""
import argparse
import collections
import struct
import sys

# GS register numbers (A+D / REGLIST)
PRIM, RGBAQ, ST, UV, XYZF2, XYZ2, TEX0_1, TEX0_2, CLAMP_1, CLAMP_2, FOG, XYZF3, XYZ3 = 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, \
    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0C, 0x0D
PRMODECONT, PRMODE, FOGCOL, ALPHA_1, ALPHA_2, FRAME_1, FRAME_2, XYOFFSET_1, XYOFFSET_2 = 0x1A, 0x1B, 0x3D, 0x42, 0x43, 0x4C, \
    0x4D, 0x18, 0x19
PRIM_NAMES = ['point', 'line', 'linestrip', 'tri', 'tristrip', 'trifan', 'sprite', 'prohibited']
VERTS_PER_PRIM = [1, 2, 2, 3, 3, 3, 2, 1]


class Gs:
    def __init__(self):
        self.prim = 0
        self.prmode = 0
        self.prmodecont = 1
        self.frame = [0, 0]
        self.tex0 = [0, 0]
        self.offset = [0, 0]
        self.fogcol = 0
        self.fog = 255
        self.verts = []  # (x, y) of the current primitive's vertices
        self.stats = None

    def attrs(self):
        return self.prim if self.prmodecont & 1 else self.prmode

    def ctx(self):
        return (self.attrs() >> 9) & 1

    def write(self, reg, value, packed_fog=None):
        if reg == PRIM:
            self.prim = value & 0x7FF
            self.verts = []
            self.stats.prims_set += 1
        elif reg == PRMODE:
            self.prmode = value & 0x7FF
        elif reg == PRMODECONT:
            self.prmodecont = value & 1
        elif reg in (FRAME_1, FRAME_2):
            self.frame[reg - FRAME_1] = value
        elif reg in (TEX0_1, TEX0_2):
            self.tex0[reg - TEX0_1] = value
        elif reg in (XYOFFSET_1, XYOFFSET_2):
            self.offset[reg - XYOFFSET_1] = value
        elif reg == FOGCOL:
            self.fogcol = value & 0xFFFFFF
        elif reg == FOG:
            self.fog = (value >> 56) & 0xFF
        elif reg in (XYZF2, XYZ2, XYZF3, XYZ3):
            if reg in (XYZF2, XYZF3):
                self.fog = (value >> 56) & 0xFF
            x, y = value & 0xFFFF, (value >> 16) & 0xFFFF
            self.verts.append((x, y))
            kind = self.attrs() & 7
            need = VERTS_PER_PRIM[kind]
            if len(self.verts) >= need:
                if reg in (XYZF2, XYZ2):
                    self.kick(kind)
                if kind in (2, 4, 5):  # strips and fans keep the previous vertices
                    self.verts = self.verts[1:] if kind != 5 else [self.verts[0]] + self.verts[2:]
                else:
                    self.verts = []

    def kick(self, kind):
        s = self.stats
        a = self.attrs()
        c = self.ctx()
        fbp = (self.frame[c] & 0x1FF) * 32          # frame buffer, in 256-byte blocks
        fbw = ((self.frame[c] >> 16) & 0x3F) * 64
        s.prims += 1
        s.by_kind[PRIM_NAMES[kind]] += 1
        s.targets[fbp] += 1
        if a & 0x20:  # FGE
            s.fog_prims += 1
            s.fog_colours[self.fogcol] += 1
            s.fog_values[min(self.fog // 32, 7)] += 1
        if a & 0x40:  # ABE
            s.blended += 1
        if a & 0x10:  # TME
            tbp = self.tex0[c] & 0x3FFF
            if tbp in s.drawn_blocks or tbp in s.prev_blocks:  # this frame's buffers, or last frame's (motion blur)
                s.feedback += 1
                s.feedback_pairs[(tbp, fbp)] += 1
        s.drawn_blocks.add(fbp)
        if kind == 6 and len(self.verts) >= 2:
            ox, oy = self.offset[c] & 0xFFFF, (self.offset[c] >> 32) & 0xFFFF
            (x0, y0), (x1, y1) = self.verts[-2], self.verts[-1]
            w = abs(x1 - x0) / 16.0
            h = abs(y1 - y0) / 16.0
            if fbw and w >= 0.9 * min(fbw, 640) and h >= 200:
                s.fullscreen += 1
                if a & 0x40:
                    s.fullscreen_blended += 1


class Stats:
    def __init__(self):
        self.prims = self.prims_set = self.fog_prims = self.blended = self.feedback = 0
        self.fullscreen = self.fullscreen_blended = 0
        self.by_kind = collections.Counter()
        self.targets = collections.Counter()
        self.fog_colours = collections.Counter()
        self.fog_values = collections.Counter()
        self.feedback_pairs = collections.Counter()
        self.drawn_blocks = set()
        self.prev_blocks = set()


def gif_packet(gs, data):
    off = 0
    n = len(data)
    while off + 16 <= n:
        lo, hi = struct.unpack_from('<QQ', data, off)
        off += 16
        nloop = lo & 0x7FFF
        pre = (lo >> 46) & 1
        prim = (lo >> 47) & 0x7FF
        flg = (lo >> 58) & 3
        nreg = (lo >> 60) & 0xF or 16
        regs = [(hi >> (4 * i)) & 0xF for i in range(nreg)]
        if pre and flg == 0:
            gs.write(PRIM, prim)
        if flg == 0:  # PACKED
            for _ in range(nloop):
                for r in regs:
                    if off + 16 > n:
                        return
                    a, b = struct.unpack_from('<QQ', data, off)
                    off += 16
                    if r == 0x0:
                        gs.write(PRIM, a & 0x7FF)
                    elif r == 0x4:  # XYZF2 packed
                        x, y = a & 0xFFFF, (a >> 32) & 0xFFFF
                        z = (b >> 4) & 0xFFFFFF
                        f = (b >> 36) & 0xFF
                        adc = (b >> 47) & 1
                        gs.write(XYZF3 if adc else XYZF2, x | (y << 16) | (z << 32) | (f << 56))
                    elif r == 0x5:  # XYZ2 packed
                        x, y = a & 0xFFFF, (a >> 32) & 0xFFFF
                        adc = (b >> 47) & 1
                        gs.write(XYZ3 if adc else XYZ2, x | (y << 16))
                    elif r == 0xA:  # FOG packed
                        gs.write(FOG, ((b >> 36) & 0xFF) << 56)
                    elif r in (0x6, 0x7, 0x8, 0x9):
                        gs.write(r, a)
                    elif r == 0xE:  # A+D
                        gs.write(b & 0xFF, a)
        elif flg == 1:  # REGLIST
            count = nloop * nreg
            for i in range(count):
                if off + 8 > n:
                    return
                (v,) = struct.unpack_from('<Q', data, off)
                off += 8
                r = regs[i % nreg]
                if r not in (0xE, 0xF):
                    gs.write(r, v)
            if count & 1:
                off += 8
        else:  # IMAGE: nloop quadwords of pixels
            off += nloop * 16


def main():
    p = argparse.ArgumentParser()
    p.add_argument('stream')
    p.add_argument('--every', type=int, default=300, help='print a frame line every N frames')
    p.add_argument('--from', dest='first', type=int, default=0)
    p.add_argument('--to', dest='last', type=int, default=1 << 60)
    args = p.parse_args()
    gs = Gs()
    frame = 0
    total = Stats()
    gs.stats = Stats()
    used_fog = collections.Counter()
    frames_with = collections.Counter()
    print('frame   prims  fog%   blended%  feedback  fullscreen(blended)  top fog colour')
    with open(args.stream, 'rb') as f:
        while True:
            head = f.read(8)
            if len(head) < 8:
                break
            kind, size = struct.unpack('<II', head)
            data = f.read(size)
            if len(data) < size:
                break
            if 1 <= kind <= 3 and args.first <= frame <= args.last:
                gif_packet(gs, data)
            elif kind == 10:
                s = gs.stats
                if args.first <= frame <= args.last and s.prims:
                    for name in ('fog_prims', 'feedback', 'fullscreen'):
                        if getattr(s, name):
                            frames_with[name] += 1
                    frames_with['frames'] += 1
                    total.prims += s.prims
                    total.fog_prims += s.fog_prims
                    total.blended += s.blended
                    total.feedback += s.feedback
                    total.fullscreen += s.fullscreen
                    total.fullscreen_blended += s.fullscreen_blended
                    total.fog_colours.update(s.fog_colours)
                    total.fog_values.update(s.fog_values)
                    total.feedback_pairs.update(s.feedback_pairs)
                    total.by_kind.update(s.by_kind)
                    if frame % args.every == 0:
                        top = s.fog_colours.most_common(1)
                        print('%6d %7d %5.1f %8.1f %9d %10d (%d)       %s' % (
                            frame, s.prims, 100.0 * s.fog_prims / s.prims, 100.0 * s.blended / s.prims, s.feedback,
                            s.fullscreen, s.fullscreen_blended, ('#%06X' % top[0][0]) if top else '-'))
                frame += 1
                last = gs.stats.drawn_blocks
                gs.stats = Stats()
                gs.stats.prev_blocks = last
            elif kind == 11:
                pass
    n = max(frames_with['frames'], 1)
    print('\n== %d frames with drawing' % frames_with['frames'])
    print('fog:        %.1f%% of primitives; on in %.0f%% of frames' % (100.0 * total.fog_prims / max(total.prims, 1),
                                                                         100.0 * frames_with['fog_prims'] / n))
    if total.fog_colours:
        print('            fog colours (BGR as FOGCOL holds them): ' + ', '.join(
            '#%06X x%d' % (c, k) for c, k in total.fog_colours.most_common(6)))
        print('            fog strength (0 = full fog .. 7 = none): ' + ', '.join(
            '%d:%d' % (b, total.fog_values[b]) for b in sorted(total.fog_values)))
    print('feedback:   %d draws (in %.0f%% of frames) - textures drawn from this frame\'s own buffers' % (
        total.feedback, 100.0 * frames_with['feedback'] / n))
    for (tbp, fbp), k in total.feedback_pairs.most_common(8):
        print('            texture block 0x%04x -> target 0x%04x  x%d' % (tbp, fbp, k))
    print('fullscreen: %d sprites (%d blended), in %.0f%% of frames' % (total.fullscreen, total.fullscreen_blended,
                                                                        100.0 * frames_with['fullscreen'] / n))
    print('blended:    %.1f%% of primitives' % (100.0 * total.blended / max(total.prims, 1)))
    print('kinds:      ' + ', '.join('%s %d' % kv for kv in total.by_kind.most_common()))


if __name__ == '__main__':
    sys.exit(main())
