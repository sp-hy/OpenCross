"""Reference-capture analysis for the battle effect comparisons: brightness / colour curves of PNG frame
sequences, the PS1 display buffer of a DuckStation state, and the effect engine's live objects read out of a state's RAM.

    python tools/py/fx_ref.py frames  DIR [--every N] [--crop x y w h] [--to W H] [--grid C R] [--from A] [--upto B]
        one line per frame: mean luma / R / G / B, % bright (L >= 200), % blue-dominant, % white, % dark, [grid of mean luma]
    python tools/py/fx_ref.py vram    STATE|DIR [--grid C R]
        the two 320 x 224 display buffers (VRAM y 0 / 240) of each state: the same statistics (15-bit -> 8-bit, x 255 / 31)
    python tools/py/fx_ref.py objects STATE|DIR [--name TAG] [--from A] [--upto B]
        the effect engine's task table (ym_effect_work, base = sentinel + 0xC - 0xC000): every live particle object
        (callback 0x80038E60) and the damage digits' damv / dam1 stages (0x800B07B8 / 0x800B06D4, "stage ..."): emitter / sprite name,
        position (16.16 high halves), velocity, scale, angles, colour, life, bias, flags
        --mat: also the object matrix (+0x30, s16 / 4096); --bit7: only objects with flag bits 15 + 7
    python tools/py/fx_ref.py seqs    STATE|DIR          the sequence tasks (callback 0x8002A26C): tag, elapsed, wait, cursor
    python tools/py/fx_ref.py warp    STATE|DIR          the screen-copy warp globals 0x800720D8..0x800720FF (ops 0x3E-0x40)
    python tools/py/fx_ref.py models  STATE|DIR          the unit model blocks (ctrl 0x800E31B0 + 0x30 u): +0x7E flags, colour, pos, rot, scale
    python tools/py/fx_ref.py nodes   STATE              the effect registry (0x800721E4): type, name, data pointer

Needs PIL + numpy (Python 3.13 has both) and zstandard (ps1_state.py).
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1_state import State, frame_of, list_states  # noqa: E402

CB_OBJECT = 0x80038E60
# The damage digits' first two stages run their own callbacks: damv (flight, battle.prg 0x800B07B8) and dam1 (rest, 0x800B06D4);
# dam2 (fade) is a plain object. `objects` lists all three ("cd" = the damv countdown +0x82: the digit waits while it is >= 1).
CB_DAMV = 0x800B07B8
CB_DAM1 = 0x800B06D4
CB_SEQ = 0x8002A26C
WORK_SIZE = 0xC000


def opt(args, name, n=1, default=None, conv=int):
    if name in args:
        i = args.index(name)
        vals = [conv(v) for v in args[i + 1:i + 1 + n]]
        del args[i:i + 1 + n]
        return vals if n > 1 else vals[0]
    return default


def flag(args, name):
    if name in args:
        args.remove(name)
        return True
    return False


# ---------------------------------------------------------------------------------------------------------------- pixels
def stats(rgb, grid=None):
    """rgb: H x W x 3 uint8 array."""
    f = rgb.astype(np.float32)
    lum = 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]
    n = lum.size
    r, g, b = f[..., 0], f[..., 1], f[..., 2]
    out = {
        "L": lum.mean(),
        "R": r.mean(),
        "G": g.mean(),
        "B": b.mean(),
        "bright": 100.0 * np.count_nonzero(lum >= 200) / n,
        "blue": 100.0 * np.count_nonzero((b >= r + 48) & (b >= g + 24)) / n,
        "white": 100.0 * np.count_nonzero(np.min(f, axis=2) >= 224) / n,
        "dark": 100.0 * np.count_nonzero(lum < 8) / n,
    }
    if grid:
        c, rws = grid
        h, w = lum.shape
        cells = []
        for j in range(rws):
            row = []
            for i in range(c):
                row.append(lum[j * h // rws:(j + 1) * h // rws, i * w // c:(i + 1) * w // c].mean())
            cells.append(row)
        out["grid"] = cells
    return out


def fmt(s):
    line = "L %6.2f R %6.2f G %6.2f B %6.2f bright %5.2f blue %5.2f white %5.2f dark %5.2f" % (
        s["L"], s["R"], s["G"], s["B"], s["bright"], s["blue"], s["white"], s["dark"])
    if "grid" in s:
        line += "  grid " + " | ".join(" ".join("%3.0f" % v for v in row) for row in s["grid"])
    return line


def load_png(path, crop=None, to=None):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    if crop:
        x, y, w, h = crop
        im = im.crop((x, y, x + w, y + h))
    if to:
        im = im.resize((to[0], to[1]), Image.BOX)
    return np.asarray(im)


def cmd_frames(args):
    every = opt(args, "--every", default=1)
    crop = opt(args, "--crop", 4)
    to = opt(args, "--to", 2)
    grid = opt(args, "--grid", 2)
    lo = opt(args, "--from", default=0)
    hi = opt(args, "--upto", default=1 << 30)
    d = args[0]
    files = sorted((f for f in os.listdir(d) if f.endswith(".png")), key=frame_of)
    for f in files:
        n = frame_of(f)
        if n < lo or n > hi or (n - lo) % every:
            continue
        print("%6d %s" % (n, fmt(stats(load_png(os.path.join(d, f), crop, to), grid))))


# ---------------------------------------------------------------------------------------------------------------- VRAM
def display_buffer(st, y0, w=320, h=224):
    rows = np.frombuffer(st.vram, dtype="<u2").reshape(512, 1024)[y0:y0 + h, :w]
    r = (rows & 31).astype(np.uint16) * 255 // 31
    g = ((rows >> 5) & 31).astype(np.uint16) * 255 // 31
    b = ((rows >> 10) & 31).astype(np.uint16) * 255 // 31
    return np.stack([r, g, b], axis=2).astype(np.uint8)


def cmd_vram(args):
    grid = opt(args, "--grid", 2)
    for p in list_states(args):
        st = State(p)
        for tag, y0 in (("A", 0), ("B", 240)):
            print("%6d %s %s" % (frame_of(p), tag, fmt(stats(display_buffer(st, y0), grid))))


# ---------------------------------------------------------------------------------------------------------------- RAM
def tag(v):
    s = struct.pack("<I", v)
    return "".join(chr(c) if 32 <= c < 127 else "." for c in s)


class Registry:
    def __init__(self, st):
        """st: a State, or a read(addr, n) callable returning n bytes of guest RAM (a live session's read)."""
        if callable(st):
            u32 = lambda a: int.from_bytes(st(a, 4), "little")  # noqa: E731
        else:
            u32 = st.u32
        self.nodes = []
        p = u32(0x800721E4)
        n = 0
        while 0x80000000 <= p < 0x80200000 and n < 4096:
            name = u32(p + 4)
            typ = u32(p + 8)
            data = u32(p + 0xC) if (typ & 0x80) else p + 0xC
            self.nodes.append((p, typ & 0x3F, tag(name), data, typ))
            p = u32(p)
            n += 1
        self.by_data = {d: (t, nm) for (_, t, nm, d, _) in self.nodes}
        self.by_node = {a: (t, nm) for (a, t, nm, d, _) in self.nodes}

    def name_of_data(self, d):
        e = self.by_data.get(d)
        return e[1] if e else "?"

    def name_of_node(self, a):
        e = self.by_node.get(a)
        return e[1] if e else "?"


def walk(st, base, end):
    """The chain from the head entry (base + 4) by next index * 4; valid iff it ends at the sentinel 0x80072118 (= base + size - 0xC)."""
    out = []
    idx = st.u16(base + 4)
    n = 0
    e = None
    seen = set()
    while idx and n < 20000:
        e = base + idx * 4
        if e in seen or e > end:
            return None
        seen.add(e)
        word = st.u32(e)
        if word & 0x80000000:
            out.append((e, st.u32(e + 4), e + 0xC))
        idx = word & 0xFFFF
        n += 1
    return out if e == end else None


def tasks(st):
    """Entries: +0 u32 (bit 31 active, bits 16-30 previous index, bits 0-15 next index; indices in 4-byte units from the work base), +4 the
    tick callback, +8 the draw callback, +0xC the task data (the 0x94-byte particle object, the sequence task, ...). The work size is the
    allocation size argument (0xC000 on the PS1, 0x13000 in RDE, which keeps it above the first 2 MB), so it is searched."""
    end = st.u32(0x80072118)
    if not (0x80000000 <= end < 0x80000000 + st.ram_size):
        return None, []
    for size in (WORK_SIZE, 0x13000, 0x4000, 0x100) + tuple(range(0x1000, 0x40000, 4)):
        base = end + 0xC - size
        out = walk(st, base, end)
        if out is not None and len(out) >= 1:
            return base, out
    return None, []


def s16x3(st, a):
    return (st.s16(a), st.s16(a + 2), st.s16(a + 4))


def cmd_objects(args):
    want = opt(args, "--name", default=None, conv=str)
    lo = opt(args, "--from", default=0)
    hi = opt(args, "--upto", default=1 << 30)
    mat = flag(args, "--mat")      # also print the object matrix (+0x30, 3x3 s16 / 4096)
    bit7 = flag(args, "--bit7")    # only objects with flag bits 15 + 7 (the attach joint's rotation)
    for p in list_states(args):
        fr = frame_of(p)
        if fr < lo or fr > hi:
            continue
        st = State(p)
        reg = Registry(st)
        base, ts = tasks(st)
        objs = [(e, cb, d) for (e, cb, d) in ts if cb in (CB_OBJECT, CB_DAMV, CB_DAM1)]
        print("== %s: work base %s, %d tasks, %d objects" % (os.path.basename(p), hex(base) if base else None, len(ts), len(objs)))
        for e, cb, o in objs:
            owner = reg.name_of_data(st.u32(o + 0xC))
            sprite = reg.name_of_node(st.u32(o + 0x1C))
            if want and want not in (owner, sprite):
                continue
            if bit7 and (st.u32(o + 0x88) & 0x8080) != 0x8080:
                continue
            pos = tuple(st.s16(o + k + 2) for k in (0, 4, 8))
            vel = tuple(struct.unpack_from("<i", st.ram, st._o(o + k))[0] for k in (0x10, 0x14, 0x18))
            acc = tuple(struct.unpack_from("<i", st.ram, st._o(o + k))[0] for k in (0x20, 0x24, 0x28))
            sc = s16x3(st, o + 0x50)
            ang = s16x3(st, o + 0x5C)
            col = (st.u16(o + 0x68), st.u16(o + 0x6A), st.u16(o + 0x6C))
            drawc = (st.u8(o + 0x74), st.u8(o + 0x75), st.u8(o + 0x76))
            life = st.u16(o + 0x7C)
            bias = st.s16(o + 0x7E)
            flags = st.u32(o + 0x88)
            frame = st.s16(o + 0x7A)
            stage = {CB_DAMV: " stage damv cd %d" % st.s16(o + 0x82), CB_DAM1: " stage dam1"}.get(cb, "")
            print("  %s em %s spr %s pos %6d %6d %6d vel %7d %7d %7d acc %6d %6d %6d sc %5d %5d %5d ang %5d %5d %5d col %4d %4d %4d rgb %3d %3d %3d life %4d bias %4d fr %2d fl %08x%s" % (
                hex(o), owner, sprite, *pos, *(v >> 6 for v in vel), *(v >> 3 for v in acc), *sc, *ang, *col, *drawc, life, bias, frame, flags, stage))
            if mat:
                print("    mat " + " ".join("%.3f" % (st.s16(o + 0x30 + 2 * k) / 4096.0) for k in range(9)))


def cmd_seqs(args):
    for p in list_states(args):
        st = State(p)
        reg = Registry(st)
        base, ts = tasks(st)
        rows = []
        for e, cb, d in ts:
            if cb == CB_SEQ:
                seq = st.u32(d + 4)
                rows.append("seq %s hdr %s flags %08x op@%s label@%s tag %s elapsed %d wait %d cursor %d inherited %d" % (
                    reg.name_of_data(seq), hex(seq), st.u32(seq) if 0x80000000 <= seq < 0x80200000 else 0, hex(st.u32(d + 8)), hex(st.u32(d + 0xC)),
                    tag(st.u32(d + 0x10)), st.s16(d + 0x14), st.s16(d + 0x18), st.s16(d + 0x1A), st.s16(d + 0x1C)))
            elif cb != CB_OBJECT:
                rows.append("task cb %s data %s" % (hex(cb), hex(d)))
        print("== %s: %d tasks" % (os.path.basename(p), len(ts)))
        for r in rows:
            print("  " + r)


def cmd_warp(args):
    for p in list_states(args):
        st = State(p)
        on = st.u8(0x800720D8)  # a byte flag
        ang = s16x3(st, 0x800720E0)
        tr = s16x3(st, 0x800720E8)
        col = (st.u8(0x800720F0), st.u8(0x800720F1), st.u8(0x800720F2))
        off = (st.s16(0x800720F4), st.s16(0x800720F6))
        print("%6d warp on %d ang %5d %5d %5d trans %5d %5d %5d col %3d %3d %3d off %4d %4d abr %d fog %d slow %d/%d" % (
            frame_of(p), on, *ang, *tr, *col, *off, st.u8(0x800720FC), st.u8(0x800721E0), st.u8(0x800720FF), st.u8(0x80072100)))


def cmd_models(args):
    for p in list_states(args):
        st = State(p)
        print("== %s" % os.path.basename(p))
        for u in range(12):
            c = 0x800E31B0 + 0x30 * u
            m = st.u32(c)
            if not (0x80000000 <= m < 0x80200000):
                continue
            pos = tuple(struct.unpack_from("<i", st.ram, st._o(m + k))[0] >> 12 for k in (0x84, 0x88, 0x8C))
            print("  unit %2d ctrl %s model %s home %6d %6d %6d flags7e %04x col %3d %3d %3d pos %6d %6d %6d rot %5d %5d %5d scale %5d %5d %5d grp %d f14 %08x f18 %04x" % (
                u, hex(c), hex(m), st.s16(c + 4), st.s16(c + 6), st.s16(c + 8), st.u16(m + 0x7E), st.u8(m + 0x2C), st.u8(m + 0x2D), st.u8(m + 0x2E), *pos,
                *s16x3(st, m + 0xA4), *s16x3(st, m + 0xAC), st.s16(c + 0x10), st.u32(c + 0x14), st.u16(c + 0x18)))


def cmd_nodes(args):
    st = State(args[0])
    for (a, t, nm, d, typ) in Registry(st).nodes:
        print("  node %s type %2d name %s data %s (%08x)" % (hex(a), t, nm, hex(d), typ))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, args = argv[0], list(argv[1:])
    {"frames": cmd_frames, "vram": cmd_vram, "objects": cmd_objects, "seqs": cmd_seqs, "warp": cmd_warp, "models": cmd_models, "nodes": cmd_nodes}[cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
