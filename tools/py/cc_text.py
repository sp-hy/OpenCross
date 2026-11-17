"""Chrono Cross dialogue / text tooling (original work).

Run with:  python -I tools/py/cc_text.py <command> ...

Commands
  langs                       lang.dat members with row counts
  room <name|id> [lang]       every message of a room: JP stream (decoded) + translation
  coverage [lang]             E0/E3/E4-referenced messages vs translation rows, all rooms
  sysmes [lang] [--table T] [--cat N]   dump a language-column table (default sysmes)
  font                        font.dat / ui.dat spritefonts (DirectXTK header parse)
  glyphs [--write]            derive the JP glyph table by aligning jp.csv with the disc bytes
  render <name|id> <msg> <out.png> [--scale N]   render one JP message from the exe font bitmap
  fontsheet <out.png> [--first N --count N]       render the exe glyph bitmaps as a grid

Sources: data/cdrom.dat (rooms, prog/slps_023.64), data/lang.dat, data/font.dat, data/ui.dat.
"""
import argparse
import collections
import csv
import io
import os
import re
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cc_fieldscript as fs  # noqa: E402  (room archive, script decoder, LZSS)

OC = os.path.normpath(os.path.join(HERE, "..", ".."))
GAME = os.path.normpath(os.path.join(OC, ".."))
DATA = os.path.join(GAME, "data")
ROOMIDS = os.path.join(HERE, "data", "text_room_ids.csv")
ROOMIDS_FALLBACK = os.path.join(GAME, "research", "data", "text", "room_to_mapfile.csv")
GLYPHS = os.path.join(HERE, "data", "jp_glyphs.tsv")

LANGS = ["en", "fr", "it", "de", "es", "jp"]          # RDE language index 0..5
BS = chr(92)                                           # literal backslash (jp.csv "\n" is BS + 'n')

# speaker ids 0..49 -> brace-tag name (research/data/text/speaker_ids.csv); 44..48 share 43
SPEAKERS = ("SERGE KID GUILE NORRIS NIKKI VIPER RIDDEL KARSH ZOAH MARCY KORCHA LUCCIA POSHUL RAZZLY ZAPPA ORCHA "
            "RADIUS FARGO MACHA GLENN LEENA MIKI HARLEY JANICE DRAGGY STARKY SPRIGG MOJO TURNIP NEOFIO GRECO SKELLY "
            "FUNGUY IRENES MEL LEEA VAN SNEFF STEENA DOC GROBYC PIERRE ORLHA PIP").split() + [None] * 5 + ["LYNX"]
NAME_MAP = {"{%s%%" % n: i for i, n in enumerate(SPEAKERS) if n}

# PS1 message control codes: code -> (name, argument bytes, RDE markup template)
CTRL = {
    0x00: ("END", 0, "{mend}"), 0x01: ("NL", 0, BS + "n"), 0x02: ("PAGE", 0, "{bpage}"),
    0x03: ("WAIT", 0, "{bwait}"), 0x04: ("DELAY", 1, "{wait(%d)}"), 0x05: ("SPEED", 1, "{speed(%d)}"),
    0x06: ("AUTOCLOSE", 1, "{wend(%d)}"), 0x07: ("ITEM", 2, "{item(%d)}"), 0x08: ("NAME", 1, None),
    0x0A: ("NUM", 1, "{num(%d)}"), 0x0B: ("NUM", 1, "{num(%d)}"), 0x0E: ("COLOR", 1, "{color(%d)}"),
    0x10: ("DIGITS", 1, "{input(%d)}"), 0x12: ("ACCENT", 1, "{gram(%d)}"), 0x15: ("SCRNAME", 1, "{playwork(%d)}"),
    0x16: ("FWAIT", 1, "{fwait(%d)}"), 0x17: ("FSET", 1, "{fset(%d)}"), 0x18: ("PARTYNAME", 1, "{PARTY%d%%}"),
    0x19: ("SCRSTR", 1, "{itemwork(%d)}"), 0x1B: ("BUTTON", 1, "{key(%d)}"), 0x1C: ("CASE", 1, "{case(%d)}"),
    0x1E: ("SUFFIX", 1, "{suffix(%d)}"), 0x1F: ("ACCENTPH", 0, "{accent}"),
}
PREFIX_MIN = 0xF8          # F8..FE (FF never seen as a prefix): next byte belongs to the same glyph


def out(s=""):
    sys.stdout.write(s + "\n")


# --------------------------------------------------------------------------- IO helpers
def decode_text(b):
    if b[:2] == b"\xff\xfe":
        return b[2:].decode("utf-16-le")
    if b[:3] == b"\xef\xbb\xbf":
        return b[3:].decode("utf-8")
    return b.decode("utf-8")


class Lang:
    """lang.dat access plus the RDE field-message lookup."""

    def __init__(self, path=None):
        self.z = zipfile.ZipFile(path or os.path.join(DATA, "lang.dat"))
        self._rows = {}
        self._cache = {}

    def read(self, name):
        return self.z.read(name)

    def rows(self, name):
        """list of (room, msg, text) in file order, plus 'loadable' flags (stop-at-first-greater-room rule)."""
        if name in self._rows:
            return self._rows[name]
        txt = self.read(name).decode("utf-8-sig")
        res, mx = [], -1
        for line in txt.split("\n"):
            line = line.rstrip("\r")
            if not line or line.startswith("﻿") or line.startswith("#"):
                continue
            p = line.split("\t", 2)
            if len(p) < 2:
                continue
            try:
                r, m = int(p[0]), int(p[1])
            except ValueError:
                continue
            res.append((r, m, p[2] if len(p) > 2 else "", r >= mx))
            mx = max(mx, r)
        self._rows[name] = res
        return res

    def table(self, name, which):
        key = (name, which)
        if key not in self._cache:
            self._cache[key] = {}
            for r, m, t, ok in self.rows(name):
                if ok:
                    self._cache[key][(r, m)] = t
        return self._cache[key]

    def ex_rows(self, lang):
        """Ex_<lang>.csv: (room,msg) -> [None, v1..v43, v43 x5, None] (50 slots)."""
        key = ("ex", lang)
        if key in self._cache:
            return self._cache[key]
        d = {}
        name = "Ex_%s.csv" % lang
        if name in self.z.namelist():
            txt = self.read(name).decode("utf-8-sig")
            mx = -1
            for line in txt.split("\n"):
                line = line.rstrip("\r")
                p = line.split("\t", 2)
                if len(p) < 3:
                    continue
                try:
                    r, m = int(p[0]), int(p[1])
                except ValueError:
                    continue
                ok = r >= mx
                mx = max(mx, r)
                if not ok:
                    continue
                s = p[2]
                v = [None]
                pos = 0
                for _ in range(43):
                    a = s.find("{", pos)
                    if a < 0:
                        break
                    b = s.find("\t", a)
                    if b < 0:
                        b = len(s)
                    v.append(s[a:b])
                    pos = b
                while len(v) < 44:
                    v.append(v[-1] if len(v) > 1 else None)
                v += [v[43]] * 5 + [None]
                d[(r, m)] = v
        self._cache[key] = d
        return d

    def mesgen(self, lang):
        key = ("mesgen", lang)
        if key in self._cache:
            return self._cache[key]
        name = "mesgen_en.csv" if lang == "en" else "mesgen.csv"
        rows = []
        for line in decode_text(self.read(name)).split("\n"):
            line = line.rstrip("\r")
            toks = [t for t in line.split(",") if t]           # wcstok collapses empty fields
            if not toks:
                continue
            vals = toks[1:]
            vals += [vals[0] if vals else ""] * (64 - len(vals))
            rows.append((toks[0], vals))
        self._cache[key] = rows
        return rows

    def wend(self, lang):
        if lang == "jp":
            return {}
        t = self.table("mess_wend.csv", "wend")
        return t

    def main_text(self, lang, room, msg):
        """main[msg] after the wend rule, or None if the row does not exist / is not loadable."""
        t = self.table(lang + ".csv", "main").get((room, msg))
        if t is None:
            return None
        w = self.wend(lang).get((room, msg))
        if w and "{wend(" in t:
            t = t[:t.index("{wend(")] + w
        return t

    def resolve(self, lang, room, msg, speaker=0):
        """Final string per 0x4517B0.  Returns (text or None, info dict)."""
        base = self.main_text(lang, room, msg)
        info = {"speaker": None, "variant": False}
        if base is None:
            return None, info
        ex = self.ex_rows(lang).get((room, msg)) if lang in ("fr", "it", "de", "es") else None
        if ex and ex[1] is not None:
            outs = ex[1]
            sp = detect_speaker(outs, speaker, lang)
            info["variant"] = True
            res = ex[sp] if sp != 0 else base
            res = "" if res is None else res
        else:
            res = base
            sp = detect_speaker(res, speaker, lang)
        info["speaker"] = sp
        for r, (key, vals) in enumerate(self.mesgen(lang)):
            n = 0
            while key in res and n < 4096:
                rep = vals[sp] if sp < len(vals) else vals[0]
                if lang == "en" and r == 26 and sp == 14 and room == 0x1D6 and msg in (0, 3, 6):
                    rep = "ah"
                res = res.replace(key, rep, 1)
                n += 1
        res = res.replace(BS + "n", "\n").replace("{n/a}", "")
        if lang != "jp":
            res = normalise_nonjp(res, lang)
        return res, info


def normalise_nonjp(s, lang):
    o = []
    for ch in s:
        c = ord(ch)
        if c == 0x20 or 0x2002 <= c <= 0x200B or c in (0x3000, 0xFEFF, 9):
            o.append(" ")
        elif c in (0x2018, 0x2019):
            o.append("'")
        elif c == 0x201D:
            o.append('"')
        elif c == 0x201C:
            o.append(chr(0x201C) if lang == "de" else '"')
        else:
            o.append(ch)
    return "".join(o)


def detect_speaker(s, dflt, lang="en"):
    """0x451580: scan the leading run of {tags}.  {PARTYn%} and {playwork(0)} depend on live game state, so
    they are skipped here (documented limitation); returns the speaker id, default 0 -> 1."""
    i, n = 0, len(s)
    while i < n:
        while i < n and s[i] in " \t":
            i += 1
        if i >= n or s[i] != "{":
            break
        j = s.find("}", i)
        if j < 0:
            break
        tag = s[i:j]                                            # without the closing brace
        if "%" in tag:
            k = tag.index("%") + 1
            v = NAME_MAP.get(tag[:k])
            if v:
                if lang == "jp" and v == 10 and s[j + 1:j + 2] == "母":
                    return 18
                return v
        i = j + 1
    return dflt if dflt else 1


# --------------------------------------------------------------------------- rooms
def load_room_ids(path=None):
    """room id -> file name stem (no ext).  Returns (id->name, name->[ids])."""
    p = path or (ROOMIDS if os.path.exists(ROOMIDS) else ROOMIDS_FALLBACK)
    ids, names = {}, collections.defaultdict(list)
    with open(p, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            nm = row["file"].rsplit("/", 1)[-1][:-4]
            ids[int(row["room"])] = nm
            names[nm].append(int(row["room"]))
    return ids, dict(names)


def resolve_room_arg(arg, ids, names):
    if arg.isdigit() and int(arg) in ids:
        r = int(arg)
        return ids[r], [r]
    nm = os.path.basename(arg)
    if nm.endswith(".bin"):
        nm = nm[:-4]
    if nm in names:
        return nm, names[nm]
    raise SystemExit("unknown room %r (name like arni_01 or numeric id)" % arg)


def jp_stream(raw, glyphs=None):
    """Decode a message byte stream to RDE-style markup (the jp.csv notation).  Unknown glyphs print [hex]."""
    o = []
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if c >= PREFIX_MIN and i + 1 < n:
            g = raw[i:i + 2]
            i += 2
        elif c >= 0x20:
            g = raw[i:i + 1]
            i += 1
        else:
            name, na, tpl = CTRL.get(c, ("C%02X" % c, 0, "{c%02X}" % c))
            args = raw[i + 1:i + 1 + na]
            i += 1 + na
            if c == 0x08 and args:
                nm = SPEAKERS[args[0]] if args[0] < len(SPEAKERS) and SPEAKERS[args[0]] else None
                o.append("{%s%%}" % nm if nm else "{name(%d)}" % args[0])
            elif "%d" in (tpl or ""):
                v = int.from_bytes(args, "little") if args else 0
                o.append(tpl % v if tpl.count("%") == 1 else tpl.replace("%d", str(v)).replace("%%", "%"))
            else:
                o.append(tpl)
            continue
        ch = glyphs.get(bytes(g)) if glyphs else None
        o.append(ch if ch else "[%s]" % bytes(g).hex())
    return "".join(o)


def glyph_index(g):
    """Glyph bytes -> index into the exe bitmap table (used by the renderer)."""
    if g[0] < PREFIX_MIN:
        return g[0] - 0x20
    return (g[0] - 0xF7) * 256 + (g[1] - 0x20)


def load_glyph_table(path=GLYPHS):
    t = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                p = line.rstrip("\n").split("\t")
                if len(p) >= 3 and p[2]:
                    t[bytes.fromhex(p[0])] = p[2]
    return t


def esc(s):
    return s.replace("\n", BS + "n")


def cmd_room(a):
    ids, names = load_room_ids(a.roomids)
    name, rids = resolve_room_arg(a.room, ids, names)
    arc = fs.Archive(os.path.join(DATA, "cdrom.dat"))
    d = fs.Dialogue(arc.dialogue_blob(name))
    gl = load_glyph_table()
    lang = Lang()
    lg = a.lang
    for rid in rids:
        out("; room %s (id %d, lang.dat key room=%d): %d messages%s" % (
            name, rid, rid, len(d.msgs), "" if gl else "; glyph table missing (run: glyphs --write)"))
        for (i, ptr, w1, w2, w3, raw) in d.msgs:
            out("[%3d] hdr w1=%d lines=%d px=%d" % (i, w1, w2, w3))
            out("  JP*: " + jp_stream(raw, gl))
            t, info = lang.resolve(lg, rid, i)
            if t is None:
                out("  %s : <no row -> MESSAGE NOT FOUND>" % lg.upper())
            else:
                extra = " (variant speaker %d)" % info["speaker"] if info["variant"] else ""
                out("  %s%s : %s" % (lg.upper(), extra, esc(t)))


# --------------------------------------------------------------------------- coverage
def room_refs(arc, name):
    """E0/E3/E4 message references: (reachable set, dead-code set, out-of-range list is up to the caller)."""
    sc = fs.Script(arc.script_blob(name), name)
    reach, dead = set(), set()
    for src, dst in ((sc.insns, reach), (sc.dead, dead)):
        for ins in src.values():
            if ins.op == 0xE0:
                dst.add(ins.ops[0][1])
            elif ins.op in (0xE3, 0xE4):
                dst.add(ins.ops[1][1])
    return reach, dead, sc


def cmd_coverage(a):
    ids, names = load_room_ids(a.roomids)
    arc = fs.Archive(os.path.join(DATA, "cdrom.dat"))
    lang = Lang()
    lg = a.lang
    tot = collections.Counter()
    misses = []
    err_rows = []
    per_room = []
    cache = {}
    for rid in sorted(ids):
        nm = ids[rid]
        if nm not in cache:
            try:
                reach, dead, sc = room_refs(arc, nm)
            except Exception as ex:                                       # noqa
                out("; room %s: script decode failed: %s" % (nm, ex))
                cache[nm] = (set(), set(), 0)
                continue
            nmsg = len(fs.Dialogue(arc.dialogue_blob(nm)).msgs)
            cache[nm] = (reach, dead, nmsg)
        reach, dead, nmsg = cache[nm]
        tot["rooms"] += 1
        before = (tot["reach_ok"], tot["reach_error"], tot["reach_miss"], tot["reach_refs"])
        for kind, S in (("reach", reach), ("dead", dead - reach)):
            for m in sorted(S):
                tot[kind + "_refs"] += 1
                if m >= nmsg:
                    tot[kind + "_oob"] += 1
                    misses.append((rid, nm, m, kind, "index >= block size %d" % nmsg))
                    continue
                t = lang.main_text(lg, rid, m)
                if t is None:
                    tot[kind + "_miss"] += 1
                    misses.append((rid, nm, m, kind, "no row"))
                elif t.strip() == "{ERROR}":
                    tot[kind + "_error"] += 1
                    err_rows.append((rid, nm, m, kind))
                else:
                    tot[kind + "_ok"] += 1
        per_room.append((rid, nm, nmsg) + tuple(tot[k] - b for k, b in zip(
            ("reach_refs", "reach_ok", "reach_error", "reach_miss"), (before[3], before[0], before[1], before[2]))))
        # block messages never referenced by E0/E3/E4 (called through other mechanisms / unused)
        tot["msgs"] += nmsg
        tot["msgs_unref"] += len([i for i in range(nmsg) if i not in reach and i not in dead])
    out("; coverage for lang=%s over %d rooms (ids)" % (lg, tot["rooms"]))
    out("; reachable E0/E3/E4 refs : %d  translated %d  {ERROR} %d  missing %d  out-of-block %d" % (
        tot["reach_refs"], tot["reach_ok"], tot["reach_error"], tot["reach_miss"], tot["reach_oob"]))
    out("; dead-code-only refs     : %d  translated %d  {ERROR} %d  missing %d  out-of-block %d" % (
        tot["dead_refs"], tot["dead_ok"], tot["dead_error"], tot["dead_miss"], tot["dead_oob"]))
    out("; messages in blocks      : %d, never referenced by E0/E3/E4: %d" % (tot["msgs"], tot["msgs_unref"]))
    if a.rooms:
        out("; per room (reachable refs only): id name msgs refs translated error missing")
        for r in per_room:
            out("ROOM %4d %-12s %4d %4d %4d %4d %4d" % r)
    shown = 0
    for rid, nm, m, kind, why in misses:
        if a.all or shown < a.limit:
            out("MISS room=%d (%s) msg=%d [%s] %s" % (rid, nm, m, kind, why))
            shown += 1
    if len(misses) > shown:
        out("; ... %d more misses (use --all)" % (len(misses) - shown))
    if a.errors:
        for rid, nm, m, kind in err_rows:
            out("ERROR-ROW room=%d (%s) msg=%d [%s]" % (rid, nm, m, kind))
    else:
        out("; {ERROR} placeholder refs: %d (use --errors to list)" % len(err_rows))


# --------------------------------------------------------------------------- langs / tables
def parse_tsv(text):
    """Excel 'Unicode text' TSV: TAB separates cells unless quoted; "" is a quote; CR/LF in quotes -> LF."""
    rows, cells, cur = [], [], []
    inq = False
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if inq:
            if ch == '"':
                if i + 1 < n and text[i + 1] == '"':
                    cur.append('"')
                    i += 1
                else:
                    inq = False
            elif ch == "\r":
                if i + 1 < n and text[i + 1] == "\n":
                    i += 1
                cur.append("\n")
            else:
                cur.append(ch)
        elif ch == '"' and not cur:
            inq = True
        elif ch == "\t":
            cells.append("".join(cur))
            cur = []
        elif ch in "\r\n":
            if ch == "\r" and i + 1 < n and text[i + 1] == "\n":
                i += 1
            cells.append("".join(cur))
            cur = []
            if any(cells):
                rows.append(cells)
            cells = []
        else:
            cur.append(ch)
        i += 1
    if cur or cells:
        cells.append("".join(cur))
        rows.append(cells)
    return rows


def cmd_langs(a):
    lang = Lang()
    out("%-34s %9s %8s  %s" % ("member", "bytes", "rows", "notes"))
    for zi in lang.z.infolist():
        nm = zi.filename
        if nm.endswith("/"):
            continue
        rows, note = "", ""
        if nm.endswith(".csv"):
            data = lang.read(nm).decode("utf-8-sig")
            lines = [l for l in data.split("\n") if l.strip()]
            rows = str(len(lines))
            if re.fullmatch(r"(en|fr|it|de|es|jp)\.csv", nm):
                note = "field messages room<TAB>msg<TAB>text"
            elif nm.startswith("Ex_"):
                note = "per-speaker variants (v1..v43)"
            elif nm.startswith("btel_"):
                note = "battle event messages battle<TAB>msg<TAB>text"
            elif nm.startswith("mesgen"):
                note = "dialect/grammar substitution keys"
            elif nm == "mess_wend.csv":
                note = "auto-close overrides (non-JP)"
        elif nm.endswith(".txt"):
            raw = lang.read(nm)
            t = parse_tsv(decode_text(raw))
            rows = str(max(0, len(t) - 1))
            note = "language-column table: " + " ".join(t[0][:8]) if t else ""
        elif nm.endswith(".pac"):
            note = "PS1 battle event pack (timing patch)"
        out("%-34s %9d %8s  %s" % (nm, zi.file_size, rows, note))


SYS_CATS = {0x00: "item/equip", 0x01: "char name", 0x17: "element/tech", 0x18: "key item", 0x19: "enemy",
            0x1A: "battle misc", 0x1B: "misc"}


def cmd_sysmes(a):
    lang = Lang()
    name = a.table + ".txt"
    t = parse_tsv(decode_text(lang.read(name)))
    hdr = t[0]
    col = None
    for i in range(1, len(hdr)):
        if hdr[i].strip().lower() == a.lang:
            col = i
    if col is None:
        raise SystemExit("column %r not in header %r" % (a.lang, hdr))
    out("; %s lang=%s column=%d header=%s rows=%d" % (name, a.lang, col, "|".join(hdr), len(t) - 1))
    for r in t[1:]:
        if len(r) <= col:
            continue
        key = r[0]
        label = ""
        if a.table == "sysmes":
            try:
                k = int(key, 0)
            except ValueError:
                continue
            cat = k >> 16
            if a.cat is not None and cat != a.cat:
                continue
            label = "cat %02X idx %-4d %s" % (cat, k & 0xFFFF, SYS_CATS.get(cat, "?"))
        out("%-10s %-34s %s" % (key, label, esc(r[col])))


# --------------------------------------------------------------------------- spritefonts
def parse_spritefont(f):
    """DirectXTK MakeSpriteFont binary (layout is public):
         char[8] "DXTKfont"; u32 glyphCount;
         glyph[count] { u32 character; i32 left, top, right, bottom; f32 xOffset, yOffset, xAdvance }  (32 bytes)
         f32 lineSpacing; u32 defaultCharacter;
         u32 texWidth, texHeight, dxgiFormat, stride, rows; u8 pixels[stride*rows]
       'f' is a file-like object positioned at 0; only the header is read."""
    h = f.read(12)
    if h[:8] != b"DXTKfont":
        raise ValueError("not a DXTKfont file")
    cnt = struct.unpack_from("<I", h, 8)[0]
    body = f.read(cnt * 32 + 28)
    chars, minc, maxc = [], 0xFFFFFFFF, 0
    for i in range(cnt):
        c, l, t, r, b, xo, yo, xa = struct.unpack_from("<Iiiiifff", body, i * 32)
        chars.append(c)
        minc, maxc = min(minc, c), max(maxc, c)
    ls, dflt, tw, th, fmt, stride, rows = struct.unpack_from("<fIIIIII", body, cnt * 32)
    return {"glyphs": cnt, "min": minc, "max": maxc, "line": ls, "default": dflt, "w": tw, "h": th,
            "fmt": fmt, "stride": stride, "rows": rows, "pixels": stride * rows, "hdr": 12 + cnt * 32 + 28,
            "sorted": chars == sorted(chars)}


def cmd_font(a):
    out("%-36s %9s %6s %-15s %9s %7s %-11s %s" % ("file", "bytes", "glyph", "range", "lineSp", "dflt", "tex WxH", "fmt/stride/rows"))
    for arcname in ("font.dat", "ui.dat"):
        p = os.path.join(DATA, arcname)
        if not os.path.exists(p):
            continue
        z = zipfile.ZipFile(p)
        for zi in z.infolist():
            if not zi.filename.endswith(".spritefont"):
                continue
            with z.open(zi) as f:
                try:
                    m = parse_spritefont(f)
                except Exception as ex:                               # noqa
                    out("%-36s %9d  parse error: %s" % (zi.filename, zi.file_size, ex))
                    continue
            ok = "" if m["hdr"] + m["pixels"] == zi.file_size else "  SIZE MISMATCH hdr+pix=%d" % (m["hdr"] + m["pixels"])
            out("%-36s %9d %6d U+%04X..U+%04X %9.4f %7d %5dx%-5d %d/%d/%d%s%s" % (
                arcname.split(".")[0] + ":" + zi.filename[:33], zi.file_size, m["glyphs"], m["min"], m["max"], m["line"],
                m["default"], m["w"], m["h"], m["fmt"], m["stride"], m["rows"], "" if m["sorted"] else " UNSORTED", ok))


# --------------------------------------------------------------------------- JP glyph table
def all_messages(arc, ids, names):
    """yield (room id, msg idx, bytes) for every (id,msg) of every room."""
    for nm in sorted(names):
        d = fs.Dialogue(arc.dialogue_blob(nm))
        for rid in names[nm]:
            for (i, ptr, w1, w2, w3, raw) in d.msgs:
                yield rid, i, raw


def byte_units(raw):
    u, i, n = [], 0, len(raw)
    while i < n:
        c = raw[i]
        if c >= PREFIX_MIN and i + 1 < n:
            u.append(("g", bytes(raw[i:i + 2])))
            i += 2
        elif c >= 0x20:
            u.append(("g", bytes(raw[i:i + 1])))
            i += 1
        else:
            na = CTRL.get(c, (0, 0))[1]
            u.append(("c", bytes(raw[i:i + 1 + na])))
            i += 1 + na
    return u


def text_units(t):
    u = []
    for m in re.finditer(r"\{[^}]*\}|\n|.", t.replace(BS + "n", "\n"), re.S):
        s = m.group()
        if s == "\n":
            u.append(("c", s))
        elif s[0] == "{" and len(s) > 1:
            u.append(("c", s))
        else:
            u.append(("g", s))
    return u


def derive_glyphs(arc, lang, ids, names):
    """Align jp.csv (RDE re-authored text) with the disc bytes message by message; where unit counts and
    control positions agree, every glyph unit pairs with one character.  Returns (counts, stats, usage)."""
    jp = lang.table("jp.csv", "main")
    stat = collections.Counter()
    votes = collections.defaultdict(collections.Counter)
    usage = collections.Counter()
    for rid, i, raw in all_messages(arc, ids, names):
        for u in byte_units(raw):
            if u[0] == "g":
                usage[u[1]] += 1
        t = jp.get((rid, i))
        if t is None:
            stat["no jp row"] += 1
            continue
        bu, tu = byte_units(raw), text_units(t)
        while tu and tu[-1] == ("c", "\n"):
            tu.pop()
        if tu and tu[-1][1] == "{mend}":
            tu.pop()
        while bu and bu[-1][0] == "c" and bu[-1][1][0] in (0, 1):
            bu.pop()
        if len(bu) != len(tu):
            stat["unit count differs (RDE edit)"] += 1
            continue
        if any(x[0] != y[0] for x, y in zip(bu, tu)):
            stat["control/glyph kind differs"] += 1
            continue
        stat["aligned"] += 1
        for x, y in zip(bu, tu):
            if x[0] == "g":
                votes[x[1]][y[1]] += 1
    return votes, stat, usage


def cmd_glyphs(a):
    ids, names = load_room_ids(a.roomids)
    arc = fs.Archive(os.path.join(DATA, "cdrom.dat"))
    lang = Lang()
    votes, stat, usage = derive_glyphs(arc, lang, ids, names)
    table, low = {}, 0
    lines = ["# JP glyph table: disc glyph bytes -> character, derived by aligning lang.dat jp.csv with the disc message bytes",
             "# columns: bytes(hex)\tbitmap_index\tchar\tvotes\tshare\trunner_up",
             "# conf: share>=0.9 and votes>=3 is solid; the rest are listed anyway (low=%s)" % "see column 7",
             "# bitmap_index = (b0-0x20) for b0<0xF8, else (b0-0xF7)*256+(b1-0x20): row in the exe bitmap table"]
    for g in sorted(votes, key=lambda b: (len(b), b)):
        ch, n = votes[g].most_common(1)[0]
        tot = sum(votes[g].values())
        share = n / tot
        runner = votes[g].most_common(2)[1] if len(votes[g]) > 1 else ("", 0)
        conf = "ok" if share >= 0.9 and n >= 3 else "low"
        low += conf == "low"
        table[g] = ch
        lines.append("%s\t%d\t%s\t%d\t%.3f\t%s\t%s" % (g.hex(), glyph_index(g), ch, n, share,
                                                          runner[0] + ("x%d" % runner[1] if runner[1] else ""), conf))
    unresolved = sorted(g for g in usage if g not in table)
    for g in unresolved:
        lines.append("%s\t%d\t\t0\t0.000\t\tunresolved(used %d)" % (g.hex(), glyph_index(g), usage[g]))
    # validation: decode every message with the table and compare with jp.csv
    jp = lang.table("jp.csv", "main")
    exact = tot_cmp = 0
    for rid, i, raw in all_messages(arc, ids, names):
        t = jp.get((rid, i))
        if t is None:
            continue
        tot_cmp += 1
        if jp_stream(raw, table) == t:
            exact += 1
    out("messages aligned: %s" % dict(stat))
    out("distinct glyphs on disc: %d, resolved: %d (low confidence %d), unresolved: %d (uses %d)" % (
        len(usage), len(table), low, len(unresolved), sum(usage[g] for g in unresolved)))
    out("round trip: %d of %d messages decode byte-for-byte to the jp.csv text (rest: RDE edits, {mend}/{wend} variants)" % (exact, tot_cmp))
    if a.write:
        with open(GLYPHS, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        out("wrote %s" % GLYPHS)


# --------------------------------------------------------------------------- font bitmaps / PNG
FONT_VA = 0x8005670C       # glyph bitmap table; 22 bytes per glyph = 11 rows x u16 LE, bit15 = leftmost pixel, 11 px


class ExeFont:
    def __init__(self):
        z = zipfile.ZipFile(os.path.join(DATA, "cdrom.dat"))
        self.exe = z.read("prog/slps_023.64")
        load = struct.unpack_from("<I", self.exe, 0x18)[0]
        self.base = FONT_VA - load + 0x800
        self.count = (len(self.exe) - self.base) // 22

    def glyph(self, idx):
        o = self.base + idx * 22
        return [struct.unpack_from("<H", self.exe, o + 2 * r)[0] for r in range(11)]


def write_png(path, w, h, pix):
    """8-bit grayscale PNG from a bytearray of w*h."""
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(pix[y * w:(y + 1) * w]) for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Canvas:
    def __init__(self, w, h, scale):
        self.w, self.h, self.s = w, h, scale
        self.pix = bytearray(b"\xff" * (w * scale * h * scale))

    def glyph(self, rows, x, y):
        s, W = self.s, self.w * self.s
        for r, v in enumerate(rows):
            for c in range(11):
                if v & (0x8000 >> c):
                    for dy in range(s):
                        base = ((y + r) * s + dy) * W + (x + c) * s
                        for dx in range(s):
                            if 0 <= base + dx < len(self.pix):
                                self.pix[base + dx] = 0

    def save(self, path):
        write_png(path, self.w * self.s, self.h * self.s, self.pix)


def cmd_render(a):
    ids, names = load_room_ids(a.roomids)
    name, rids = resolve_room_arg(a.room, ids, names)
    arc = fs.Archive(os.path.join(DATA, "cdrom.dat"))
    d = fs.Dialogue(arc.dialogue_blob(name))
    raw = d.msgs[a.msg][5]
    font = ExeFont()
    gl = load_glyph_table()
    # layout: advance 8 px when the first glyph byte <= 0x74, else 12 px (renderer); lines of 14 px
    items, x, y, w, pages = [], 2, 2, 8, 0
    i, n = 0, len(raw)
    used = []
    while i < n:
        c = raw[i]
        if c >= 0x20:
            g = raw[i:i + 2] if c >= PREFIX_MIN else raw[i:i + 1]
            i += len(g)
            adv = 8 if c <= 0x74 else 12
            items.append((glyph_index(g), x, y))
            used.append(g)
            x += adv
            w = max(w, x + 2)
        else:
            na = CTRL.get(c, (0, 0))[1]
            i += 1 + na
            if c == 0x01:
                x, y = 2, y + 14
            elif c in (0x02, 0x03):
                x, y = 2, y + 20
    h = y + 16
    cv = Canvas(w, h, a.scale)
    for idx, gx, gy in items:
        cv.glyph(font.glyph(idx), gx, gy)
    cv.save(a.out)
    out("room %s msg %d: %d glyphs -> %s (%dx%d px, scale %d)" % (name, a.msg, len(items), a.out, w, h, a.scale))
    out("JP: " + jp_stream(raw, gl))


def cmd_fontsheet(a):
    font = ExeFont()
    first, count = a.first, a.count
    cols = 32
    rows = (count + cols - 1) // cols
    cv = Canvas(cols * 13 + 2, rows * 13 + 2, a.scale)
    for k in range(count):
        cv.glyph(font.glyph(first + k), 2 + (k % cols) * 13, 2 + (k // cols) * 13)
    cv.save(a.out)
    out("glyph indices %d..%d -> %s" % (first, first + count - 1, a.out))


# --------------------------------------------------------------------------- main
def main(argv=None):
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(prog="cc_text", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--roomids", help="room id csv (room,file,...) [default tools/py/data/text_room_ids.csv]")
    sp = ap.add_subparsers(dest="cmd", required=True)
    sp.add_parser("langs").set_defaults(fn=cmd_langs)
    p = sp.add_parser("room")
    p.add_argument("room")
    p.add_argument("lang", nargs="?", default="en", choices=LANGS)
    p.set_defaults(fn=cmd_room)
    p = sp.add_parser("coverage")
    p.add_argument("lang", nargs="?", default="en", choices=LANGS)
    p.add_argument("--all", action="store_true")
    p.add_argument("--limit", type=int, default=40)
    p.add_argument("--errors", action="store_true")
    p.add_argument("--rooms", action="store_true", help="per-room table")
    p.set_defaults(fn=cmd_coverage)
    p = sp.add_parser("sysmes")
    p.add_argument("lang", nargs="?", default="en", choices=LANGS)
    p.add_argument("--table", default="sysmes")
    p.add_argument("--cat", type=lambda s: int(s, 0))
    p.set_defaults(fn=cmd_sysmes)
    sp.add_parser("font").set_defaults(fn=cmd_font)
    p = sp.add_parser("glyphs")
    p.add_argument("--write", action="store_true")
    p.set_defaults(fn=cmd_glyphs)
    p = sp.add_parser("render")
    p.add_argument("room")
    p.add_argument("msg", type=int)
    p.add_argument("out")
    p.add_argument("--scale", type=int, default=4)
    p.set_defaults(fn=cmd_render)
    p = sp.add_parser("fontsheet")
    p.add_argument("out")
    p.add_argument("--first", type=int, default=0)
    p.add_argument("--count", type=int, default=512)
    p.add_argument("--scale", type=int, default=3)
    p.set_defaults(fn=cmd_fontsheet)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
