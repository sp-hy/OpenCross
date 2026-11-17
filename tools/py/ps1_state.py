"""PS1 save-state reader for stock DuckStation (state version 92): RAM, VRAM and CPU registers out of a .sav, plus a battle-state watch table.

    python tools/py/ps1_state.py info  <state.sav>                 header, offsets + the watch row of one state
    python tools/py/ps1_state.py watch <dir-or-state> [...]        one row per state (sorted), the named addresses below
    python tools/py/ps1_state.py extract <state.sav> <out-prefix>  writes <prefix>.ram (RAM, size from the file) and <prefix>.vram (1 MiB, 2048 B/row)
    python tools/py/ps1_state.py hex <state.sav> <addr> [len]      hex dump of guest RAM
    python tools/py/ps1_state.py poke <in.sav> <out.sav> ADDR=VAL[:WIDTH] ...   patch guest RAM; the output is always an uncompressed state
    python tools/py/ps1_state.py regs <state.sav|dir> [...]        r0..r31, hi, lo, pc, npc of each state

File layout (little endian):
  0x000 u32 magic 0x43435544 ("DUCC"), 0x004 u32 version (92), 0x008 char title[128], 0x088 char serial[32],
  0x0A8 u32 media_path_length, offset_to_media_path, media_subimage_index, screenshot_compression_type, screenshot_width,
  screenshot_height, screenshot_compressed_size, offset_to_screenshot, data_compression_type (0 none, 1 deflate, 2 zstd, 3 xz),
  data_compressed_size, data_uncompressed_size, offset_to_data (header = 0xD8 bytes).
Body (at offset_to_data, decompressed if needed): StateWrapper stream; a section marker is u32 length + the name (no NUL), e.g.
"System", "CPU", "Bus", "DMA", "GPU", "GPU-VRAM". "Bus" is followed directly by u32 ram_size (0x200000, or 0x800000 with the
8MB option), the RAM bytes sit some words later: RAM is located by the main exe signature at guest 0x80010000
(`ps1_ram.EXE_SIGNATURES`), which also tells the build (Japanese or USA) and so the RAM map the state is read with. VRAM = the 1 MiB right after the "GPU-VRAM" marker. CPU: r0..r31 start 16 bytes after
the end of the "CPU" marker, followed by hi, lo, pc, npc.
Needs the `zstandard` module for zstd states.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps1_ram  # noqa: E402

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
EXE_ADDR = 0x10000
VRAM_TAG = b"GPU-VRAM"
VRAM_SIZE = 0x100000
STATE_MAGIC = 0x43435544
HEADER_SIZE = 0xD8
HDR_FIELDS = ("media_path_length", "offset_to_media_path", "media_subimage_index", "screenshot_compression_type",
              "screenshot_width", "screenshot_height", "screenshot_compressed_size", "offset_to_screenshot",
              "data_compression_type", "data_compressed_size", "data_uncompressed_size", "offset_to_data")
HDR_FIELDS_OFF = 0xA8
REG_NAMES = ("r0 at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra "
             "hi lo pc npc").split()
CPU_REGS_AFTER_MARKER = 16


def marker(name):
    return struct.pack("<I", len(name)) + name.encode()


def decompress_body(raw, hdr):
    comp, off, csize = hdr["data_compression_type"], hdr["offset_to_data"], hdr["data_compressed_size"]
    blob = raw[off:off + csize]
    if comp == 0:
        return blob
    if comp == 2:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(blob)
    raise ValueError("unsupported data compression type %d" % comp)


# Watch table: (name, ps1_ram name, offset, width in bytes, signed). Battle unit i is the ps1_ram name unit<i> (the record
# stride differs between the builds); the field ring slots are bytes 0 / 4 / 8 of `ring`.
WATCH = [
    ("ovl_id", "ovl_id", 0, 4, False),
    ("mt_ovl", "menu_ovl_id", 0, 4, False),
    ("mt_mode", "mt_mode", 0, 1, False),
    ("mt_cur", "mt_cur", 0, 1, False),
    ("mt_scroll", "mt_scroll", 0, 1, False),
    ("title_var", "title_var", 0, 1, False),
    ("mt_5480", "mt_5480", 0, 1, False),
    ("mt_5478", "mt_5478", 0, 1, False),
    ("mt_547c", "mt_547c", 0, 1, False),
    ("ld_5010", "ld_5010", 0, 1, False),
    ("ld_cur", "ld_cur", 0, 4, False),
    ("ld_card", "ld_card", 0, 4, False),
    ("pad_e0", "pad_held", 0, 2, False),
    ("pad_e2", "pad_held", 2, 2, False),
    ("pad_e4", "pad_held", 4, 2, False),
    ("pad_e6", "pad_held", 6, 2, False),
    ("loc", "loc", 0, 2, False),
    ("btl_id", "btl_id", 0, 2, False),
    ("area", "area", 0, 2, False),
    ("btl_frame", "btl_frame", 0, 4, False),
    ("tgt_064b", "tgt_064b", 0, 1, False),
    ("phase_fc40", "phase_fc40", 0, 1, False),
    ("pc_0649", "pc_0649", 0, 1, False),
    ("menu_064a", "menu_064a", 0, 1, False),
    ("cur_064e", "cur_064e", 0, 1, False),
    ("e065a", "e065a", 0, 1, False),
    ("e0652", "e0652", 0, 1, False),
    ("tgt_0650", "tgt_0650", 0, 1, False),
    ("ui_0642", "ui_0642", 0, 1, False),
    ("hint_f0ae", "hint_f0ae", 0, 1, False),
    ("act_kind", "act_kind", 0, 1, False),
    ("act_str", "act_str", 0, 1, False),
    ("act_elem", "act_elem", 0, 2, False),
    ("act_actor", "act_actor", 0, 1, False),
    ("e05da", "act_element", 0, 2, False),
    ("tgt0", "act_targets", 0, 1, False),   # current action target list (0xFF end)
    ("tgt1", "act_targets", 1, 1, False),
    ("tgt2", "act_targets", 2, 1, False),
    ("att_310e", "att_310e", 0, 1, False),
    ("cur_310f", "cur_310f", 0, 1, False),
    ("u3_sta", "unit3", ps1_ram.UNIT_STAMINA, 2, True),
    ("e3372", "e3372", 0, 2, False),
    ("fx_load", "fx_load", 0, 4, False),
    ("ovl", "ovl_resident", 0, 4, False),
    ("ring0", "ring", 0, 1, False),
    ("ring1", "ring", 4, 1, False),
    ("ring2", "ring", 8, 1, False),
    ("party0", "party", 0, 1, False),
    ("party1", "party", 1, 1, False),
    ("party2", "party", 2, 1, False),
    ("u0_hp", "units", ps1_ram.UNIT_HP, 2, True),
    ("u0_gauge", "units", ps1_ram.UNIT_GAUGE, 2, True),
    ("u0_sta", "units", ps1_ram.UNIT_STAMINA, 2, True),
    ("u3_hp", "unit3", ps1_ram.UNIT_HP, 2, True),
    ("u4_hp", "unit4", ps1_ram.UNIT_HP, 2, True),
    ("u5_hp", "unit5", ps1_ram.UNIT_HP, 2, True),
]


class State:
    def __init__(self, path):
        self.path = path
        raw = self.raw = open(path, "rb").read()
        if len(raw) < HEADER_SIZE or struct.unpack_from("<I", raw, 0)[0] != STATE_MAGIC:
            raise ValueError("%s: not a DuckStation state" % path)
        self.version = struct.unpack_from("<I", raw, 4)[0]
        self.title = raw[8:136].split(b"\0")[0].decode("utf-8", "replace")
        self.serial = raw[136:168].split(b"\0")[0].decode("ascii", "replace")
        self.hdr = dict(zip(HDR_FIELDS, struct.unpack_from("<12I", raw, HDR_FIELDS_OFF)))
        self.compressed = self.hdr["data_compression_type"] != 0
        body = self.body = decompress_body(raw, self.hdr)
        self.body_file_off = None if self.compressed else self.hdr["offset_to_data"]
        sig, region = -1, None
        for r, signature in ps1_ram.EXE_SIGNATURES.items():
            sig, region = body.find(signature), r
            if sig >= EXE_ADDR:
                break
        if sig < EXE_ADDR:
            raise ValueError("%s: main exe signature not found" % path)
        self.map = ps1_ram.RamMap(region)   # the build's addresses: st.map.btl_id
        self.ram_off = sig - EXE_ADDR   # offset in the body (file offset = body_file_off + ram_off when uncompressed)
        bus = body.find(marker("Bus"))
        if bus < 0:
            raise ValueError("%s: Bus section not found" % path)
        self.ram_size = struct.unpack_from("<I", body, bus + 7)[0]
        tag = body.find(marker("GPU-VRAM"))
        if tag < 0:
            raise ValueError("%s: GPU-VRAM tag not found" % path)
        self.vram_off = tag + 4 + len(VRAM_TAG)
        self.ram = body[self.ram_off:self.ram_off + self.ram_size]
        self.vram = body[self.vram_off:self.vram_off + VRAM_SIZE]
        self.body_len = len(body)

    def regs(self):
        """{name: value} of r0..r31, hi, lo, pc, npc."""
        t = self.body.find(marker("CPU"))
        if t < 0:
            raise ValueError("%s: CPU section not found" % self.path)
        w = struct.unpack_from("<36I", self.body, t + 7 + CPU_REGS_AFTER_MARKER)
        return dict(zip(REG_NAMES, w))

    def _o(self, addr):
        return (addr & 0x1FFFFFFF) if addr >= 0x80000000 else addr

    def u8(self, a):
        return self.ram[self._o(a)]

    def u16(self, a):
        return struct.unpack_from("<H", self.ram, self._o(a))[0]

    def s16(self, a):
        return struct.unpack_from("<h", self.ram, self._o(a))[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.ram, self._o(a))[0]

    def bytes_at(self, a, n):
        o = self._o(a)
        return self.ram[o:o + n]

    def watch(self, table=WATCH):
        row = {}
        m = self.map
        for name, key, offset, width, signed in table:
            o = self._o(getattr(m, key) + offset)
            v = int.from_bytes(self.ram[o:o + width], "little", signed=signed)
            row[name] = v
        # Derived: the element of the current action = (pgdat record pointer - table base) / 0x2C,
        # the field leader's position, and the first word of the resident effect overlay.
        p, base = self.u32(m.pgdat_ptr), self.u32(m.pgdat_base)
        row["elem"] = (p - base) // 0x2C if base and p >= base and p - base < 0x2C * 600 else -1
        # Field entities: table [fld_entities], stride 0x278, the controlled entity's index is the
        # byte fld_lead (FE 4C); X / Z are 16.16 at +0x3C / +0x44.
        ents, lead = self.u32(m.fld_entities), self.u8(m.fld_lead)
        if 0x80000000 <= ents < 0x80000000 + self.ram_size and lead < 64:
            e = self._o(ents) + lead * 0x278
            row["lead_x"] = struct.unpack_from("<i", self.ram, e + 0x3C)[0] >> 16
            row["lead_z"] = struct.unpack_from("<i", self.ram, e + 0x44)[0] >> 16
        else:
            row["lead_x"] = row["lead_z"] = None
        row["ovl0"] = "%08x" % self.u32(m.effect_ovl)
        return row

    def vram_pixel(self, x, y):
        """BGR555 halfword at VRAM (x, y); rows are 2048 bytes."""
        return struct.unpack_from("<H", self.vram, y * 2048 + x * 2)[0]


def frame_of(path):
    base = os.path.basename(path)
    digits = "".join(ch for ch in base if ch.isdigit())
    return int(digits) if digits else -1


def list_states(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, f) for f in os.listdir(a) if f.endswith(".sav")]
        else:
            out.append(a)
    return sorted(out, key=frame_of)


def window(paths, elem=None):
    """(first, last) state frames of an element cast: first = act_kind 1 and fx_load != 0 (the pack load starts, e.g. run frame 30 of a
    PC cast), last = the last state with act_kind 1 and the effect overlay resident (ovl != 0; it comes ~100 frames after the load and
    ends with the pack). elem filters on e05da."""
    first = last = ovl_first = ovl_last = None
    for p in paths:
        r = State(p).watch()
        if r["act_kind"] != 1 or (elem is not None and r["e05da"] != elem):
            continue
        f = frame_of(p)
        if first is None and r["fx_load"] != 0:
            first = f
        if first is None and ovl_first is None and r["ovl"] != 0:
            ovl_first = f   # scripted casts: the fx_load blip can fall between two states; the resident overlay still marks the pack
        if first is not None and (r["ovl"] != 0 or r["fx_load"] != 0):
            last = f
        if r["ovl"] != 0:
            ovl_last = f
    if first is None and ovl_first is not None:
        return (ovl_first, ovl_last)
    return (first, last if last is not None else first)


def manifest(paths, elem=None):
    first, last = window(paths, elem)
    out = ["cast_frame: %s (first state with act_kind 1 and fx_load != 0)" % first, "effect_last_frame: %s" % last]
    for p in paths:
        if frame_of(p) == first:
            st = State(p)
            r = st.watch()
            out.append("caster_unit: %d  target_unit: %d  (act_actor / e0637)  party0: %d" % (r["act_actor"], r["tgt0"], r["party0"]))
            ov = next((p2 for p2 in paths if frame_of(p2) >= first and State(p2).watch()["ovl"] != 0), None)
            out.append("pack_resident_from: %s (first state with ovl != 0)" % (frame_of(ov) if ov else None))
            for nm, stt in (("at cast", st), ("at pack start", State(ov) if ov else st)):
                eye = [stt.s16(stt.map.camera_eye + 2 * k) for k in range(3)]
                tg = [stt.s16(stt.map.camera_target + 2 * k) for k in range(3)]
                out.append("camera %s: eye %s target %s H %d (RAM camera_eye / target / h; pick name: ref_info.py)" % (
                    nm, eye, tg, stt.u16(stt.map.camera_h)))
            break
    return chr(10).join(out)


def harvest(paths, out):
    """Runs of consecutive states with the same (actor, element) while an element effect is active (act_kind 1 and the pack loading
    or resident: fx_load != 0 or ovl != 0). The target is the first target unit (e0637) at the run's first state."""
    runs, cur = [], None
    for p in paths:
        r = State(p).watch()
        act = r["act_kind"] == 1 and (r["fx_load"] != 0 or r["ovl"] != 0)
        key = (r["act_actor"], r["e05da"])
        f = frame_of(p)
        if act and cur and cur[0] == key and f - cur[2] <= 180:   # the load blip (fx_load) and the resident overlay are ~100 frames apart
            cur[2] = f
        elif act:
            if cur:
                runs.append(cur)
            cur = [key, f, f, r["tgt0"]]
    if cur:
        runs.append(cur)
    with open(out, "w") as fh:
        fh.write("\t".join(["start", "end", "unit", "element", "target"]) + "\n")
        for (a, e), f0, f1, t in runs:
            fh.write("\t".join(str(v) for v in (f0, f1, a, e, t)) + "\n")
    print("harvested %d runs -> %s" % (len(runs), out))


def campick(paths):
    """Sequence-task tags / names that are camera shots, in order of first appearance (fx_ref.py seqs tags)."""
    import re
    import fx_ref as fr
    shot = re.compile(r"^(ch\d+|tgt\w*|apr\w*|\d+\w?)$")
    seen = []
    for p in paths:
        try:
            st = State(p)
            reg = fr.Registry(st)
            base, ts = fr.tasks(st)
            for e, cb, d in ts:
                if cb == st.map.cb_seq:
                    for n in (fr.tag(st.u32(d + 0x10)), reg.name_of_data(st.u32(d + 4))):
                        n = n.rstrip(".")
                        if shot.match(n) and n.strip("0") and n not in seen:
                            seen.append(n)
        except Exception:
            continue
    return ",".join(seen)


def fmt_row(row):
    return " ".join("%s=%s" % (k, v) for k, v in row.items())


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[0]
    if cmd == "info":
        st = State(argv[1])
        print("%s: %s v%d %s (%s build), body %d B (%s), RAM %d B at %d, VRAM at %d" % (
            argv[1], st.serial, st.version, st.title.encode("ascii", "replace").decode(), st.map.region, st.body_len,
            {0: "uncompressed", 1: "deflate", 2: "zstd", 3: "xz"}.get(st.hdr["data_compression_type"], "?"), st.ram_size, st.ram_off, st.vram_off))
        print(fmt_row(st.watch()))
    elif cmd == "watch":
        fields = None
        rest = argv[1:]
        if rest and rest[0] == "--fields":
            fields = rest[1].split(",")
            rest = rest[2:]
        for p in list_states(rest):
            try:
                st = State(p)
            except Exception as e:  # noqa: BLE001
                print("%s: %s" % (p, e))
                continue
            row = st.watch()
            if fields:
                row = {k: row.get(k) for k in fields}
            print("%6d %s" % (frame_of(p), fmt_row(row)))
    elif cmd == "extract":
        st = State(argv[1])
        open(argv[2] + ".ram", "wb").write(st.ram)
        open(argv[2] + ".vram", "wb").write(st.vram)
        print("wrote %s.ram (%d) and %s.vram (%d)" % (argv[2], len(st.ram), argv[2], len(st.vram)))
    elif cmd == "poke":
        # poke <in.sav> <out.sav> ADDR=VALUE[:WIDTH] ...: the output is an uncompressed state (header patched, body patched)
        st = State(argv[1])
        body = bytearray(st.body)
        for spec in argv[3:]:
            a, v = spec.split("=")
            width = 1
            if ":" in v:
                v, width = v.split(":")
                width = int(width)
            addr, val = int(a, 0), int(v, 0)
            off = st.ram_off + st._o(addr)
            old = int.from_bytes(body[off:off + width], "little")
            body[off:off + width] = val.to_bytes(width, "little")
            print("poke %08X: %d -> %d (%d bytes)" % (addr, old, val, width))
        hdr = bytearray(st.raw[:st.hdr["offset_to_data"]])
        h = dict(st.hdr, data_compression_type=0, data_compressed_size=len(body), data_uncompressed_size=len(body))
        struct.pack_into("<12I", hdr, HDR_FIELDS_OFF, *[h[k] for k in HDR_FIELDS])
        open(argv[2], "wb").write(bytes(hdr) + bytes(body))
        print("wrote", argv[2])
    elif cmd == "regs":
        for p in list_states(argv[1:]):
            r = State(p).regs()
            print(os.path.basename(p), " ".join("%s=%08x" % (k, v) for k, v in r.items()))
    elif cmd == "window":
        # window <dir> [elem]: first / last state frame whose action is an element cast (act_kind 1, fx_load != 0, e05da == elem)
        print(window(list_states([argv[1]]), int(argv[2]) if len(argv) > 2 else None))
    elif cmd == "manifest":
        # manifest <dir> [elem]: cast frame (first state of the effect), caster / target unit, raw camera, the watch table
        print(manifest(list_states([argv[1]]), int(argv[2]) if len(argv) > 2 else None))
    elif cmd == "prune":
        # prune <dir> <first> <last>: delete states (and ../frames PNGs) whose run frame is outside [first, last]
        lo, hi = int(argv[2]), int(argv[3])
        n = 0
        for d in (argv[1], os.path.join(os.path.dirname(argv[1].rstrip("/\\")), "frames")):
            if os.path.isdir(d):
                for f in os.listdir(d):
                    if frame_of(f) not in range(lo, hi + 1) and f.endswith((".sav", ".png")):
                        os.remove(os.path.join(d, f))
                        n += 1
        print("pruned", n)
    elif cmd == "campick":
        # campick <dir>: camera shot sequences (ch10..ch13, tgtN, aprN, numbered shots) by first appearance over the states, comma list
        print(campick(list_states([argv[1]])))
    elif cmd == "harvest":
        # harvest <dir> <out.tsv>: enemy / PC element casts by run of (actor, element) over the sampled states
        harvest(list_states([argv[1]]), argv[2])
    elif cmd == "hex":
        st = State(argv[1])
        addr = int(argv[2], 0)
        n = int(argv[3], 0) if len(argv) > 3 else 64
        b = st.bytes_at(addr, n)
        for i in range(0, len(b), 16):
            print("%08X  %s" % (addr + i, b[i:i + 16].hex(" ")))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
