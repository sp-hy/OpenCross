"""Save forge: copy a save.dat with one slot rewritten for reference captures (never point it at the owner's folder).

    python tools/py/cc_save_forge.py <in save.dat> <out save.dat> --slot N [options]

Options (all optional; every edit lands in slot N of memcard1.mcr, the block checksum is recomputed, the output is a
plain stored ZIP with the same members, which OpenCross and ps1 read like RDE's WinZip-AES file):
  --room ID            location word (raw head 0x18E = script variable 6): the room Continue loads
  --entrance N         script variable 0x3A (raw head 0x1C2): the ROOM_PARAM entrance Continue re-enters with
  --party a[,b[,c]]    party character ids (raw head 0x59C.. and the 0x34 preview; 0xFF pads); recruits them too
  --recruit a,b,...    extra recruited-character bits (raw head 0x590, 44 bits)
  --char ID:k=v,...    stat edits of one character record: hp (current + max, 10 bits), curhp (current only; 0 = KO'd), atk def hit evd mag mdef
                       (7 bits), weapon armor (item ids)
  --grid ID:ROW:e,e,.. element ids written left to right into the character's ROW (0 = level 1) cells; the whole grid
                       is unlocked for that character (0xDAD8 masks = the char.bin level masks) and the equipped
                       quantity nibble of every filled cell is set to 5
  --unlock ID          unlock the whole grid of a character without touching its cells
  --var ID=VALUE       script variable (u16 at raw head 0x188 + ID; ID in hex with 0x or decimal)
  --set-bit N / --clear-bit N   SET_VAR_BIT / CLEAR_VAR_BIT semantics: var[N >> 4] bit N & 15
  --scenario N         scenario (story progression) counter (variable 0 and the preview progression, raw head 0x40)
  --key-items N[,N..]  the key items held, replacing the source save's (1-based number, raw head 0x180 bit N - 1; Time Egg = 53)
  --stars N            star level (raw head 0x3F preview, 0x5A4 total, 0x5A5 current)
  --gold N             gold (0x58C and the 0x3B preview)
  --cleared            a New Game+ "cleared" save as the ending's FE 85 writes it: variable 0x7E = 3, summary flags (raw head 0x42
                       bits 0-1) = 3, location = map 9 (the ending room) with entrance 3 (Continue opens the New Game+ title list)
  --flags N            only the 2 summary flag bits (raw head 0x42; 2 = a New Game+ game, 3 = cleared)
  --mcd PATH           also write the forged memcard1.mcr as a raw 128 KiB memory card image (stock DuckStation .mcd)
  --mcd-only           with --mcd: skip writing the output save.dat

  rowlen CHAR ROW      (own sub-command) print the number of cells of that grid row. Tech cells (Pilfer = 163 ...) are plain
                       element ids in the save record, so --grid places techs like elements.

Save layout used: the character record is 0xCC bytes at 0xAA24 + 0xCC i with the grid cells at +0x40; the 48 grid cells are listed row by row
for the set bits of the char.bin level masks +0x30..+0x3F.
"""
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_save as c  # noqa: E402

REC = 0xAA24
REC_SIZE = 0xCC
NAME_OFF = 0xD80C
UNLOCK_OFF = 0xDAD8
QTY_OFF = 0xDD98


def level_masks(char_id):
    """char.bin +0x30..+0x3F: 8 x u16 cell masks per row (needs the game dir: parent of OpenCross or --game)."""
    import cc_tables as t
    g = t.Game(os.environ.get("OPENCROSS_GAMEDIR") or None)
    recs, _ = t.load_chars(g)
    r = recs[char_id]
    return [struct.unpack_from("<H", r, 0x30 + 2 * k)[0] for k in range(8)]


def cell_index(masks, row, col_rank):
    """Index into the 48-cell list of the col_rank-th set bit of row `row` (bits ascending)."""
    idx = 0
    for r in range(row):
        idx += bin(masks[r]).count("1")
    bits = [b for b in range(16) if masks[row] >> b & 1]
    if col_rank >= len(bits):
        raise SystemExit("row %d has only %d cells" % (row, len(bits)))
    return idx + col_rank


def parse_kv(spec):
    out = {}
    for kv in spec.split(","):
        if not kv:
            continue
        k, v = kv.split("=")
        out[k.strip()] = int(v, 0)
    return out


def main(argv):
    if len(argv) == 3 and argv[0] == "rowlen":
        # rowlen CHAR ROW: number of grid cells in that row (the element list wraps over this many)
        print(bin(level_masks(int(argv[1], 0))[int(argv[2], 0)]).count("1"))
        return 0
    if len(argv) < 3:
        print(__doc__)
        return 1
    src, dst = argv[0], argv[1]
    args = argv[2:]
    opts = {"slot": None, "room": None, "entrance": None, "party": None, "recruit": [], "char": [], "grid": [],
            "unlock": [], "var": [], "set_bit": [], "clear_bit": [], "stars": None, "scenario": None, "key_items": None, "gold": None, "flags": None, "mcd": None, "mcd_only": False}
    i = 0
    while i < len(args):
        a = args[i]
        v = args[i + 1] if i + 1 < len(args) else None
        if a == "--slot":
            opts["slot"] = int(v)
        elif a == "--room":
            opts["room"] = int(v, 0)
        elif a == "--entrance":
            opts["entrance"] = int(v, 0)
        elif a == "--party":
            opts["party"] = [int(x, 0) for x in v.split(",")]
        elif a == "--recruit":
            opts["recruit"] += [int(x, 0) for x in v.split(",")]
        elif a == "--char":
            opts["char"].append(v)
        elif a == "--grid":
            opts["grid"].append(v)
        elif a == "--unlock":
            opts["unlock"].append(int(v, 0))
        elif a == "--var":
            opts["var"].append(v)
        elif a == "--set-bit":
            opts["set_bit"].append(int(v, 0))
        elif a == "--clear-bit":
            opts["clear_bit"].append(int(v, 0))
        elif a == "--scenario":
            opts["scenario"] = int(v, 0)
        elif a == "--key-items":
            opts["key_items"] = [int(x, 0) for x in v.split(",")]
        elif a == "--stars":
            opts["stars"] = int(v, 0)
        elif a == "--gold":
            opts["gold"] = int(v, 0)
        elif a == "--cleared":
            opts["flags"] = 3
            opts["var"].append("0x7E=3")
            opts["room"] = 9
            opts["entrance"] = 3
            i += 1
            continue
        elif a == "--flags":
            opts["flags"] = int(v, 0)
        elif a == "--mcd":
            opts["mcd"] = v
        elif a == "--mcd-only":
            opts["mcd_only"] = True
            i += 1
            continue
        else:
            raise SystemExit("unknown option %s" % a)
        i += 2
    if opts["slot"] is None:
        raise SystemExit("--slot is required")
    slot = opts["slot"]

    members = c.read_zip(src)
    card_member = next((m for m in members if m.name == "memcard1.mcr"), None)
    if card_member is None:
        raise SystemExit("no memcard1.mcr in %s" % src)
    card = c.Card(card_member.data)
    slots = card.cc_slots()
    if slot not in slots:
        raise SystemExit("slot %d not on card 1 (have %s)" % (slot, sorted(slots)))
    entry, blk = slots[slot]
    blk = bytearray(blk)
    img = bytearray(c.decompress(blk))
    log = []

    def var_set(vid, value):
        struct.pack_into("<H", blk, 0x188 + vid, value & 0xFFFF)

    def var_get(vid):
        return struct.unpack_from("<H", blk, 0x188 + vid)[0]

    if opts["room"] is not None:
        var_set(6, opts["room"])
        log.append("room %d" % opts["room"])
    if opts["entrance"] is not None:
        var_set(0x3A, opts["entrance"])
        log.append("entrance %d" % opts["entrance"])
    for spec in opts["var"]:
        k, v = spec.split("=")
        var_set(int(k, 0), int(v, 0))
        log.append("var %s=%s" % (k, v))
    for n in opts["set_bit"]:
        var_set(n >> 4, var_get(n >> 4) | (1 << (n & 15)))
        log.append("set bit %d" % n)
    for n in opts["clear_bit"]:
        var_set(n >> 4, var_get(n >> 4) & ~(1 << (n & 15)))
        log.append("clear bit %d" % n)

    recruit = list(opts["recruit"])
    if opts["party"] is not None:
        ids = (opts["party"] + [0xFF, 0xFF, 0xFF])[:3]
        for k in range(3):
            blk[0x59C + k] = ids[k]
            blk[0x34 + k] = ids[k]
        recruit += [x for x in ids if x != 0xFF]
        # Preview leader name (Shift-JIS) from the image name of party[0] is not re-encoded: the load list keeps the
        # old preview name, which the game overwrites on the next save.
        log.append("party %s" % ids)
    if recruit:
        bits = int.from_bytes(blk[0x590:0x596], "little")
        for x in recruit:
            bits |= 1 << x
        blk[0x590:0x596] = bits.to_bytes(6, "little")
        log.append("recruited += %s" % sorted(set(recruit)))

    for spec in opts["char"]:
        cid, kv = spec.split(":", 1)
        cid = int(cid, 0)
        b = REC + cid * REC_SIZE
        for k, v in parse_kv(kv).items():
            if k == "hp":
                v = min(v, 1023)
                struct.pack_into("<H", img, b + 0x16, v)   # max HP
                struct.pack_into("<H", img, b + 0xB8, v)   # current HP
                struct.pack_into("<H", img, b + 0xBC, v)   # max-HP related
            elif k == "curhp":   # current HP only (0 = the character starts the battle KO'd); max HP is left alone
                struct.pack_into("<H", img, b + 0xB8, min(v, 1023))
            elif k in ("atk", "def", "hit", "evd", "mag", "mdef"):
                off = {"atk": 0x18, "def": 0x19, "hit": 0x1A, "evd": 0x1B, "mag": 0x1C, "mdef": 0x1D}[k]
                img[b + off] = min(v, 127)
            elif k == "weapon":
                struct.pack_into("<H", img, b + 0x20, v)
            elif k == "armor":
                struct.pack_into("<H", img, b + 0x22, v)
            else:
                raise SystemExit("unknown stat %s" % k)
        log.append("char %d: %s" % (cid, kv))

    def unlock_all(cid):
        masks = level_masks(cid)
        for k in range(8):
            struct.pack_into("<H", img, UNLOCK_OFF + cid * 0x10 + 2 * k, masks[k])
        return masks

    for cid in opts["unlock"]:
        unlock_all(cid)
        log.append("unlock %d" % cid)
    for spec in opts["grid"]:
        cid, row, elems = spec.split(":", 2)
        cid, row = int(cid, 0), int(row, 0)
        masks = unlock_all(cid)
        b = REC + cid * REC_SIZE
        placed = []
        for rank, e in enumerate([int(x, 0) for x in elems.split(",") if x]):
            cell = cell_index(masks, row, rank)
            struct.pack_into("<H", img, b + 0x40 + 2 * cell, e & 0x1FF)
            q = QTY_OFF + cid * 0x18 + (cell >> 1)
            img[q] = (img[q] & 0x0F) | 0x50 if cell & 1 else (img[q] & 0xF0) | 0x05
            placed.append((cell, e))
        log.append("grid %d row %d: %s" % (cid, row, placed))

    if opts["flags"] is not None:
        blk[0x42] = (blk[0x42] & ~3) | (opts["flags"] & 3)
        log.append("summary flags %d" % (opts["flags"] & 3))
    if opts["scenario"] is not None:
        struct.pack_into("<H", blk, 0x40, opts["scenario"])
        var_set(0, opts["scenario"])
        log.append("scenario %d" % opts["scenario"])
    if opts["key_items"] is not None:
        blk[0x180:0x188] = bytes(8)
        for n in opts["key_items"]:
            blk[0x180 + ((n - 1) >> 3)] |= 1 << ((n - 1) & 7)
        log.append("key items %s" % opts["key_items"])
    if opts["stars"] is not None:
        blk[0x3F] = blk[0x5A4] = blk[0x5A5] = opts["stars"] & 0xFF
        log.append("stars %d" % opts["stars"])
    if opts["gold"] is not None:
        struct.pack_into("<I", blk, 0x58C, opts["gold"])
        struct.pack_into("<I", blk, 0x3B, opts["gold"])
        log.append("gold %d" % opts["gold"])

    new_blk = c.pack_block(bytes(blk), bytes(img))
    data = bytearray(card_member.data)
    base = c.BLOCK * entry.idx
    data[base:base + c.BLOCK] = new_blk
    card_member.data = bytes(data)
    if opts["mcd_only"] and not opts["mcd"]:
        raise SystemExit("--mcd-only needs --mcd PATH")
    if opts["mcd"]:
        if len(card_member.data) != 0x20000 or card_member.data[:2] != b"MC":
            raise SystemExit("memcard1.mcr is not a raw 128 KiB card image")
        with open(opts["mcd"], "wb") as f:
            f.write(card_member.data)
        print("wrote %s (%d bytes)" % (opts["mcd"], len(card_member.data)))
    if opts["mcd_only"]:
        return 0
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as z:
        for m in members:
            z.writestr(m.name, m.data)
    print("wrote %s: slot %d (block %d): %s" % (dst, slot, entry.idx, "; ".join(log) or "no edits"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
