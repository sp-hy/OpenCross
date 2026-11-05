"""cc_save - reader/verifier for Chrono Cross (RDE) save.dat and PS1 memory-card images.

Original code, stdlib only.  READ-ONLY: never writes to a save folder
(`dump` writes only the JSON path you give it; point it outside the save folder).
Run with:  python -I tools/py/cc_save.py <command> ...

Commands:
  info <save.dat>                    archive members, sizes, AES/CRC/HMAC status
  slots <save.dat|.mcr> [--card N]   memory-card directory + per-slot preview fields
  dump <save.dat|.mcr> <slot> <out.json> [--card N] [--image]
                                     fully decoded slot (party, 44 characters, inventory, key items, flags)
  checksum <save.dat|.mcr> [--card N]  verify block checksum of every used block (+ recompress round-trip)
  ext <save.dat>                     ext_save.dat fields

<slot> is the CHRONOnn number from the directory filename (00 = auto-save).  --card N picks
memcardN.mcr inside a save.dat (default 1).  A bare .mcr file is treated as the card.

The decompressor step table (STEPS), the JP letter table and the field offsets are transcribed from
RyudoSynbios/game-tools-collection (MIT License, (c) RyudoSynbios),
src/lib/templates/chrono-cross/saveEditor/{template.ts,utils/resource.ts,utils/compression/*.ts}.
"""
import hashlib
import hmac
import json
import struct
import sys
import zlib

PASSWORD = b"376c671d-da29-44c7-a158-4a18faa5fed0"

# --------------------------------------------------------------------------
# AES-256 (encrypt direction only; WinZip AES uses CTR so decrypt == encrypt)
# --------------------------------------------------------------------------
try:  # optional fast path
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
except Exception:  # pragma: no cover
    Cipher = None


def _build_sbox():
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)          # p *= 3
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09                                                  # q /= 3
        x = q ^ ((q << 1) | (q >> 7)) & 0xFF ^ ((q << 2) | (q >> 6)) & 0xFF \
            ^ ((q << 3) | (q >> 5)) & 0xFF ^ ((q << 4) | (q >> 4)) & 0xFF
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


_SBOX = _build_sbox()
_XT = [((i << 1) ^ 0x1B) & 0xFF if i & 0x80 else i << 1 for i in range(256)]


def _expand_key(key):
    nk = len(key) // 4
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _XT[rcon]
        elif nk > 6 and i % nk == 4:
            t = [_SBOX[b] for b in t]
        w.append([w[i - nk][j] ^ t[j] for j in range(4)])
    rks = []
    for r in range(nr + 1):
        rk = []
        for c in range(4):
            rk += w[4 * r + c]
        rks.append(rk)
    return rks, nr


_SHIFT = [0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11]


def _encrypt_block(rks, nr, block):
    s = [block[i] ^ rks[0][i] for i in range(16)]
    for r in range(1, nr + 1):
        s = [_SBOX[s[_SHIFT[i]]] for i in range(16)]
        if r != nr:
            o = []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c:c + 4]
                o += [_XT[a0] ^ _XT[a1] ^ a1 ^ a2 ^ a3,
                      a0 ^ _XT[a1] ^ _XT[a2] ^ a2 ^ a3,
                      a0 ^ a1 ^ _XT[a2] ^ _XT[a3] ^ a3,
                      _XT[a0] ^ a0 ^ a1 ^ a2 ^ _XT[a3]]
            s = o
        rk = rks[r]
        s = [s[i] ^ rk[i] for i in range(16)]
    return bytes(s)


def aes_ctr_winzip(key, data):
    """WinZip AE CTR: 16-byte little-endian counter starting at 1, big keystream block XOR."""
    if Cipher is not None:
        try:
            ks = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
            out = bytearray()
            for i in range(0, len(data), 16):
                ctr = (i // 16 + 1).to_bytes(16, "little")
                out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks.update(ctr)))
            return bytes(out)
        except Exception:
            pass
    rks, nr = _expand_key(key)
    out = bytearray()
    for i in range(0, len(data), 16):
        ks = _encrypt_block(rks, nr, (i // 16 + 1).to_bytes(16, "little"))
        out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks))
    return bytes(out)


# --------------------------------------------------------------------------
# ZIP container (WinZip AES-256, AE-2, deflate)
# --------------------------------------------------------------------------

class Member:
    def __init__(self, name):
        self.name = name
        self.size = 0
        self.csize = 0
        self.crc = 0
        self.method = 0
        self.actual = 0
        self.aes_version = 0
        self.strength = 0
        self.flags = 0
        self.data = b""
        self.hmac_ok = None
        self.crc_ok = None
        self.verifier_ok = None


def read_zip(path_or_bytes, password=PASSWORD):
    buf = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, "rb").read()
    i = buf.rfind(b"PK\x05\x06")
    if i < 0:
        raise ValueError("no end-of-central-directory (truncated archive / libzip temp file?)")
    _, _, _, n, _, cd_size, cd_off, _ = struct.unpack_from("<IHHHHIIH", buf, i)
    members = []
    p = cd_off
    for _ in range(n):
        sig, vmade, vneed, flags, method, mtime, mdate, crc, csize, size, nlen, elen, clen, \
            disk, iattr, eattr, lho = struct.unpack_from("<IHHHHHHIIIHHHHHII", buf, p)
        if sig != 0x02014B50:
            raise ValueError("bad central directory entry")
        name = buf[p + 46:p + 46 + nlen].decode("utf8", "replace")
        m = Member(name)
        m.size, m.csize, m.crc, m.method, m.flags = size, csize, crc, method, flags
        p += 46 + nlen + elen + clen
        # local header
        lsig, _, _, _, _, _, _, _, _, lnlen, lelen = struct.unpack_from("<IHHHHHIIIHH", buf, lho)
        extra = buf[lho + 30 + lnlen:lho + 30 + lnlen + lelen]
        raw = buf[lho + 30 + lnlen + lelen:lho + 30 + lnlen + lelen + csize]
        if method == 99:
            q = 0
            while q + 4 <= len(extra):
                hid, hl = struct.unpack_from("<HH", extra, q)
                if hid == 0x9901:
                    m.aes_version, vendor, m.strength, m.actual = struct.unpack_from("<H2sBH", extra, q + 4)
                q += 4 + hl
            if m.strength != 3:
                raise ValueError("%s: only AES-256 supported" % name)
            salt, ver, body, mac = raw[:16], raw[16:18], raw[18:-10], raw[-10:]
            dk = hashlib.pbkdf2_hmac("sha1", password, salt, 1000, 66)
            ekey, akey, pv = dk[:32], dk[32:64], dk[64:66]
            m.verifier_ok = pv == ver
            m.hmac_ok = hmac.new(akey, body, hashlib.sha1).digest()[:10] == mac
            plain = aes_ctr_winzip(ekey, body)
        else:
            m.actual = method
            plain = raw
        if m.actual == 8:
            plain = zlib.decompressobj(-15).decompress(plain)
        m.data = plain
        # AE-2 stores CRC 0 (small files may also use 0 in AE-1 style); only check when non-zero
        m.crc_ok = None if (method == 99 and m.aes_version == 2) or crc == 0 else (zlib.crc32(plain) & 0xFFFFFFFF) == crc
        members.append(m)
    return members


# --------------------------------------------------------------------------
# PS1 memory card
# --------------------------------------------------------------------------

BLOCK = 0x2000
CC_PRODUCT = "SLPSP02364"            # JP product code used by RDE (also SLUSP01041 for the US disc)


class DirEntry:
    def __init__(self, idx, frame):
        self.idx = idx                     # 1..15 (== its first block number)
        self.state, self.size, self.next = struct.unpack_from("<IIH", frame, 0)
        self.name = frame[10:10 + 20].split(b"\0")[0].decode("ascii", "replace")
        self.xor_ok = _xor(frame[:127]) == frame[127]

    @property
    def used(self):
        return (self.state & 0xF0) == 0x50

    @property
    def first(self):
        return self.state == 0x51


def _xor(b):
    x = 0
    for v in b:
        x ^= v
    return x


class Card:
    def __init__(self, data):
        if len(data) != 0x20000:
            raise ValueError("memory card must be 0x20000 bytes, got %#x" % len(data))
        self.data = bytes(data)
        self.header_ok = data[:2] == b"MC" and _xor(data[:127]) == data[127]
        self.dir = [DirEntry(i, data[0x80 * i:0x80 * i + 0x80]) for i in range(1, 16)]

    def block(self, n):
        return self.data[BLOCK * n:BLOCK * (n + 1)]

    def chain(self, entry):
        out, e, seen = [entry.idx], entry, set()
        while e.next != 0xFFFF and e.next < 15 and e.idx not in seen:
            seen.add(e.idx)
            e = self.dir[e.next]
            out.append(e.idx)
        return out

    def cc_slots(self):
        """{slot_number: (dir entry, block bytes)} for CHRONOnn saves (first block of each file)."""
        res = {}
        for e in self.dir:
            if e.first and "CHRONO" in e.name and (CC_PRODUCT in e.name or "SLUSP01041" in e.name):
                try:
                    n = int(e.name[-2:])
                except ValueError:
                    continue
                res[n] = (e, self.block(e.idx))
        return res


def load_card(path, cardno=1):
    raw = open(path, "rb").read()
    if raw[:2] == b"PK":
        for m in read_zip(raw):
            if m.name == "memcard%d.mcr" % cardno:
                return Card(m.data)
        raise ValueError("no memcard%d.mcr" % cardno)
    return Card(raw)


# --------------------------------------------------------------------------
# CC block: checksum, bit-packed section codec
# --------------------------------------------------------------------------

PACK_OFF, PACK_END = 0x5AC, 0x1F21       # compressed region inside the 0x2000 block
PACK_LEN = PACK_END - PACK_OFF            # 0x1975
IMG_SIZE = 0xE1F4
RAM_BASE = 0x800641A4                     # image offset 0 == guest RAM 0x800641A4 (derived from the (0x6ec78) hints)


def block_sum(blk):
    return sum(blk[:0x1FF8]) & 0xFFFFFFFF


def block_checksum_ok(blk):
    """The loader's test: bytes 0x180..0x1FF7, plus the header frame 0x000..0x17F when the version at 0x1FF8 is above 0x102."""
    start = 0 if struct.unpack_from("<I", blk, 0x1FF8)[0] > 0x102 else 0x180
    return struct.unpack_from("<I", blk, 0x1FFC)[0] == sum(blk[start:0x1FF8]) & 0xFFFFFFFF


# step = (mode, length, count, offset, special)
STEPS = [
    (0, 9, 0x180, 0xD13C, 2),      # items (u16 ids)
    (0, 7, 0x180, 0xD43C, 1),      # item quantities (u8)
    (0, 9, 0x0C0, 0xD5BC, 2),      # elements (u16 ids)
    (0, 7, 0x0C0, 0xD73C, 1),      # element quantities
    (1, 3, 0x02C, 0xDD98, 0x30),   # equipped element quantities, 4-bit nibbles, 0x18 bytes/char
    (0, 8, 0x02C, 0xAAD4, 0xCC),   # status bits
    (0, 8, 0x02C, 0xAA26, 0xCC),   # record+2 (unknown)
    (2, 9, 0x02C, 0xAA64, 2),      # grid cells: 48 x u16 per char
    (0, 7, 0x02C, 0xAA3C, 0xCC),   # ATK
    (0, 7, 0x02C, 0xAAE4, 0xCC),
    (0, 7, 0x02C, 0xAAE5, 0xCC),
    (0, 7, 0x02C, 0xAA3E, 0xCC),   # HIT
    (0, 7, 0x02C, 0xAAE6, 0xCC),
    (0, 7, 0x02C, 0xAAE7, 0xCC),
    (0, 7, 0x02C, 0xAA3D, 0xCC),   # DEF
    (0, 7, 0x02C, 0xAAE8, 0xCC),
    (0, 7, 0x02C, 0xAAE9, 0xCC),
    (0, 7, 0x02C, 0xAA3F, 0xCC),   # EVD
    (0, 7, 0x02C, 0xAAEA, 0xCC),
    (0, 7, 0x02C, 0xAAEB, 0xCC),
    (0, 7, 0x02C, 0xAA40, 0xCC),   # MAG
    (0, 7, 0x02C, 0xAAEC, 0xCC),
    (0, 7, 0x02C, 0xAAED, 0xCC),
    (0, 7, 0x02C, 0xAA41, 0xCC),   # MDEF
    (3, 10, 0x02C, 0xDAD8, 0),     # unlocked element levels: 8 x u16 per char, 0x10 bytes/char
    (0, 10, 0x02C, 0xAADC, 0xCC),  # current HP
    (0, 10, 0x02C, 0xAA3A, 0xCC),  # max HP
    (0, 10, 0x02C, 0xAAE0, 0xCC),
    (0, 9, 0x02C, 0xAA44, 0xCC),   # weapon
    (0, 9, 0x02C, 0xAA46, 0xCC),   # armour
    (0, 9, 0x02C, 0xAA48, 0xCC),   # accessory 1
    (0, 9, 0x02C, 0xAA4A, 0xCC),   # accessory 2
    (0, 9, 0x02C, 0xAA4C, 0xCC),   # accessory 3
    (0, 9, 0x02C, 0xAA51, 0xCC),   # character model id
    (0, 10, 0x02C, 0xAAE2, 0xCC),
    (0, 7, 0x02C, 0xAAEE, 0xCC),
    (0, 7, 0x02C, 0xAAEF, 0xCC),
    (4, 11, 0x02C, 0xD80C, 0),     # names: 6 symbols x 11 bits, 0x10 bytes/char
    (0, 0x20, 1, 0xE1B8, 1),
    (0, 0x20, 1, 0xE1BC, 1),
    (0, 0x20, 1, 0xE1C0, 1),
    (0, 0x20, 1, 0xE1C4, 1),
    (0, 0x20, 1, 0x0000, 1),       # button config: field
    (0, 0x20, 1, 0x0004, 1),       # battle
    (0, 0x20, 1, 0x0008, 1),       # field analog
    (0, 0x20, 1, 0x000C, 1),       # battle analog
    (0, 0x20, 1, 0xE1E0, 1),
    (0, 0x20, 1, 0xE1E4, 1),
    (0, 8, 0x02C, 0xAACC, 0xCC),
    (0, 0x10, 1, 0xE1E8, 1),       # obtained frames bitfield
    (0, 8, 1, 0xAA25, 1),
    (0, 8, 1, 0xCC69, 1),
    (0, 0x20, 1, 0xE1EC, 1),
    (0, 0x20, 1, 0xE1F0, 1),
]


def _dt(length):
    return 4 if length == 0x20 else 2 if length > 8 else 1


class _BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def get(self, n):
        # LSB-first bit stream
        v = 0
        for k in range(n):
            b = self.pos + k
            if self.data[b >> 3] >> (b & 7) & 1:
                v |= 1 << k
        self.pos += n
        return v


class _BitWriter:
    def __init__(self, nbytes):
        self.buf = bytearray(nbytes)
        self.pos = 0

    def put(self, v, n):
        for k in range(n):
            if v >> k & 1:
                b = self.pos + k
                self.buf[b >> 3] |= 1 << (b & 7)
        self.pos += n


def _put(img, off, size, v):
    img[off:off + size] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")


def decompress(blk):
    """0x2000 CC block -> 0xE1F4 image.  Mirrors the reference decompressor."""
    br = _BitReader(blk[PACK_OFF:PACK_END])
    img = bytearray(IMG_SIZE)
    for mode, length, count, off, special in STEPS:
        if mode == 0:
            sz = _dt(length)
            for i in range(count):
                _put(img, off + i * special, sz, br.get(length))
        elif mode == 1:
            for c in range(count):
                for j in range(special):
                    v = br.get(length)
                    o = off + c * 0x18 + (j >> 1)
                    img[o] |= (v << 4) if j & 1 else v
        elif mode == 2:
            for c in range(count):
                for k in range(0x30):
                    _put(img, off + c * 0xCC + k * special, 2, br.get(length))
        elif mode == 3:
            for c in range(count):
                for k in range(8):
                    _put(img, off + c * 0x10 + k * 2, 2, br.get(length))
        elif mode == 4:
            for c in range(count):
                o = off + c * 0x10
                for _ in range(6):
                    ch = br.get(length)
                    if ch & 0x700:
                        img[o] = ((ch & 0x700) >> 8) - 9 & 0xFF     # 1..7 -> 0xF8..0xFE prefix
                        o += 1
                    img[o] = ch & 0xFF
                    o += 1
                img[o] = 0
    return img


def compress(img):
    """0xE1F4 image -> 0x1975 packed bytes (inverse of decompress)."""
    bw = _BitWriter(PACK_LEN)
    for mode, length, count, off, special in STEPS:
        if mode == 0:
            sz = _dt(length)
            for i in range(count):
                o = off + i * special
                bw.put(int.from_bytes(img[o:o + sz], "little"), length)
        elif mode == 1:
            for c in range(count):
                for j in range(special):
                    b = img[off + c * 0x18 + (j >> 1)]
                    bw.put((b >> 4) if j & 1 else (b & 15), length)
        elif mode == 2:
            for c in range(count):
                for k in range(0x30):
                    o = off + c * 0xCC + k * special
                    bw.put(int.from_bytes(img[o:o + 2], "little"), length)
        elif mode == 3:
            for c in range(count):
                for k in range(8):
                    o = off + c * 0x10 + k * 2
                    bw.put(int.from_bytes(img[o:o + 2], "little"), length)
        elif mode == 4:
            for c in range(count):
                o = off + c * 0x10
                for _ in range(6):
                    ch = img[o]
                    o += 1
                    if ch > 0xF7:
                        ch = ((ch - 0xF7) << 8) | img[o]
                        o += 1
                    bw.put(ch, length)
    return bytes(bw.buf)


def pack_block(blk, img):
    """Return a new 0x2000 block: packed region replaced from img, checksum recomputed."""
    out = bytearray(blk)
    out[PACK_OFF:PACK_END] = compress(img)
    struct.pack_into("<I", out, 0x1FFC, block_sum(out))
    return bytes(out)


# --------------------------------------------------------------------------
# field decoding
# --------------------------------------------------------------------------

_JP_RUNS = [
    (0x75, " あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをんがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽぁぃぅぇぉっゃゅょ。「」#ー~⋯?!•0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
    (0xF5, "、"),
    (0xF800, "abcdefghijklmnopqrstuvwxyzアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲンガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポァィゥェォッャュョヴ"),
    (0xF86C, "蛇骨"),
    (0xF873, "星子夫改良"),
    (0xF88E, "龍種"),
]
JP = {}
for _a, _t in _JP_RUNS:
    for _i, _c in enumerate(_t):
        JP[_a + _i] = _c
US = {0x20: " "}
for _i, _c in enumerate("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"):
    US[0x28 + _i] = _c


def decode_text(raw, table=None):
    """Names/strings: bytes -> str.  0xF8..0xFE start a 2-byte code (F8 xx).  0x00 terminates."""
    if table is None:
        table = JP
    out, i = [], 0
    while i < len(raw):
        b = raw[i]
        if b == 0:
            break
        if 0xF8 <= b <= 0xFE and i + 1 < len(raw):
            code = (b << 8) | raw[i + 1]
            i += 2
        else:
            code = b
            i += 1
        out.append(table.get(code, "{%X}" % code))
    return "".join(out)


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


CHAR_NAMES = ["Serge", "Kid", "Guile", "Norris", "Nikki", "Viper", "Riddel", "Karsh", "Zoah", "Marcy", "Korcha",
              "Luccia", "Poshul", "Razzly", "Zappa", "Orcha", "Radius", "Fargo", "Macha", "Glenn", "Leena", "Miki",
              "Harle", "Janice", "Draggy", "Starky", "Sprigg", "Mojo", "Turnip", "NeoFio", "Greco", "Skelly",
              "Funguy", "Irenes", "Mel", "Leah", "Van", "Sneff", "Steena", "Doc", "Grobyc", "Pierre", "Orlha", "Pip"]


def preview(blk):
    """Raw (uncompressed) preview/state fields in the 0x2000 block."""
    return {
        "title_sjis": blk[4:0x44].split(b"\0")[0].decode("shift_jis", "replace"),
        "leader_name": blk[0x28:0x34].split(b"\0")[0].decode("shift_jis", "replace"),
        "leader_name_hex": blk[0x28:0x34].hex(),
        "party_ids": list(blk[0x34:0x37]),
        "playtime_frames": u32(blk, 0x37),
        "playtime": _hms(u32(blk, 0x37) // 60),
        "gold": u32(blk, 0x3B),
        "stars": blk[0x3F],
        "progression": u16(blk, 0x40),
        "new_game_plus": bool(blk[0x42] & 2),
        "location": u16(blk, 0x18E),
        "formation_ids": list(blk[0x59C:0x59F]),
        "formation_names": [CHAR_NAMES[x] if x < 44 else None for x in blk[0x59C:0x59F]],
        "playtime_frames_full": u32(blk, 0x588),
        "gold_full": u32(blk, 0x58C),
        "stars_total": blk[0x5A4],
        "stars_current": blk[0x5A5],
        "pos_x": u16(blk, 0x488),
        "pos_z": u16(blk, 0x48A),
        "recruited": [CHAR_NAMES[i] for i in range(44) if blk[0x590 + i // 8] >> (i % 8) & 1],
        "checksum_stored": "%08X" % u32(blk, 0x1FFC),
        "checksum_calc": "%08X" % block_sum(blk),
    }


def _hms(s):
    return "%d:%02d:%02d" % (s // 3600, s // 60 % 60, s % 60)


def character(img, i):
    b = 0xAA24 + i * 0xCC            # record base (stat record 0xB0 layout + 0x1C extension)
    r = lambda o: b + o
    return {
        "index": i,
        "name": decode_text(img[0xD80C + i * 0x10:0xD80C + i * 0x10 + 16]),
        "name_hex": img[0xD80C + i * 0x10:0xD80C + i * 0x10 + 16].hex(),
        "model_id": img[0xAA51 + i * 0xCC],
        "color_bit": img[0xAA25] if i == 0 else None,
        "hp": u16(img, 0xAADC + i * 0xCC), "max_hp": u16(img, 0xAA3A + i * 0xCC),
        "atk": img[0xAA3C + i * 0xCC], "def": img[0xAA3D + i * 0xCC], "hit": img[0xAA3E + i * 0xCC],
        "evd": img[0xAA3F + i * 0xCC], "mag": img[0xAA40 + i * 0xCC], "mdef": img[0xAA41 + i * 0xCC],
        "status_bits": img[0xAAD4 + i * 0xCC],
        "weapon": u16(img, 0xAA44 + i * 0xCC), "armor": u16(img, 0xAA46 + i * 0xCC),
        "accessories": [u16(img, 0xAA48 + i * 0xCC + 2 * k) for k in range(3)],
        "ext_AAE0_u16": u16(img, 0xAAE0 + i * 0xCC), "ext_AAE2_u16": u16(img, 0xAAE2 + i * 0xCC),
        "ext_bonus_bytes_AAE4": img[0xAAE4 + i * 0xCC:0xAAF0 + i * 0xCC].hex(),
        "rec_AA26": img[0xAA26 + i * 0xCC], "rec_AACC": img[0xAACC + i * 0xCC],
        "grid_cells": [u16(img, 0xAA64 + i * 0xCC + 2 * k) for k in range(48)],
        "unlocked_levels": [u16(img, 0xDAD8 + i * 0x10 + 2 * k) for k in range(8)],
        "equipped_quantities": [(img[0xDD98 + i * 0x18 + (j >> 1)] >> (4 * (j & 1))) & 15 for j in range(48)],
    }


def full_record(blk, with_image=False):
    img = decompress(blk)
    items = [(u16(img, 0xD13C + 2 * k), img[0xD43C + k]) for k in range(0x180)]
    elems = [(u16(img, 0xD5BC + 2 * k), img[0xD73C + k]) for k in range(0xC0)]
    d = {
        "preview": preview(blk),
        "party": {"formation": list(blk[0x59C:0x59F]), "recruited_bits": blk[0x590:0x596].hex()},
        "characters": [character(img, i) for i in range(44)],
        "items": [{"slot": k, "id": a, "qty": q} for k, (a, q) in enumerate(items) if a],
        "elements": [{"slot": k, "id": a, "qty": q} for k, (a, q) in enumerate(elems) if a],
        "key_items_bits_0x180": blk[0x180:0x188].hex(),
        "button_config": {n: "%08X" % u32(img, o) for n, o in
                          (("field", 0), ("battle", 4), ("field_analog", 8), ("battle_analog", 12))},
        "obtained_frames_u16": u16(img, 0xE1E8),
        "pip_color_stats": {n: u16(blk, 0x55C + 2 * k) for k, n in
                            enumerate(("green", "white", "black", "red", "yellow", "blue"))},
        "options_0x5A0": blk[0x5A0:0x5A2].hex(),
        "misc_image_u32": {("%04X" % o): "%08X" % u32(img, o) for o in (0xE1B8, 0xE1BC, 0xE1C0, 0xE1C4, 0xE1E0, 0xE1E4, 0xE1EC, 0xE1F0)},
        "misc_image_u8": {"CC69": img[0xCC69]},
        "raw_head_hex_0x000_0x5AC": blk[:PACK_OFF].hex(),
        "raw_tail_hex_0x1F21_0x1FFC": blk[PACK_END:0x2000].hex(),
    }
    if with_image:
        d["image_hex"] = img.hex()
    return d


# --------------------------------------------------------------------------
# ext_save.dat
# --------------------------------------------------------------------------

def parse_ext(data):
    magic, ver = struct.unpack_from("<4sI", data, 0)
    out = {"size": len(data), "magic": magic.rstrip(bytes(1)).decode("ascii", "replace"), "version": hex(ver), "slots": []}
    if magic != b"EXT\0" or len(data) not in (0xA634, 0xA5F8):
        out["error"] = "unexpected size/magic"
        return out
    for s in range(30):
        o = 8 + s * 0x588
        lang, bc = struct.unpack_from("<II", data, o)
        names = []
        for k in range(44):
            raw = data[o + 8 + k * 32:o + 8 + k * 32 + 32]
            t = raw.split(b"\0")[0].decode("utf8", "replace")
            if t:
                names.append((k, t))
        flags = None
        if len(data) == 0xA634:
            flags = (data[0xA5F8 + 2 * s], data[0xA5F8 + 2 * s + 1])
        if lang or bc or names or (flags and any(flags)):
            out["slots"].append({"slot": s, "card": s // 15 + 1, "block": s % 15, "language": lang,
                                 "battle_counter": bc, "custom_names": names, "form_byte_valid": flags})
    return out


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _members(path):
    return {m.name: m for m in read_zip(path)}


def _card_args(argv):
    cardno = 1
    if "--card" in argv:
        k = argv.index("--card")
        cardno = int(argv[k + 1])
        del argv[k:k + 2]
    return cardno


def cmd_info(argv):
    for m in read_zip(argv[0]):
        print("%-16s size=%-7d csize=%-6d method=%d/%d aes_v=%d verifier=%s hmac=%s crc=%s" % (
            m.name, m.size, m.csize, m.method, m.actual, m.aes_version, m.verifier_ok, m.hmac_ok, m.crc_ok))
        if len(m.data) != m.size:
            print("   !! decoded length %d != declared %d" % (len(m.data), m.size))


def cmd_slots(argv):
    cardno = _card_args(argv)
    card = load_card(argv[0], cardno)
    print("card %d: header %s" % (cardno, "ok" if card.header_ok else "BAD"))
    for e in card.dir:
        print("  dir %2d state=%02X size=%#07x next=%04X xor=%s name=%r" % (
            e.idx, e.state, e.size, e.next, "ok" if e.xor_ok else "BAD", e.name))
    for n, (e, blk) in sorted(card.cc_slots().items()):
        p = preview(blk)
        print("slot %02d (block %d) title=%r leader=%r playtime=%s gold=%d stars=%d/%d loc=%#x party=%s cksum=%s/%s" % (
            n, e.idx, p["title_sjis"], p["leader_name"], p["playtime"], p["gold_full"], p["stars_current"],
            p["stars_total"], p["location"], p["formation_names"], p["checksum_stored"], p["checksum_calc"]))


def cmd_dump(argv):
    cardno = _card_args(argv)
    with_image = "--image" in argv
    argv = [a for a in argv if a != "--image"]
    card = load_card(argv[0], cardno)
    slot = int(argv[1])
    sl = card.cc_slots()
    if slot not in sl:
        raise SystemExit("slot %d not present (have %s)" % (slot, sorted(sl)))
    d = full_record(sl[slot][1], with_image)
    with open(argv[2], "w", encoding="utf8") as f:
        json.dump(d, f, ensure_ascii=False, indent=1)
    print("wrote", argv[2])


def cmd_checksum(argv):
    cardno = _card_args(argv)
    card = load_card(argv[0], cardno)
    allok = True
    for n, (e, blk) in sorted(card.cc_slots().items()):
        ck = block_checksum_ok(blk)
        img = decompress(blk)
        rt = compress(img) == blk[PACK_OFF:PACK_END]
        rb = pack_block(blk, img) == blk
        allok &= ck and rt and rb
        print("slot %02d block %2d checksum %s  recompress==original %s  repacked block==original %s" % (
            n, e.idx, "ok" if ck else "BAD", rt, rb))
    print("ALL OK" if allok else "MISMATCH")


def cmd_ext(argv):
    m = _members(argv[0]).get("ext_save.dat")
    if not m:
        raise SystemExit("no ext_save.dat")
    print(json.dumps(parse_ext(m.data), ensure_ascii=False, indent=1, default=str))


def main(argv):
    cmds = {"info": cmd_info, "slots": cmd_slots, "dump": cmd_dump, "checksum": cmd_checksum, "ext": cmd_ext}
    if len(argv) < 2 or argv[1] not in cmds:
        print(__doc__)
        return 2
    sys.stdout.reconfigure(encoding="utf8")
    cmds[argv[1]](argv[2:])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
