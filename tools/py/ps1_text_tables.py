"""Writes the generated text tables of src/import/text_tables.inc.

Run with:  python -I tools/py/ps1_text_tables.py [--check]

Blocks it writes (the room-id block is kept as it is):
  kJpGlyphChars   the JP glyph table, from tools/py/data/jp_glyphs.tsv (cc_text.py glyphs --write)
  kSjisChars      the Shift-JIS characters the JP menu files use (menutest.prg, shopmenu.prg, namemenu.prg, menutest.prd, the .ome profiles),
                  code -> Unicode by the standard code page
  kMenuPartsJp    how a menutest / shopmenu row is put together from the menu overlay's strings when it is not the one string
                  at its key address: the parts (guest addresses) and which of them start a new line
  kOmeFilesJp     the character profile file of each ome row
  kSaveTitleProgressJp  the story-progress value of each save-location title of menutest.prd's "stbl" (JP), the key a USA
                  title (which starts with its progress value) is matched by
  kUsaGlyphChars  the USA glyph table, from tools/py/data/usa_glyphs.tsv (field font column)
  kMenuStringsUsa the USA menu message and line / tab field of each menutest / shopmenu key, and the characters of that field
                  holding a key's text when it sits inside a longer field, from usa_menu_strings.tsv

The menu rows are matched against lang.dat's menutest.txt / shopmenu.txt JP column, ignoring line breaks and blanks: the
overlay strings at the key (one or several in a row), or 1 to 3 strings near it per line, whose text equals the row. A row
with no match is the disc's own text: the string at its key, continued by the strings that follow it while each brings the
text closer to the row (strings that are another row's key end it, except one directly after the key whose follower is not
a key: the other memory card's form of the same line, which shares the rest), or no source when the key is not the start of
a string. The output keeps
the overlay's own line breaks between strings in a row, except in a row whose text is one line: the PS1 draws that row on one
line. Sources: data/cdrom.dat, data/lang.dat.
"""
import argparse
import difflib
import itertools
import os
import re
import sys
import unicodedata
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cc_text as ct  # noqa: E402

OC = os.path.normpath(os.path.join(HERE, "..", ".."))
INC = os.path.join(OC, "src", "import", "text_tables.inc")
MENU_BASE = 0x801D2800
MENU_SOURCES = (("menutest.txt", "prog/menutest.prg", 0), ("shopmenu.txt", "prog/shopmenu.prg", 1))
SJIS_FILES = ("prog/menutest.prg", "prog/shopmenu.prg", "prog/namemenu.prg", "menu/menutest.prd")


def sjis_strings(b):
    """Offset -> text of every NUL-terminated run that is valid Shift-JIS without control characters."""
    out = {}
    i = 0
    while i < len(b):
        if b[i] == 0:
            i += 1
            continue
        end = b.find(b"\0", i)
        end = len(b) if end < 0 else end
        try:
            text = b[i:end].decode("shift_jis")
            if all(ord(ch) >= 0x20 for ch in text):
                out[i] = text
        except UnicodeDecodeError:
            pass
        i = end + 1
    return out


def jp_column(lang, name):
    rows = ct.parse_tsv(ct.decode_text(lang.read(name)))
    col = [h.strip() for h in rows[0]].index("JP")
    return [(r[0], r[col] if len(r) > col else "") for r in rows[1:]]


def bare(text):
    return re.sub(r"[\s　]", "", text)


def likeness(text, want):
    return difflib.SequenceMatcher(None, bare(text), bare(want), autojunk=False).ratio()


def continued(strings, following, keyed, off, want):
    """The string at off and the strings after it that bring the text closer to want (see the module notes)."""
    run = [off]
    best = likeness(strings[off], want)
    at = following.get(off)
    if at in keyed and following.get(at) not in keyed:
        at = following.get(at)
    while at is not None and at not in keyed and len(run) < 4:
        score = likeness("".join(strings[a] for a in run + [at]), want)
        if score <= best:
            break
        run.append(at)
        best = score
        at = following.get(at)
    return run


def menu_parts(lang, z):
    """[(table, key, [(address, starts_line)])] for the rows that are not the single string at their key."""
    rows = []
    stats = {}
    for name, prg, table in MENU_SOURCES:
        strings = sjis_strings(z.read(prg))
        starts = sorted(strings)
        following = {a: starts[i + 1] for i, a in enumerate(starts[:-1])}
        column = jp_column(lang, name)
        keyed = set()
        for key_text, want in column:
            try:
                if want:
                    keyed.add(int(key_text, 0) - MENU_BASE)
            except ValueError:
                pass
        counts = {}
        for key_text, want in column:
            try:
                key = int(key_text, 0)
            except ValueError:
                continue
            off = key - MENU_BASE
            parts = None
            if want and off in strings and bare(strings[off]) == bare(want):
                kind = "single"
            else:
                kind = "none"
                if want and off in strings:
                    run = [off]
                    while len(run) < 6 and run[-1] in following:
                        run.append(following[run[-1]])
                        if bare("".join(strings[a] for a in run)) == bare(want):
                            parts, kind = [(a, i > 0 and "\n" in want) for i, a in enumerate(run)], "lines"
                            break
                if not parts and want:
                    near = [a for a in starts if abs(a - off) < 0x400]
                    found = []
                    for line in want.split("\n"):
                        hit = None
                        for count in (1, 2, 3):
                            for combo in itertools.permutations(near, count):
                                if bare("".join(strings[a] for a in combo)) == bare(line):
                                    hit = combo
                                    break
                            if hit:
                                break
                        if not hit:
                            found = None
                            break
                        found.append(hit)
                    if found:
                        parts = [(a, i > 0 and j == 0) for i, hit in enumerate(found) for j, a in enumerate(hit)]
                        kind = "composed"
                if not parts and want and off in strings:
                    run = continued(strings, following, keyed, off, want)
                    if len(run) > 1:
                        parts, kind = [(a, i > 0 and "\n" in want) for i, a in enumerate(run)], "continued"
                if not parts and off in strings:
                    kind = "edited"
            counts[kind] = counts.get(kind, 0) + 1
            if parts and not (len(parts) == 1 and parts[0][0] == off):
                rows.append((table, key, [(a + MENU_BASE, nl) for a, nl in parts]))
        stats[name] = counts
    return rows, stats


def ome_files(lang, z):
    """Row i of ome.txt -> the profile file (menu/player/<stem>.ome) whose text holds the row's text (compatibility forms
    folded, blanks and colons dropped); of several equal files the first one no earlier row took."""
    def fold(text):
        return re.sub(r"[\s　:]", "", unicodedata.normalize("NFKC", text))

    files = sorted(name for name in z.namelist() if name.startswith("menu/player/") and name.endswith(".ome"))
    texts = {name: fold(z.read(name).decode("shift_jis", "replace")) for name in files}
    out, used = [], set()
    for key, want in jp_column(lang, "ome.txt"):
        lines = [fold(line) for line in want.split("\n") if fold(line)]
        hits = [name for name in files if lines and all(line in texts[name] for line in lines)]
        hits.sort(key=lambda name: name in used)
        out.append(hits[0][len("menu/player/"):-4] if hits else "")
        used.update(hits[:1])
    return out


def sjis_chars(z):
    codes = {}
    texts = [text for name in SJIS_FILES for text in sjis_strings(z.read(name)).values()]
    # A profile's text lines end in CR LF.
    for name in (member for member in z.namelist() if member.endswith(".ome")):
        texts += sjis_strings(z.read(name).replace(b"\r\n", b"\0")).values()
    for text in texts:
        if text:
            for ch in text:
                b = ch.encode("shift_jis")
                if len(b) == 2:
                    codes[b[0] << 8 | b[1]] = ord(ch)
    return codes


def save_title_progress(z):
    """The u32 after each 32-byte title of the JP "stbl" entry of menu/menutest.prd."""
    prd = z.read("menu/menutest.prd")
    count = int.from_bytes(prd[8:12], "little") // 64
    for i in range(count):
        off = int.from_bytes(prd[12 + 4 * i:16 + 4 * i], "little")
        if prd[off + 4:off + 8] == b"stbl":
            length = int.from_bytes(prd[off + 9:off + 12], "little") // 16
            data = prd[off + 12:off + 12 + length]
            return [int.from_bytes(data[k + 32:k + 36], "little") for k in range(0, len(data) - 35, 36)]
    return []


def usa_glyph_block():
    rows = []
    with open(os.path.join(HERE, "data", "usa_glyphs.tsv"), encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            if len(cells) >= 2 and cells[1]:
                rows.append((int(cells[0], 16), ord(cells[1][0])))
    return rows


def usa_menu_strings():
    rows = []
    with open(os.path.join(HERE, "data", "usa_menu_strings.tsv"), encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            if len(cells) >= 5 and cells[2] and cells[4] != "none":
                span = (int(cells[5]), int(cells[6])) if len(cells) >= 7 else (0, 0)
                rows.append((0 if cells[0] == "menutest" else 1, int(cells[1], 0), int(cells[2]), int(cells[3]) if cells[3] else 255) + span)
    return rows


def glyph_block():
    rows = []
    with open(os.path.join(HERE, "data", "jp_glyphs.tsv"), encoding="utf-8") as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            if len(cells) >= 3 and cells[2]:
                rows.append((int(cells[0], 16), ord(cells[2])))
    return rows


def pairs(rows, per_line, fmt):
    lines = []
    for i in range(0, len(rows), per_line):
        lines.append("  " + "".join(fmt(r) for r in rows[i:i + per_line]))
    return lines


def build(lang, z, old):
    room_block = old[old.index("// kRoomNames"):].rstrip("\r\n").split("\n")
    room_block = [l.rstrip("\r") for l in room_block]
    out = ["// Generated tables (PS1 JP glyph codes -> Unicode, Shift-JIS menu characters, menu row parts, lang.dat room ids -> map file",
           "// stems); do not edit by hand: tools/py/ps1_text_tables.py writes every block but kRoomNames.",
           "// kJpGlyphChars: {glyph key (b0 for single-byte codes, b0<<8|b1 for page-prefixed ones), Unicode code point}",
           "inline constexpr struct { unsigned short key; unsigned int cp; } kJpGlyphChars[] = {"]
    out += pairs(glyph_block(), 8, lambda r: "{0x%X,0x%X}," % r)
    out += ["};", "// kSjisChars: {two-byte Shift-JIS code, Unicode code point}, ascending",
            "inline constexpr struct { unsigned short code; unsigned short cp; } kSjisChars[] = {"]
    out += pairs(sorted(sjis_chars(z).items()), 8, lambda r: "{0x%X,0x%X}," % r)
    rows, stats = menu_parts(lang, z)
    out += ["};", "// kMenuPartsJp: Ps1MenuPart rows (import/ps1_text.h)", "inline constexpr Ps1MenuPart kMenuPartsJp[] = {"]
    for table, key, parts in rows:
        addrs = ",".join("0x%X" % a for a, _ in parts[:4])
        mask = sum(1 << i for i, (_, nl) in enumerate(parts[:4]) if nl)
        out.append("  {%d,0x%X,%d,{%s},%d}," % (table, key, min(len(parts), 4), addrs, mask))
    out += ["};", "// kOmeFilesJp: ome row -> menu/player/<stem>.ome", "inline constexpr const char* kOmeFilesJp[] = {"]
    out += pairs(ome_files(lang, z), 6, lambda stem: '"%s",' % stem)
    out += ["};", "// kSaveTitleProgressJp: story-progress value of save-location title row 1, 2, ...",
            "inline constexpr unsigned short kSaveTitleProgressJp[] = {"]
    out += pairs(save_title_progress(z), 16, lambda v: "%d," % v)
    out += ["};", "// kUsaGlyphChars: {USA glyph code, Unicode code point}",
            "inline constexpr struct { unsigned char code; unsigned short cp; } kUsaGlyphChars[] = {"]
    out += pairs(usa_glyph_block(), 8, lambda r: "{0x%X,0x%X}," % r)
    out += ["};", "// kMenuStringsUsa: Ps1MenuString rows (import/ps1_text.h)", "inline constexpr Ps1MenuString kMenuStringsUsa[] = {"]
    out += pairs(usa_menu_strings(), 4, lambda r: "{%d,0x%X,%d,%d,%d,%d}," % r)
    out += ["};"] + room_block
    return "\r\n".join(out) + "\r\n", stats


def main(argv=None):
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(prog="ps1_text_tables", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report only, do not write")
    a = ap.parse_args(argv)
    lang = ct.Lang()
    z = zipfile.ZipFile(os.path.join(ct.DATA, "cdrom.dat"))
    with open(INC, encoding="utf-8", newline="") as handle:
        old = handle.read()
    text, stats = build(lang, z, old)
    for name, counts in stats.items():
        print("%s: %s" % (name, counts))
    if not a.check:
        with open(INC, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        print("wrote %s" % INC)


if __name__ == "__main__":
    main()
