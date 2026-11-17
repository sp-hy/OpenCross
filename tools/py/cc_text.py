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
  usa-glyphs [--write]        USA glyph table (field font, menu sheet, widths) checked against en.csv
  usa-room <name> [--en]      every message of a room from the USA disc, in the en.csv notation
  usa-tables [--cat N --verbose --write]   USA executable string tables, dialect tables, table map
  usa-menu [--verbose --write]             menutest / shopmenu keys -> USA menu messages
  usa-compare                 match rate of the USA disc text against en.csv, btel_en.csv, sysmes.txt
  usa-fontsheet <out.png>     render the USA field glyph bitmaps as a grid

Sources: data/cdrom.dat (rooms, prog/slps_023.64), data/lang.dat, data/font.dat, data/ui.dat; the usa-* commands
read the USA PS1 disc image (--disc, --disc2) and the record names in src/vfs/disc_names.inc.
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


EXE_STRINGS = 0x508E0                                  # string tables in prog/slps_023.64
EXE_TABLE_CATEGORY = {0: 0x00, 1: 0x01, 23: 0x17, 24: 0x18, 25: 0x19, 26: 0x1A}
BATTLE_SCRIPT_RECORD = 2679                            # cdrom.fnd record of battle scene 0's script


def offset_strings(b, start=0):
    """u16 offset table (the first offset = 2 * entry count, the last entry is the end) -> NUL-terminated byte strings."""
    count = struct.unpack_from("<H", b, start)[0] // 2
    res = []
    for i in range(count - 1):
        at = start + struct.unpack_from("<H", b, start + 2 * i)[0]
        end = at
        while end < len(b) and b[end] != 0:
            code = b[end]
            end += 2 if code >= PREFIX_MIN or code in (4, 5, 8, 0xE, 0xF, 0x16, 0x17) else 1
        res.append(b[at:end])
    return res


def record_strings(b):
    """Message-block layout (8-byte records, u16 text offset first) -> one byte string per record."""
    if len(b) < 8:
        return []
    ptrs = [struct.unpack_from("<H", b, 8 * i)[0] for i in range(struct.unpack_from("<H", b, 0)[0] // 8)]
    return [b[at:min([nxt for nxt in ptrs + [len(b)] if nxt > at], default=len(b))] for at in ptrs]


def extra_text_pairs(z, lang):
    """(disc bytes, RDE text) pairs from the tables the dialogue does not cover: the executable's string tables against
    sysmes, battle event packs against btel_jp, btmes.dat, keyitem.dat and waa_spec.dat against their lang.dat tables."""
    def jp_rows(name, key_col=0):
        table = parse_tsv(decode_text(lang.read(name)))
        hdr = [h.strip() for h in table[0]]
        col = hdr.index("JP")
        return {r[key_col]: (r[col] if len(r) > col else "") for r in table[1:]}

    exe = z.read("prog/slps_023.64")
    count = struct.unpack_from("<I", exe, EXE_STRINGS)[0]
    offs = struct.unpack_from("<%dI" % count, exe, EXE_STRINGS + 4)
    sysmes = {int(k, 0): v for k, v in jp_rows("sysmes.txt").items() if k.startswith("0x")}
    for table, cat in EXE_TABLE_CATEGORY.items():
        for i, raw in enumerate(offset_strings(exe, EXE_STRINGS + offs[table])):
            text = sysmes.get((cat << 16) | i)
            if text is None:
                continue
            if cat == 0x1A and i % 3:
                lines = sysmes.get((cat << 16) | (i - i % 3 + 1), "").split("\n")
                text = lines[i % 3 - 1] if len(lines) > i % 3 - 1 else ""
            yield raw, text
    fnd = z.read("cdrom.fnd")
    btel = {}
    for line in lang.read("btel_jp.csv").decode("utf-8-sig").split("\n"):
        cells = line.rstrip("\r").split("\t", 2)
        if len(cells) == 3 and cells[0].isdigit() and cells[1].isdigit():
            btel[(int(cells[0]), int(cells[1]))] = cells[2]
    for bid in sorted({k[0] for k in btel}):
        rec = fnd[(BATTLE_SCRIPT_RECORD + bid) * 0x40:][:0x40].split(b"\0")[0].decode("latin-1").lower().replace(chr(92), "/")
        at = rec.find("/battle/event/")
        if at < 0:
            continue
        pac = z.read(rec[at + 1:])
        end0, eof = struct.unpack_from("<2I", pac, 8)
        for msg, raw in enumerate(record_strings(pac[end0:eof])):
            if (bid, msg) in btel:
                yield raw, btel[(bid, msg)]
    rows = jp_rows("btmes.txt")
    for i, raw in enumerate(record_strings(z.read("battle/data/btmes.dat"))):
        if str(i) in rows:
            yield raw.lstrip(b"\x75"), rows[str(i)]
    for name, table in (("menu/keyitem.dat", "keyitem.txt"), ("menu/waa_spec.dat", "waa_spec.txt")):
        rows = jp_rows(table)
        for i, raw in enumerate(offset_strings(z.read(name))):
            lines = rows.get(str(i - i % 3), "").split("\n")
            if len(lines) > i % 3:
                yield raw, lines[i % 3]


def derive_extra_glyphs(votes, lang):
    """Votes for the glyph codes the dialogue never uses, from extra_text_pairs: the glyph units of a pair are matched one to
    one when both sides have the same number of glyphs."""
    known = set(votes)
    z = zipfile.ZipFile(os.path.join(DATA, "cdrom.dat"))
    for raw, text in extra_text_pairs(z, lang):
        bg = [u[1] for u in byte_units(raw) if u[0] == "g"]
        tg = [u[1] for u in text_units(re.sub(r"^\{(wmode|size)[^}]*\}", "", text)) if u[0] == "g"]
        if len(bg) != len(tg) or not any(g not in known for g in bg):
            continue
        for g, ch in zip(bg, tg):
            if g not in known:
                votes[g][ch] += 1


def cmd_glyphs(a):
    ids, names = load_room_ids(a.roomids)
    arc = fs.Archive(os.path.join(DATA, "cdrom.dat"))
    lang = Lang()
    votes, stat, usage = derive_glyphs(arc, lang, ids, names)
    derive_extra_glyphs(votes, lang)
    table, low = {}, 0
    lines = ["# JP glyph table: disc glyph bytes -> character, derived by aligning lang.dat jp.csv with the disc message bytes"
             " (codes the dialogue never uses: the executable, battle and menu tables against their lang.dat tables)",
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


# --------------------------------------------------------------------------- USA build (read from a PS1 disc image)
DISC_NAMES = os.path.join(OC, "src", "vfs", "disc_names.inc")
USA_GLYPHS = os.path.join(HERE, "data", "usa_glyphs.tsv")
USA_TABLES = os.path.join(HERE, "data", "usa_tables.tsv")
USA_MENU_STRINGS = os.path.join(HERE, "data", "usa_menu_strings.tsv")
USA_FONT_VA = 0x800567D0                 # field glyph bitmaps in the executable: glyph i = code 0x20 + i, 22 bytes each
USA_TEXT_VA = 0x8005783C                 # string-table container in the executable
USA_FIELD_BASE = 0x800A3308              # load address of prog/field.prg
USA_COLOUR_VA = 0x800E1D50               # field text colours, 3 bytes (r, g, b) each
USA_DIALECT_COLUMN_VA = 0x800E1D8C       # field overlay: character id -> dialect column
USA_WIDTH_RECORD = 2059                  # u8[256] pen advance per code - 0x20
USA_DIALECT_TABLE = 6                    # ACCENT n reads string table n + 6
USA_NAME_FORMS = 0x20                    # ACCENT 0x20 is indexed by character id + 1, not by dialect column
USA_CTRL_ARGS = {0x04: 1, 0x05: 1, 0x06: 1, 0x07: 2, 0x08: 1, 0x0A: 1, 0x0B: 1, 0x0E: 1, 0x10: 1, 0x12: 1, 0x15: 1,
                 0x16: 1, 0x17: 1, 0x18: 1, 0x19: 1, 0x1B: 1, 0x1C: 1, 0x1E: 1}
USA_TABLE_KINDS = {0: ("items", 0x00, 0), 1: ("character names", 0x01, 1), 2: ("battle strings and descriptions", 0x1A, 26),
                   3: ("button names", 0x1B, 27), 4: ("elements and techs", 0x17, 23), 5: ("key items", 0x18, 24),
                   6: ("enemy names", 0x19, 25)}
USA_CASE_MARKUP = {0: "{Cap}", 1: "{CAP}", 2: "{CapL}"}
USA_SUFFIX_MARKUP = {0: "{-y}", 1: "{-i}"}
USA_BUTTON_MARKUP = {0x10: "up", 0x11: "down", 0x12: "right", 0x13: "left"}
USA_COLOUR_MARKUP = {8: "GRAY"}

# Field font, codes 0x20..0xDE, identified from the executable's glyph bitmaps (votes from en.csv confirm the Latin part).
USA_FIELD_CHARS = (
    " ()-,+./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "=!\"$%&*:?–[]｡'#" + chr(0x3000) + "←↑↓→＋（）．“”"
    "αβγδÀÄÇÈËÒÖÙÜ"
    "àâáäçèêéëìîíïùûúü"
    "òóôöÑñ♪♫Ï¡MWmw" + chr(0xE0AC) + chr(0x2009) * 7 +
    "『』×&☆□△※<>~…?!・「」#−/、―‥＿○’:-0123456789%,;<>")
# Menu sheet (usfo frames): where it differs from the field font.
USA_MENU_CHARS = dict(zip(range(0x85, 0xAD), "ÀÄÇÈËÒÖÙÜàáâ"
                          "äçèéêëìíîïùúûüò"
                          "óôöÑñÌÍÎÏŒœ¡¿"))
USA_MENU_CHARS.update(zip(range(0xB6, 0xC0), "×&★□△※<>~○"))
USA_MENU_CHARS[0x6F] = "_"
USA_FIELD_NOTES = {0x75: "choice indent blank", 0x80: "item-kind prefix", 0x81: "item-kind prefix", 0x82: "item-kind prefix",
                   0x83: "item-kind prefix", 0xAC: "boxed E icon", 0xA8: "alternate M", 0xA9: "alternate W",
                   0xAA: "alternate m", 0xAB: "alternate w", 0xD0: "digits of the NUM code 0A", 0xDB: "the comma the text uses"}
# Menu text ('mess' entries) is ASCII; these two-byte Shift-JIS symbols are mapped to sheet codes.
USA_MENU_SJIS = {b"\x81\x67": 0x7E, b"\x81\x68": 0x7F, b"\x81\x9b": 0xBF, b"\x81\x7e": 0xB6, b"\x82\x77": 0xB6}


def usa_find_disc(number):
    """First USA cue sheet for disc `number` in the DuckStation game folders."""
    for folder in (os.path.join(OC, "tools", "duckstation", "games"),
                   os.path.join(os.environ.get("USERPROFILE", ""), "Documents", "DuckStation", "games")):
        if not os.path.isdir(folder):
            continue
        for name in sorted(os.listdir(folder)):
            if "(USA)" in name and ("Disc %d" % number) in name and name.lower().endswith(".cue"):
                return os.path.join(folder, name)
    return None


def disc_names(number):
    """Record names of disc 1 or 2 from the engine's generated name list."""
    with open(DISC_NAMES, encoding="utf-8") as handle:
        text = handle.read()
    first = text[text.index("kDisc1[]"):]
    names = re.findall(r'^"(.*)",$', first[:first.index("};")], re.M)
    if number == 2:
        for index, name in re.findall(r'^\{(\d+), "(.*)"\},$', first[first.index("kDisc2Changes"):], re.M):
            names[int(index)] = name
    return names


class DiscImage:
    """A PS1 disc image (cue sheet, raw 2352-byte image or 2048-byte iso) and the game's own file table."""

    def __init__(self, path):
        if path.lower().endswith(".cue"):
            with open(path, encoding="latin-1") as handle:
                ref = re.search(r'FILE\s+"?([^"\r\n]+?)"?\s+BINARY', handle.read()).group(1)
            path = os.path.join(os.path.dirname(path), ref)
        self.handle = open(path, "rb")
        head = self.handle.read(16)
        self.stride = 2352 if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00" else 2048
        self.data_at = (24 if head[15] == 2 else 16) if self.stride == 2352 else 0
        table = self.read_user(24, 12)
        self.words = struct.unpack("<%dI" % (len(table) // 4), table)
        self.number = struct.unpack_from("<I", self.read_user(37, 1), 0)[0]
        self.names = disc_names(self.number)

    def read_user(self, lba, count):
        out = []
        for i in range(count):
            self.handle.seek((lba + i) * self.stride + self.data_at)
            out.append(self.handle.read(2048))
        return b"".join(out)

    def record(self, index):
        lba = self.words[index] & 0x7FFFFF
        size = ((self.words[index + 1] & 0x7FFFFF) - lba) * 2048 - (self.words[index] >> 24) * 8
        if self.words[index] & 0x800000 or size <= 0:
            return b""
        return self.read_user(lba, (size + 2047) // 2048)[:size]

    def find(self, name):
        name = name.lower()
        for i, rec in enumerate(self.names):
            if rec.lower() == name:
                return i
        return None

    def file(self, name):
        i = self.find(name)
        return None if i is None else self.record(i)


class UsaBuild:
    """The USA build's text data: executable, field overlay, width table, menu art, room messages."""

    def __init__(self, disc1=None, disc2=None):
        path = disc1 or usa_find_disc(1)
        if not path:
            raise SystemExit("no USA disc 1 image found; pass --disc <cue|bin|iso>")
        self.discs = [DiscImage(path)]
        second = disc2 or usa_find_disc(2)
        if second:
            self.discs.append(DiscImage(second))
        self.exe = self.discs[0].record(0)
        if self.exe[:8] != b"PS-X EXE" or len(self.exe) != 606208:
            raise SystemExit("%s is not the USA build (executable %d bytes)" % (path, len(self.exe)))
        self.load = struct.unpack_from("<I", self.exe, 0x18)[0]
        self.field = self.discs[0].file("prog/field.prg")
        self.widths = self.discs[0].record(USA_WIDTH_RECORD)
        self.menutest = self.discs[0].file("menu/menutest.prd")

    def exe_at(self, va):
        return va - self.load + 0x800

    def file(self, name):
        for disc in self.discs:
            b = disc.file(name)
            if b:
                return b
        return None

    def room_messages(self, room):
        """Raw bytes of every message of a room (subfile 8 of map/mapbin/<room>.bin)."""
        b = self.file("map/mapbin/%s.bin" % room)
        if not b:
            return None
        num = struct.unpack_from("<I", b, 0)[0]
        offs = struct.unpack_from("<%dI" % (num + 1), b, 4)
        sub = b[offs[8]:offs[9]] if num > 8 else b""
        return [msg[5] for msg in fs.Dialogue(fs.lzss_decode(sub) if sub[:4] == b"sszl" else b"").msgs]

    def rooms(self):
        seen = []
        for disc in self.discs:
            for rec in disc.names:
                rec = rec.lower()
                if rec.startswith("map/mapbin/") and rec.endswith(".bin") and rec[11:-4] not in seen:
                    seen.append(rec[11:-4])
        return seen

    def battle_packs(self):
        """(scene number, message list) of every battle/event/evtNNN.pac."""
        seen = set()
        for disc in self.discs:
            for i, rec in enumerate(disc.names):
                match = re.match(r"battle/event/evt(\d+)\.pac$", rec.lower())
                if not match or rec.lower() in seen:
                    continue
                seen.add(rec.lower())
                pac = disc.record(i)
                end0, eof = struct.unpack_from("<2I", pac, 8)
                yield int(match.group(1)), [msg[5] for msg in fs.Dialogue(pac[end0:eof]).msgs]

    def glyph(self, code):
        at = self.exe_at(USA_FONT_VA) + (code - 0x20) * 22
        return list(struct.unpack_from("<11H", self.exe, at))

    def tables(self):
        """The 55 string tables: list of byte-string lists (a table's last offset only ends the previous string)."""
        at = self.exe_at(USA_TEXT_VA)
        count = struct.unpack_from("<I", self.exe, at)[0]
        offs = struct.unpack_from("<%dI" % count, self.exe, at + 4)
        return [usa_offset_strings(self.exe, at + rel) for rel in offs]

    def dialect_columns(self, count=50):
        at = USA_DIALECT_COLUMN_VA - USA_FIELD_BASE
        return list(self.field[at:at + count])

    def colours(self, count=9):
        at = USA_COLOUR_VA - USA_FIELD_BASE
        return [tuple(self.field[at + 3 * i:at + 3 * i + 3]) for i in range(count)]

    def menu_frames(self):
        """usfo sheet frames of menutest.prd: code -> (u, v, w, h, x, y, advance); codes without a frame are absent."""
        sheet = drp_entries(self.menutest).get(("usfo", 3))
        out = {}
        if not sheet:
            return out
        count = struct.unpack_from("<H", sheet, 2)[0] & 0x7FFF
        for num in range(1, count + 1):
            base = struct.unpack_from("<H", sheet, 0xC + 2 * (num - 1))[0]
            if base + 16 > len(sheet) or struct.unpack_from("<H", sheet, base)[0] == 0:
                continue
            x, y = struct.unpack_from("<2h", sheet, base + 4)
            u, v, w, h = sheet[base + 8], sheet[base + 9], sheet[base + 12], sheet[base + 13]
            out[0x20 + num] = (u, v, w, h, x, y, x + w + 1)
        return out

    def menu_messages(self):
        mess = drp_entries(self.menutest).get(("mess", 7), b"")
        return [usa_menu_text(text) for text in usa_offset_strings(mess, 0, plain=True)] if mess else []


def drp_entries(b):
    """DRP pack -> {(name, type): payload}."""
    out = {}
    if not b or b[:4] != b"drp\0":
        return out
    count = struct.unpack_from("<H", b, 8)[0] // 64
    for entry_at in struct.unpack_from("<%dI" % count, b, 12):
        if entry_at == 0 or entry_at + 12 > len(b):
            continue
        name = b[entry_at + 4:entry_at + 8].rstrip(b"\0").decode("latin-1")
        size = int.from_bytes(b[entry_at + 9:entry_at + 12], "little") // 16
        out[(name, b[entry_at + 8])] = b[entry_at + 12:entry_at + 12 + size]
    return out


def usa_offset_strings(b, start, plain=False):
    """u16 offset table (first offset = 2 * entry count) -> byte strings up to the NUL; the last offset only ends the
    previous string.  Control codes skip their argument bytes unless `plain` (ASCII menu text)."""
    num = struct.unpack_from("<H", b, start)[0] // 2
    res = []
    for i in range(num - 1):
        at = end = start + struct.unpack_from("<H", b, start + 2 * i)[0]
        while end < len(b) and b[end] != 0:
            end += 1 if plain or b[end] >= 0x20 else 1 + USA_CTRL_ARGS.get(b[end], 0)
        res.append(b[at:end])
    return res


def usa_menu_text(text):
    """ASCII menu message -> Unicode, Shift-JIS symbols through the sheet codes, CR and LF as line breaks."""
    pieces, i = [], 0
    while i < len(text):
        if text[i] in (0x81, 0x82) and i + 1 < len(text):
            code = USA_MENU_SJIS.get(bytes(text[i:i + 2]))
            pieces.append(USA_MENU_CHARS.get(code, USA_FIELD_CHARS[code - 0x20]) if code else chr(0xFFFD))
            i += 2
            continue
        pieces.append("\n" if text[i] in (10, 13) else chr(text[i]))
        i += 1
    return "".join(pieces).rstrip("\n")


def usa_char(code, chars=None):
    if chars and code in chars:
        return chars[code]
    return USA_FIELD_CHARS[code - 0x20] if 0x20 <= code < 0x20 + len(USA_FIELD_CHARS) else "[%02X]" % code


def usa_markup(raw, keys=None, chars=None):
    """USA message bytes -> the en.csv notation; `keys` maps ACCENT arguments to mesgen_en keys."""
    pieces, i, num = [], 0, len(raw)
    while i < num:
        code = raw[i]
        if code >= 0x20:
            pieces.append({0xA4: "{♪}", 0xA5: "{♪♪}"}.get(code) or usa_char(code, chars))
            i += 1
            continue
        args = raw[i + 1:i + 1 + USA_CTRL_ARGS.get(code, 0)]
        i += 1 + len(args)
        v = args[0] if args else 0
        if code == 0x01:
            pieces.append(BS + "n")
        elif code == 0x07:
            pieces.append("{item(%d)}" % int.from_bytes(args, "little"))
        elif code == 0x08:
            if v == 0x83:
                pieces.append("{RAZ%}")
            elif v & 0x80:
                pieces.append("{SERGE%%%d}" % (v & 0x7F))
            else:
                pieces.append("{%s%%}" % SPEAKERS[v] if v < len(SPEAKERS) and SPEAKERS[v] else "{name(%d)}" % v)
        elif code == 0x0E:
            pieces.append("{color(%s)}" % USA_COLOUR_MARKUP.get(v, v))
        elif code == 0x12:
            pieces.append((keys or {}).get(v, "{dialect(%d)}" % v))
        elif code == 0x1B:
            pieces.append("{key(%s)}" % USA_BUTTON_MARKUP.get(v, v))
        elif code == 0x1C:
            pieces.append(USA_CASE_MARKUP.get(v, "{case(%d)}" % v))
        elif code == 0x1E:
            pieces.append(USA_SUFFIX_MARKUP.get(v, "{suffix(%d)}" % v))
        elif code == 0x1F:
            pieces.append("◆")
        elif code in CTRL:
            tpl = CTRL[code][2]
            pieces.append(tpl.replace("%d", str(v)).replace("%%", "%") if "%d" in tpl else tpl)
        else:
            pieces.append("{c%02X}" % code)
    return "".join(pieces)


def usa_dialect_keys(build, lang):
    """ACCENT argument -> mesgen_en key, by matching each USA dialect table (through the character -> column map) with
    every mesgen_en row over character ids 0..49.  Returns ({arg: key}, {arg: matching ids})."""
    tables = build.tables()
    columns = build.dialect_columns()
    rows = lang.mesgen("en")[1:]

    def flat(text):
        text = text.replace("{-y}", "").replace("{-i}", "").replace("◆", "")
        return re.sub(r"[{}%\d_]", "", text)
    keys, score = {}, {}
    for arg in range(1, len(tables) - USA_DIALECT_TABLE):
        table = tables[arg + USA_DIALECT_TABLE]
        values = []
        for cid in range(50):
            k = cid + 1 if arg == USA_NAME_FORMS else columns[cid] + 1
            values.append(flat(usa_markup(table[k])) if k < len(table) else None)
        best, best_n = None, -1
        for key, vals in rows:
            hits = sum(1 for cid in range(50) if values[cid] is not None and flat(vals[cid]) == values[cid])
            if hits > best_n:
                best, best_n = key, hits
        keys[arg], score[arg] = best, best_n
    return keys, score


def usa_units(raw):
    """Glyph / control units of a USA message as en.csv writes it: case codes dropped, the digit row of a number entry
    dropped, the trailing end codes dropped."""
    u, i, num = [], 0, len(raw)
    while i < num:
        code = raw[i]
        if code >= 0x20:
            u.append(("g", code))
            i += 1
            continue
        if code == 0x10:
            u.append(("c", code))
            i += 12
            continue
        if code != 0x1C:
            u.append(("c", code))
        i += 1 + USA_CTRL_ARGS.get(code, 0)
    while u and u[-1][0] == "c" and u[-1][1] in (0, 1):
        u.pop()
    return u


def en_units(row):
    row = re.sub(r"\{(Cap|CAP|CapL)\}", "", row)
    u = [("c", unit) if unit == "◆" else (kind, unit) for kind, unit in text_units(row)]
    while u and (u[-1] == ("c", "\n") or u[-1][1] in ("{mend}",)):
        u.pop()
    return u


def usa_glyph_votes(build, lang, ids):
    """code -> Counter of en.csv characters, from same-index messages whose unit sequences line up."""
    en = lang.table("en.csv", "main")
    names = collections.defaultdict(list)
    for rid, nm in ids.items():
        names[nm].append(rid)
    votes = collections.defaultdict(collections.Counter)
    stat = collections.Counter()
    for room in build.rooms():
        msgs = build.room_messages(room)
        for rid in names.get(room, [])[:1]:
            for i, raw in enumerate(msgs or []):
                row = en.get((rid, i))
                if row is None or row.strip() == "{ERROR}":
                    continue
                bu, tu = usa_units(raw), en_units(row)
                if len(bu) != len(tu) or any(x[0] != y[0] for x, y in zip(bu, tu)):
                    stat["not aligned"] += 1
                    continue
                stat["aligned"] += 1
                for x, y in zip(bu, tu):
                    if x[0] == "g":
                        votes[x[1]][y[1]] += 1
    return votes, stat


def cmd_usa_glyphs(a):
    build = UsaBuild(a.disc, a.disc2)
    ids, _ = load_room_ids(a.roomids)
    votes, stat = usa_glyph_votes(build, Lang(), ids)
    frames = build.menu_frames()
    lines = ["# USA glyph table: code -> character for the field font (executable bitmaps, glyph = code - 0x20) and the menu",
             "# sheet (usfo frames of menu/menutest.prd over the usfo TIM of record 234). Characters are identified from the",
             "# bitmaps; votes count en.csv characters at the same place in aligned messages (cc_text.py usa-glyphs).",
             "# columns: code\tfield_char\tfield_advance\tmenu_char\tmenu_u\tmenu_v\tmenu_w\tmenu_h\tmenu_x\tmenu_y\tmenu_advance"
             "\tvotes\tshare\trunner_up\tnote"]
    disagree = []
    for code in range(0x20, 0x20 + len(USA_FIELD_CHARS)):
        ch = usa_char(code)
        fr = frames.get(code)
        menu = USA_MENU_CHARS.get(code, ch) if fr else ""
        cnt = votes.get(code, collections.Counter())
        tot = sum(cnt.values())
        top = cnt.most_common(2)
        share = top[0][1] / tot if tot else 0.0
        if top and top[0][0] != ch and normalise_nonjp(top[0][0], "en") != normalise_nonjp(ch, "en") and share >= 0.5:
            disagree.append((code, ch, top[0]))
        runner = ("%sx%d" % top[1]) if len(top) > 1 else ""
        geo = "\t".join(str(x) for x in fr[:7]) if fr else "\t\t\t\t\t\t"
        twin = USA_FIELD_CHARS.find(ch)
        note = USA_FIELD_NOTES.get(code) or ("same character as %02x" % (0x20 + twin) if 0x20 + twin < code else "")
        lines.append("%02x\t%s\t%d\t%s\t%s\t%d\t%.3f\t%s\t%s" % (
            code, ch, build.widths[code - 0x20], menu, geo, tot, share, runner, note))
    out("messages: %s" % dict(stat))
    out("codes with votes: %d of %d; votes disagreeing with the bitmap identification: %s" % (
        len(votes), len(USA_FIELD_CHARS), ", ".join("%02x %r->%r" % (code, ch, row) for code, ch, row in disagree) or "none"))
    out("menu sheet frames: %d" % len(frames))
    if a.write:
        with open(USA_GLYPHS, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
        out("wrote %s" % USA_GLYPHS)


def cmd_usa_room(a):
    build = UsaBuild(a.disc, a.disc2)
    msgs = build.room_messages(a.room)
    if msgs is None:
        raise SystemExit("no room %r on the USA discs" % a.room)
    lang = Lang()
    keys, _ = usa_dialect_keys(build, lang)
    ids, _ = load_room_ids(a.roomids)
    rid = next((r for r, nm in sorted(ids.items()) if nm == a.room), None)
    en = lang.table("en.csv", "main")
    out("; USA room %s: %d messages (en.csv room id %s)" % (a.room, len(msgs), rid))
    for i, raw in enumerate(msgs):
        out("[%3d] US: %s" % (i, usa_markup(raw, keys)))
        if a.en and rid is not None and (rid, i) in en:
            out("      EN: %s" % en[(rid, i)])


def cmd_usa_tables(a):
    build = UsaBuild(a.disc, a.disc2)
    lang = Lang()
    tables = build.tables()
    keys, score = usa_dialect_keys(build, lang)
    rows = ["# USA executable string tables: index, entries (the last offset only ends the table), what they hold, the RDE",
            "# table and key they correspond to, and the Japanese executable's table of the same content (cc_text.py usa-tables).",
            "# columns: table\tentries\tkind\trde\tjp_table\tdialect_arg\tmatch"]
    for index, entries in enumerate(tables):
        if index in USA_TABLE_KINDS:
            kind, cat, jp = USA_TABLE_KINDS[index]
            rows.append("%d\t%d\t%s\tsysmes 0x%02X\t%d\t\t" % (index, len(entries), kind, cat, jp))
        else:
            arg = index - USA_DIALECT_TABLE
            rows.append("%d\t%d\tdialect form\tmesgen_en %s\t\t0x%02X\t%d/50" % (
                index, len(entries), keys.get(arg), arg, score.get(arg, 0)))
        if a.cat is not None and a.cat != index:
            continue
        if a.cat is not None or a.verbose:
            for i, entry in enumerate(entries):
                out("%2d %4d  %s" % (index, i, esc(usa_markup(entry, keys))))
    if a.cat is None:
        out("\n".join(rows[3:]))
        cols = build.dialect_columns()
        out("dialect column per character id: %s" % " ".join(str(col) for col in cols))
        out("text colours: %s" % build.colours())
    if a.write:
        with open(USA_TABLES, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(rows) + "\n")
        out("wrote %s" % USA_TABLES)


def usa_menu_key(text):
    text = text.replace("{66}", '"').replace("{99}", '"').replace("“", '"').replace("”", '"')
    return re.sub(r"\s+", " ", text.replace(BS + "n", " ")).strip().lower()


def cmd_usa_menu(a):
    """menutest.txt / shopmenu.txt keys (JP overlay addresses) -> the USA menu message (and line) holding the same text."""
    build = UsaBuild(a.disc, a.disc2)
    mess = build.menu_messages()
    parts = []
    for i, message in enumerate(mess):
        parts.append((usa_menu_key(message), i, ""))
        for j, seg in enumerate(re.split(r"[\n\t]+", message)):
            if seg.strip():
                parts.append((usa_menu_key(seg), i, str(j)))
    lang = Lang()
    rows = ["# RDE menu / shop string keys (the JP overlay address) -> USA menu message: index in the 'mess' entry of",
            "# menu/menutest.prd and the line / tab segment holding the same English text (cc_text.py usa-menu).",
            "# match: exact = the whole message, segment = one line or tab field of it, contained = inside a longer segment.",
            "# columns: table\tkey\tmess\tsegment\tmatch"]
    for table in ("menutest", "shopmenu"):
        sheet = parse_tsv(decode_text(lang.read(table + ".txt")))
        col = [i for i, h in enumerate(sheet[0]) if h.strip().lower() == "en"][0]
        st = collections.Counter()
        for r in sheet[1:]:
            if not r[0].startswith("0x"):
                continue
            text = r[col] if len(r) > col else ""
            if not text.strip():
                st["empty"] += 1
                continue
            k = usa_menu_key(text)
            hit = next((part for part in parts if part[0] == k), None)
            how = "exact" if hit and not hit[2] else "segment" if hit else ""
            if not hit:
                hit = next((part for part in parts if k and k in part[0] and part[2]), None)
                how = "contained" if hit else "none"
            st[how] += 1
            rows.append("%s\t%s\t%s\t%s\t%s" % (table, r[0].upper().replace("0X", "0x"), hit[1] if hit else "",
                                               hit[2] if hit else "", how))
        out("%s: %s" % (table, dict(st)))
    out("USA menu messages: %d" % len(mess))
    if a.verbose:
        for i, message in enumerate(mess):
            out("%3d %s" % (i, esc(message)))
    if a.write:
        with open(USA_MENU_STRINGS, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(rows) + "\n")
        out("wrote %s" % USA_MENU_STRINGS)


def usa_level(u, row):
    """0 identical, 1 after the {Cap} before names and the number-entry digit row, 2 also line breaks and quotes,
    3 also waits, speeds and case marks; None = different text."""
    names = (r"(?=\{[A-Z]+%\d?\}|\{PARTY\d%\}|\{playwork\(\d\)\}|\{itemwork\(\d\)\}|\{RAZ%\}|\{SERGE\}"
             r"|\{color\(\d\)\}\{)")
    for level in range(4):
        if level >= 1:
            u = re.sub(r"\{input\((\d)\)\}0123456789", r"{input(\1)}", re.sub(r"\{Cap\}" + names, "", u))
            row = re.sub(r"\{Cap\}" + names, "", row)
        if level >= 2:
            u, row = (re.sub(r" +", " ", text.replace(BS + "n", " ").replace("{bpage}", "{bpage} ").replace("’", "'")
                           .replace("“", '"').replace("”", '"')) for text in (u, row))
        if level >= 3:
            u, row = (re.sub(r"\s+", " ", re.sub(r"\{(?:speed|wait|wend|fset|fwait)\([^)]*\)\}|\{(?:Cap|CAP|CapL)\}", "", text))
                    .strip() for text in (u, row))
        if u == row:
            return level
    return None


def usa_plain(text):
    return re.sub(r"\W+", "", re.sub(r"\{[^}]*\}", "", text.replace(BS + "n", " ")).lower())


def cmd_usa_compare(a):
    """Match rate of the USA disc text against RDE's English tables."""
    build = UsaBuild(a.disc, a.disc2)
    lang = Lang()
    keys, _ = usa_dialect_keys(build, lang)
    ids, _ = load_room_ids(a.roomids)
    names = collections.defaultdict(list)
    for rid, nm in ids.items():
        names[nm].append(rid)
    en = lang.table("en.csv", "main")
    st = collections.Counter()
    for room in build.rooms():
        msgs = build.room_messages(room) or []
        for rid in names.get(room, [])[:1]:
            plain = {usa_plain(row) for (r, _), row in en.items() if r == rid and row.strip() != "{ERROR}"}
            for i, raw in enumerate(msgs):
                u = usa_markup(raw, keys).rstrip()
                while u.endswith(BS + "n"):
                    u = u[:-2]
                row = en.get((rid, i))
                if row is not None and row.strip() == "{ERROR}":
                    st["en row is {ERROR}"] += 1
                    continue
                level = usa_level(u, row) if row is not None else None
                if level is not None:
                    st["same index, level %d" % level] += 1
                elif row is not None and usa_plain(u) == usa_plain(row):
                    st["same index, punctuation or markup only"] += 1
                elif usa_plain(u) in plain:
                    st["other index"] += 1
                else:
                    st["edited or no counterpart"] += 1
    out("field messages: %s" % sorted(st.items()))
    bt = {}
    for line in lang.read("btel_en.csv").decode("utf-8-sig").split("\n"):
        part = line.rstrip("\r").split("\t", 2)
        if len(part) == 3 and part[0].isdigit() and part[1].isdigit():
            bt[(int(part[0]), int(part[1]))] = part[2]
    st = collections.Counter()
    for scene, msgs in build.battle_packs():
        for i, raw in enumerate(msgs):
            row = bt.get((scene, i))
            if row is None or row.strip() == "{ERROR}":
                continue
            u = usa_markup(raw, keys).rstrip()
            while u.endswith(BS + "n"):
                u = u[:-2]
            level = usa_level(u, row)
            st["level %d" % level if level is not None else "differ"] += 1
    out("battle messages: %s" % sorted(st.items()))
    sysmes = {}
    sheet = parse_tsv(decode_text(lang.read("sysmes.txt")))
    col = [i for i, h in enumerate(sheet[0]) if h.strip().lower() == "en"][0]
    for r in sheet[1:]:
        if r[0].startswith("0x"):
            sysmes[int(r[0], 16)] = r[col] if len(r) > col else ""
    tables = build.tables()
    for table, (kind, cat, _) in sorted(USA_TABLE_KINDS.items()):
        st = collections.Counter()
        for i, raw in enumerate(tables[table]):
            rde = sysmes.get((cat << 16) | i)
            if rde is None:
                continue
            u = usa_markup(raw, keys)
            st["exact" if u == rde else "spacing" if u.split() == rde.split() else "differ"] += 1
        out("table %d (%s) vs sysmes 0x%02X: %s" % (table, kind, cat, dict(st)))


def cmd_usa_fontsheet(a):
    build = UsaBuild(a.disc, a.disc2)
    count, cols, cell = len(USA_FIELD_CHARS), 16, 15
    w, h, sc = cols * cell + 2, (count + cols - 1) // cols * 14 + 2, a.scale
    pix = bytearray(b"\xff" * (w * sc * h * sc))
    for k in range(count):
        x0, y0 = 2 + (k % cols) * cell, 2 + (k // cols) * 14
        for r, bits in enumerate(build.glyph(0x20 + k)):
            for col in range(16):
                if bits & (0x8000 >> col):
                    for dy in range(sc):
                        at = ((y0 + r) * sc + dy) * w * sc + (x0 + col) * sc
                        pix[at:at + sc] = b"\x00" * sc
    write_png(a.out, w * sc, h * sc, pix)
    out("USA field glyphs 0x20..0x%02X -> %s" % (0x1F + count, a.out))


# --------------------------------------------------------------------------- main
def main(argv=None):
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(prog="cc_text", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--roomids", help="room id csv (room,file,...) [default tools/py/data/text_room_ids.csv]")
    ap.add_argument("--disc", help="USA disc 1 image (cue, bin or iso) [default: the DuckStation games folder]")
    ap.add_argument("--disc2", help="USA disc 2 image, for the rooms only disc 2 holds")
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
    p = sp.add_parser("usa-glyphs")
    p.add_argument("--write", action="store_true")
    p.set_defaults(fn=cmd_usa_glyphs)
    p = sp.add_parser("usa-room")
    p.add_argument("room")
    p.add_argument("--en", action="store_true", help="show the en.csv row under each message")
    p.set_defaults(fn=cmd_usa_room)
    p = sp.add_parser("usa-tables")
    p.add_argument("--cat", type=int, help="dump one table")
    p.add_argument("--verbose", action="store_true", help="dump every table")
    p.add_argument("--write", action="store_true")
    p.set_defaults(fn=cmd_usa_tables)
    p = sp.add_parser("usa-menu")
    p.add_argument("--verbose", action="store_true", help="list the menu messages")
    p.add_argument("--write", action="store_true")
    p.set_defaults(fn=cmd_usa_menu)
    sp.add_parser("usa-compare").set_defaults(fn=cmd_usa_compare)
    p = sp.add_parser("usa-fontsheet")
    p.add_argument("out")
    p.add_argument("--scale", type=int, default=2)
    p.set_defaults(fn=cmd_usa_fontsheet)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
