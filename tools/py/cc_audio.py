"""Chrono Cross audio formats: SEAD .sab (HCA header only), AKAO music / instrument
bank / SFX program, PS1 SPU ADPCM.  Stdlib only.

Usage (python -I tools/py/cc_audio.py [--game DIR] <command>):
  sab   info  <file>            SEAD container + HCA header fields
  music info  <file> [--no-ops] AKAO music header, channels, opcode listing
  wave  info  <file>            instrument bank (SPU samples, ADSR, base note)
  wave  export <file> <outdir>  decode every sample to 16-bit WAV
  sfx   info  <file|room>       effect00.dat style SFX blob / room yos1+yos0
  ops                           print the opcode table
  stats                         parse everything in cdrom.dat, failure table
<file> is a path on disk or a member name inside <game>/data/cdrom.dat.
"""
import argparse
import collections
import csv
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cc_container  # noqa: E402


class AudioError(Exception):
    pass


# ----------------------------------------------------------------- file access
def default_game_dir():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", ".."))


class Game:
    def __init__(self, game_dir):
        self.dir = game_dir
        self.zip = zipfile.ZipFile(os.path.join(game_dir, "data", "cdrom.dat"))
        self.names = [i.filename for i in self.zip.infolist() if not i.filename.endswith("/")]

    def read(self, name):
        return self.zip.read(name)

    def glob(self, prefix, suffix=""):
        return sorted(n for n in self.names if n.startswith(prefix) and n.endswith(suffix))


def load_arg(path, game_dir):
    if os.path.exists(path):
        with open(path, "rb") as f:
            return f.read(), path
    g = Game(game_dir)
    for cand in (path, path.replace("\\", "/"), "sound/" + path):
        if cand in g.names:
            return g.read(cand), cand
    raise AudioError("not found on disk or in cdrom.dat: " + path)


# ----------------------------------------------------------------- HCA / SAB
def crc16_hca(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def parse_hca_header(d, off=0):
    """Big-endian HCA header (no decoding).  Returns dict.  data_offset = header size."""
    if d[off:off + 4] & b"\x7f\x7f\x7f\x7f" if False else (bytes(x & 0x7F for x in d[off:off + 4]) != b"HCA\0"):
        raise AudioError("no HCA magic")
    version, hsize = struct.unpack_from(">HH", d, off + 4)
    h = {"version": "%d.%d" % (version >> 8, version & 255), "header_size": hsize}
    p = off + 8
    end = off + hsize
    h["crc_ok"] = crc16_hca(d[off:end]) == 0
    h["ciph"] = 0
    while p + 4 <= end:
        tag = bytes(x & 0x7F for x in d[p:p + 4])
        if tag == b"fmt\0":
            ch = d[p + 4]
            rate = int.from_bytes(d[p + 5:p + 8], "big")
            frames, mute_h, mute_f = struct.unpack_from(">IHH", d, p + 8)
            h.update(channels=ch, sample_rate=rate, frame_count=frames, mute_header=mute_h,
                     mute_footer=mute_f)
            p += 16
        elif tag == b"comp":
            bs, minr, maxr, trk, cfg, tot, base, st, hfr = struct.unpack_from(">HBBBBBBBB", d, p + 4)
            h.update(block_size=bs, min_res=minr, max_res=maxr, track_count=trk, channel_config=cfg,
                     total_bands=tot, base_bands=base, stereo_bands=st, hfr_groups=hfr)
            p += 16
        elif tag == b"dec\0":
            bs, minr, maxr, cnt, c2, trk_sel = struct.unpack_from(">HBBBBB", d, p + 4)
            h.update(block_size=bs, min_res=minr, max_res=maxr, dec_count=cnt)
            p += 12
        elif tag == b"loop":
            ls, le, a, b = struct.unpack_from(">IIHH", d, p + 4)
            h.update(loop_start_frame=ls, loop_end_frame=le, loop_r01=a, loop_r02=b)
            p += 16
        elif tag == b"ciph":
            h["ciph"] = struct.unpack_from(">H", d, p + 4)[0]
            p += 6
        elif tag == b"vbr\0":
            p += 8
        elif tag == b"athr":
            p += 6
        elif tag == b"rva\0":
            h["rva"] = struct.unpack_from(">f", d, p + 4)[0]
            p += 8
        elif tag == b"comm":
            n = d[p + 4]
            h["comment"] = d[p + 5:p + 5 + n].decode("latin1")
            p += 5 + n
        elif tag == b"pad\0":
            break
        else:
            break
    if "frame_count" in h:
        h["total_samples"] = h["frame_count"] * 1024 - h["mute_header"] - h["mute_footer"]
    return h


def _cstr(b):
    return b.split(b"\0")[0].decode("latin1")


def parse_sab(d):
    """SEAD 'sabf' container.  Returns dict with header, sections, materials."""
    if d[:4] != b"sabf":
        raise AudioError("not sabf")
    s = {"version": d[4], "flags": d[5], "hdr_size": struct.unpack_from("<H", d, 6)[0],
         "file_size": struct.unpack_from("<I", d, 12)[0], "name": _cstr(d[16:32]), "sections": {}}
    if s["file_size"] != len(d):
        raise AudioError("sabf size field %d != file size %d" % (s["file_size"], len(d)))
    p = 0x20
    while p + 16 <= len(d) and d[p:p + 4] in (b"snd ", b"seq ", b"trk ", b"mtrl"):
        tag = d[p:p + 4].decode()
        off = struct.unpack_from("<I", d, p + 8)[0]
        cnt = struct.unpack_from("<I", d, off + 4)[0]
        offs = [struct.unpack_from("<I", d, off + 0x10 + 4 * i)[0] + off for i in range(cnt)]
        s["sections"][tag] = {"offset": off, "count": cnt, "entries": offs}
        p += 16
    if "mtrl" not in s["sections"]:
        raise AudioError("no mtrl section")
    mats = []
    for e in s["sections"]["mtrl"]["entries"]:
        m = {"offset": e, "channels": d[e + 4], "codec": d[e + 5]}
        (m["sample_rate"], m["loop_start"], m["loop_end"], m["extra_size"], m["stream_size"],
         m["sound_id"]) = struct.unpack_from("<6I", d, e + 8)
        m["extra_offset"] = e + 0x20
        m["data_offset"] = e + 0x20 + m["extra_size"]
        if m["codec"] == 7:
            x = d[e + 0x20:e + 0x30]
            m["extra"] = {"hca_hdr_size": struct.unpack_from("<H", x, 2)[0],
                          "block_size": struct.unpack_from("<H", x, 4)[0],
                          "loop_start_frame": struct.unpack_from("<H", x, 6)[0],
                          "loop_end_frame": struct.unpack_from("<H", x, 8)[0]}
            m["hca"] = parse_hca_header(d, e + 0x30)
            h = m["hca"]
            m["frames_in_file"] = (len(d) - m["data_offset"]) / float(h["block_size"])
        mats.append(m)
    s["materials"] = mats
    for tag in ("snd ", "seq ", "trk "):
        sec = s["sections"].get(tag)
        if sec:
            sec["entry_types"] = [d[o] for o in sec["entries"]]
    # snd entry: duration seconds float at +0x24 (observed)
    if "snd " in s["sections"] and s["sections"]["snd "]["entries"]:
        o = s["sections"]["snd "]["entries"][0]
        s["duration_s"] = struct.unpack_from("<f", d, o + 0x24)[0]
        s["snd_name"] = _cstr(d[o + (struct.unpack_from("<H", d, o + 2)[0]):o + 0x80])
    return s


def cmd_sab_info(d, label, out):
    s = parse_sab(d)
    out("%s: sabf v%d name=%r size=%d sections=%s" % (label, s["version"], s["name"], s["file_size"],
                                                      {k: (v["offset"], v["count"]) for k, v in s["sections"].items()}))
    if "duration_s" in s:
        out("  snd[0] name=%r duration=%.3fs" % (s.get("snd_name"), s["duration_s"]))
    for i, m in enumerate(s["materials"]):
        out("  mtrl[%d] @0x%X ch=%d codec=%d rate=%d loop=%d..%d (samples) extra=%d stream=%d id=0x%X"
            % (i, m["offset"], m["channels"], m["codec"], m["sample_rate"], m["loop_start"], m["loop_end"],
               m["extra_size"], m["stream_size"], m["sound_id"]))
        if "hca" in m:
            h = m["hca"]
            out("    extra: %s" % m["extra"])
            out("    HCA v%s hdr=%d crc_ok=%s ch=%d rate=%d frames=%d mute=%d/%d block=%d ciph=%d "
                "res=%d..%d trk=%d cfg=%d bands=%d/%d/%d hfr=%d" % (
                    h["version"], h["header_size"], h["crc_ok"], h["channels"], h["sample_rate"], h["frame_count"],
                    h["mute_header"], h["mute_footer"], h["block_size"], h["ciph"], h["min_res"], h["max_res"],
                    h["track_count"], h["channel_config"], h["total_bands"], h["base_bands"], h["stereo_bands"],
                    h["hfr_groups"]))
            out("    total_samples=%d loop frames %s..%s frames_in_file=%.3f" % (
                h["total_samples"], h.get("loop_start_frame"), h.get("loop_end_frame"), m["frames_in_file"]))
    return s


# ----------------------------------------------------------------- AKAO opcodes
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# ticks per length index, 192 ticks = whole note
NOTE_LENGTHS = [192, 96, 48, 24, 12, 6, 3, 32, 16, 8, 4]
# arg kinds: u=u8 s=s8 w=u16 r=s16 relative (target = address of the field + value)
OPS = {
    0xA0: ("FinishChannel", ""), 0xA1: ("LoadInstrument", "u"), 0xA2: ("NextNoteLength", "u"),
    0xA3: ("ChannelMasterVolume", "u"), 0xA4: ("PitchBendSlide", "us"), 0xA5: ("SetOctave", "u"),
    0xA6: ("OctaveUp", ""), 0xA7: ("OctaveDown", ""), 0xA8: ("ChannelVolume", "u"),
    0xA9: ("ChannelVolumeSlide", "uu"), 0xAA: ("ChannelPan", "u"), 0xAB: ("ChannelPanSlide", "uu"),
    0xAC: ("NoiseClock", "u"), 0xAD: ("AttackRate", "u"), 0xAE: ("DecayRate", "u"),
    0xAF: ("SustainLevel", "u"), 0xB0: ("DecayRateSustainLevel", "uu"), 0xB1: ("SustainRate", "u"),
    0xB2: ("ReleaseRate", "u"), 0xB3: ("ResetAdsr", ""), 0xB4: ("Vibrato", "uuu"),
    0xB5: ("VibratoDepth", "u"), 0xB6: ("VibratoOff", ""), 0xB7: ("AttackMode", "u"),
    0xB8: ("Tremolo", "uuu"), 0xB9: ("TremoloDepth", "u"), 0xBA: ("TremoloOff", ""),
    0xBB: ("SustainMode", "u"), 0xBC: ("AutoPan", "uu"), 0xBD: ("AutoPanDepth", "u"),
    0xBE: ("AutoPanOff", ""), 0xBF: ("ReleaseMode", "u"), 0xC0: ("TransposeAbs", "s"),
    0xC1: ("TransposeRel", "s"), 0xC2: ("ReverbOn", ""), 0xC3: ("ReverbOff", ""),
    0xC4: ("NoiseOn", ""), 0xC5: ("NoiseOff", ""), 0xC6: ("FmOn", ""), 0xC7: ("FmOff", ""),
    0xC8: ("LoopPoint", ""), 0xC9: ("LoopN", "u"), 0xCA: ("LoopInfinite", ""),
    0xCB: ("ResetVoiceModes", ""), 0xCC: ("LegatoOn", ""), 0xCD: ("Nop_CD", ""),
    0xCE: ("NoiseOnDelayToggle", "u"), 0xCF: ("NoiseToggleDelay", "u"), 0xD0: ("FullLengthNoteOn", ""),
    0xD1: ("Nop_D1", ""), 0xD2: ("FmOnDelayToggle", "u"), 0xD3: ("FmToggleDelay", "u"),
    0xD4: ("RateSidechainOn", ""), 0xD5: ("RateSidechainOff", ""), 0xD6: ("PitchVolSidechainOn", ""),
    0xD7: ("PitchVolSidechainOff", ""), 0xD8: ("FineTuneAbs", "s"), 0xD9: ("FineTuneRel", "s"),
    0xDA: ("PortamentoOn", "u"), 0xDB: ("PortamentoOff", ""), 0xDC: ("FixNoteLength", "s"),
    0xDD: ("VibratoDepthSlide", "uu"), 0xDE: ("TremoloDepthSlide", "uu"), 0xDF: ("AutoPanDepthSlide", "uu"),
    0xE0: ("Flag_E0", ""), 0xE1: ("RandomPitchDepth", "u"), 0xE2: ("RandomPitchOff", ""),
    0xE4: ("VibratoRateSlide", "uu"), 0xE5: ("TremoloRateSlide", "uu"), 0xE6: ("AutoPanRateSlide", "uu"),
}
FE_OPS = {
    0x00: ("Tempo", "w"), 0x01: ("TempoSlide", "uw"), 0x02: ("ReverbDepth", "w"),
    0x03: ("ReverbDepthSlide", "uw"), 0x04: ("DrumModeOn", ""), 0x05: ("DrumModeOff", ""),
    0x06: ("Jump", "r"), 0x07: ("JumpIfThreshold", "ur"), 0x08: ("JumpOnNthPass", "ur"),
    0x09: ("JumpOnNthPassPop", "ur"), 0x0A: ("LoadInstrumentNoAttack", "u"), 0x0B: ("TriggerSfx", "u"),
    0x0E: ("Call", "r"), 0x0F: ("Return", ""), 0x10: ("ReserveVoices", "u"), 0x11: ("FreeVoices", ""),
    0x12: ("ChannelMasterVolumeSlide", "uu"), 0x13: ("NoRekeyOnResume", ""), 0x14: ("LoadPatch", "u"),
    0x15: ("TimeSignature", "uu"), 0x16: ("MeasureNumber", "w"), 0x19: ("KeyOnVolumeSlide", "sus"),
    0x1A: ("TenutoOn", ""), 0x1B: ("TenutoOff", ""), 0x1C: ("SkipByte", "u"),
    0x1D: ("UseReservedVoices", ""), 0x1E: ("NoReservedVoices", ""),
}
ARGSZ = {"u": 1, "s": 1, "w": 2, "r": 2}


def decode_op(code, p, base=0):
    """Decode one instruction at code[p].  Returns (length, mnemonic_with_args, key, info) where key is
    a histogram key.  Raises IndexError if truncated."""
    op = code[p]
    if op < 0x84:
        return 1, "note %s len=%d" % (NOTE_NAMES[op // 11], NOTE_LENGTHS[op % 11]), "NOTE", None
    if op < 0x8F:
        return 1, "tie len=%d" % NOTE_LENGTHS[op - 0x84], "TIE", None
    if op < 0x9A:
        return 1, "rest len=%d" % NOTE_LENGTHS[op - 0x8F], "REST", None
    if op < 0xA0:
        return 1, "invalid_%02X" % op, "BAD", "invalid"
    if 0xF0 <= op <= 0xFD:
        n = code[p + 1]
        kind = "note %s" % NOTE_NAMES[op - 0xF0] if op < 0xFC else ("tie" if op == 0xFC else "rest")
        return 2, "%s len=%d(explicit)" % (kind, n), "NOTE_EXPLICIT", None
    if op == 0xFF:
        return 1, "FinishChannel(FF)", "FF", None
    if op == 0xFE:
        sub = code[p + 1]
        ent = FE_OPS.get(sub)
        if ent is None:
            return 2, "FE %02X (unimplemented, finishes channel)" % sub, "FE%02X" % sub, "unimplemented"
        name, fmt, q = ent[0], ent[1], p + 2
        return _args(code, p, q, name, fmt, "FE%02X" % sub, 2)
    ent = OPS.get(op)
    if ent is None:
        return 1, "op_%02X (unimplemented, finishes channel)" % op, "%02X" % op, "unimplemented"
    return _args(code, p, p + 1, ent[0], ent[1], "%02X" % op, 1)


def _args(code, p, q, name, fmt, key, hdr):
    vals = []
    for k in fmt:
        if q + ARGSZ[k] > len(code):
            raise IndexError
        if k == "u":
            vals.append(str(code[q]))
        elif k == "s":
            vals.append(str(struct.unpack_from("<b", code, q)[0]))
        elif k == "w":
            vals.append("0x%04X" % struct.unpack_from("<H", code, q)[0])
        else:
            rel = struct.unpack_from("<h", code, q)[0]
            vals.append("->0x%X" % (q + rel))
        q += ARGSZ[k]
    return q - p, "%s %s" % (name, " ".join(vals)) if vals else name, key, None


def decode_span(code, start, end, hist=None, listing=None, errors=None, tail_ok=False):
    p = start
    while p < end:
        try:
            n, txt, key, bad = decode_op(code, p)
        except IndexError:
            if errors is not None and not tail_ok:
                errors.append("truncated instruction at 0x%X" % p)
            return
        if p + n > end:
            if errors is not None and not tail_ok:
                errors.append("instruction at 0x%X crosses span end 0x%X" % (p, end))
            return
        if hist is not None:
            hist[key] += 1
        if bad and errors is not None:
            errors.append("%s at 0x%X" % (bad, p))
        if listing is not None:
            listing.append((p, code[p:p + n], txt))
        p += n


def print_optable(out):
    out("op   args  name")
    out("00-83 0    note: key=op//11 (+octave*12), length=NOTE_LENGTHS[op%%11] %s" % (NOTE_LENGTHS,))
    out("84-8E 0    tie   (length index op-0x84)")
    out("8F-99 0    rest  (length index op-0x8F)")
    out("F0-FB 1    note with explicit length byte; FC tie, FD rest (same, explicit length)")
    for op in sorted(OPS):
        out("%02X   %-5s %s" % (op, OPS[op][1] or "-", OPS[op][0]))
    for sub in sorted(FE_OPS):
        out("FE%02X %-5s %s" % (sub, FE_OPS[sub][1] or "-", FE_OPS[sub][0]))
    out("others (E3, E7-EF, FF, FE0C/0D/17/18/1F+): finish the channel")


# ----------------------------------------------------------------- AKAO music
def akao_header(d):
    if len(d) < 0x40 or d[:4] != b"AKAO":
        raise AudioError("no AKAO magic")
    return {"id": struct.unpack_from("<H", d, 4)[0], "length": struct.unpack_from("<H", d, 6)[0]}


def parse_music(d):
    h = akao_header(d)
    w = struct.unpack_from("<16I", d, 0)
    h.update(field08=w[2], reverb_or_vol=w[4], bank_id=w[5], channel_mask=w[8], keyed_mask=w[9],
             alloc_mask=w[10], patch_rel=w[12], keymap_rel=w[13])
    h["patch_abs"] = 0x30 + w[12] if w[12] else 0
    h["keymap_abs"] = 0x34 + w[13] if w[13] else 0
    if h["length"] != len(d) and h["length"] != (len(d) & 0xFFFF):
        raise AudioError("length field 0x%X != file size %d" % (h["length"], len(d)))
    chans = [i for i in range(32) if h["channel_mask"] >> i & 1]
    p = 0x40
    starts = []
    for c in chans:
        if p + 2 > len(d):
            raise AudioError("channel table past EOF")
        off = struct.unpack_from("<H", d, p)[0]
        starts.append((c, p + off))
        p += 2
    h["channels"] = starts
    h["code_start"] = p
    for c, s in starts:
        if not (p <= s < len(d)):
            raise AudioError("channel %d start 0x%X outside code" % (c, s))
    h["tail"] = min([x for x in (h["patch_abs"], h["keymap_abs"]) if x] + [len(d)])
    return h


def music_spans(h):
    st = sorted(set(s for _, s in h["channels"]))
    spans = []
    for i, s in enumerate(st):
        e = st[i + 1] if i + 1 < len(st) else h["tail"]
        spans.append((s, e))
    return spans


def cmd_music_info(d, label, out, ops=True):
    h = parse_music(d)
    out("%s: AKAO music id=%d length=%d bank_id(wave)=%d (wave%04d.dat) +0x10=0x%X +0x08=%d" % (
        label, h["id"], h["length"], h["bank_id"], h["bank_id"], h["reverb_or_vol"], h["field08"]))
    out("  channel mask 0x%08X (%d channels) keyed=0x%X alloc=0x%X patch@0x%X keymap@0x%X" % (
        h["channel_mask"], len(h["channels"]), h["keyed_mask"], h["alloc_mask"], h["patch_abs"], h["keymap_abs"]))
    if h["patch_abs"]:
        pt = struct.unpack_from("<16H", d, h["patch_abs"])
        out("  patch table (u16[16], 0x8000+ = none): %s" % " ".join("%04X" % x for x in pt))
    hist = collections.Counter()
    errs = []
    lst = {}
    for s, e in music_spans(h):
        L = []
        decode_span(d, s, e, hist, L, errs)
        lst[s] = L
    insts = sorted(set(int(t.split()[1]) for L in lst.values() for _, _, t in L if t.startswith("LoadInstrument ")))
    out("  instruments referenced (A1): %s" % insts)
    for c, s in h["channels"]:
        out("  channel %2d  start 0x%04X" % (c, s))
    if ops:
        for s, e in music_spans(h):
            out("  --- code span 0x%04X..0x%04X" % (s, e))
            for p, b, t in lst[s]:
                out("    %04X  %-12s %s" % (p, b.hex(" "), t))
    for e in errs:
        out("  ERROR " + e)
    return h, hist, errs


# ----------------------------------------------------------------- SPU ADPCM / banks
ADPCM_F = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def adpcm_decode(blocks, state=None):
    """Decode a bytes object of 16-byte ADPCM blocks to a list of int16.  Stops after a block with
    flag bit0 (loop end).  Returns (samples, flags_list, n_blocks)."""
    old = older = 0
    out = []
    flags = []
    nb = 0
    for o in range(0, len(blocks) - 15, 16):
        b = blocks[o:o + 16]
        shift = b[0] & 0xF
        filt = min(b[0] >> 4, 4)
        fl = b[1]
        flags.append(fl)
        nb += 1
        if shift > 12:
            shift = 9
        f0, f1 = ADPCM_F[filt]
        for i in range(28):
            nib = (b[2 + (i >> 1)] >> (4 * (i & 1))) & 0xF
            if nib & 8:
                nib -= 16
            s = ((nib << 12) >> shift) + ((old * f0 + older * f1 + 32) >> 6)
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out.append(s)
            older, old = old, s
        if fl & 1:
            break
    return out, flags, nb


_AT = [(0, 1), (1, 2), (2, 3), (3, 4)]  # attack/sustain step fields -> +7,+6,+5,+4


def adsr_decode(lo, up):
    d = {"attack_exp": bool(lo >> 15), "attack_shift": lo >> 10 & 31, "attack_step": lo >> 8 & 3,
         "decay_shift": lo >> 4 & 15, "sustain_level": lo & 15,
         "sustain_exp": bool(up >> 15), "sustain_dec": bool(up >> 14 & 1), "sustain_shift": up >> 8 & 31,
         "sustain_step": up >> 6 & 3, "release_exp": bool(up >> 5 & 1), "release_shift": up & 31}
    return d


def _env_wait_adj(exp, dec, shift, step_field, level):
    step = (-8 + step_field) if dec else (7 - step_field)
    adj = step << max(0, 11 - shift)
    wait = 1 << max(0, shift - 11)
    if exp and not dec and level > 0x6000:
        wait *= 4
    if exp and dec:
        adj = (adj * level) >> 15
    return wait, adj


def adsr_times(lo, up, rate=44100.0):
    """Seconds for attack (0->0x7FFF), decay (->sustain level) and release (0x7FFF->0), hardware model."""
    a = adsr_decode(lo, up)
    res = {}
    lvl, t = 0, 0
    while lvl < 0x7FFF and t < 10 * 44100 * 100:
        w, adj = _env_wait_adj(a["attack_exp"], False, a["attack_shift"], a["attack_step"], lvl)
        lvl = min(0x7FFF, lvl + adj)
        t += w
    res["attack_s"] = t / rate
    target = (a["sustain_level"] + 1) * 0x800
    t = 0
    lvl = 0x7FFF
    while lvl > target and t < 44100 * 1000:
        w, adj = _env_wait_adj(True, True, a["decay_shift"], 0, lvl)
        lvl = max(0, lvl + adj)
        t += w
    res["decay_s"] = t / rate
    lvl, t = 0x7FFF, 0
    while lvl > 0 and t < 44100 * 1000:
        w, adj = _env_wait_adj(a["release_exp"], True, a["release_shift"], 0, lvl)
        if adj == 0:
            t = 44100 * 1000
            break
        lvl = max(0, lvl + adj)
        t += w
    res["release_s"] = t / rate
    return res


def parse_bank(d, base=0):
    """AKAO instrument bank starting at d[base].  Returns dict with instruments."""
    if d[base:base + 4] != b"AKAO":
        raise AudioError("no AKAO magic")
    bid = struct.unpack_from("<H", d, base + 4)[0]
    spu, size, first, count = struct.unpack_from("<4I", d, base + 0x10)
    if not 0 < count <= 256:
        raise AudioError("bad instrument count %d" % count)
    tbl = base + 0x40
    data = tbl + 16 * count
    if data > len(d):
        raise AudioError("instrument table past EOF")
    insts = []
    for i in range(count):
        s, l, ft, sn, lo, up = struct.unpack_from("<IIhhHH", d, tbl + 16 * i)
        insts.append({"index": first + i, "start": s, "loop": l, "fine_tune": ft, "base_note": sn,
                      "adsr_lo": lo, "adsr_hi": up})
    avail = len(d) - data
    b = {"bank_id": bid, "spu_addr": spu, "sample_bytes": size, "first_index": first, "count": count,
         "instruments": insts, "data_offset": data, "data_avail": avail, "base": base}
    if size > avail + 0x40:
        raise AudioError("sample size 0x%X exceeds available 0x%X" % (size, avail))
    return b


def bank_samples(d, b):
    """Return {start: (n_samples, flags, loop_start_sample or None, end_flags)} per unique start."""
    data = d[b["data_offset"]:b["data_offset"] + b["sample_bytes"]]
    starts = sorted(set(i["start"] for i in b["instruments"]))
    res = {}
    for s in starts:
        if s >= len(data):
            res[s] = None
            continue
        pcm, fl, nb = adpcm_decode(data[s:])
        ls = None
        for k, f in enumerate(fl):
            if f & 4:
                ls = k * 28
                break
        res[s] = {"pcm": pcm, "flags": fl, "loop_sample": ls, "ended": bool(fl and fl[-1] & 1), "blocks": nb}
    return res


def cmd_wave_info(d, label, out):
    b = parse_bank(d)
    out("%s: AKAO bank id=%d spu_addr=0x%X sample_bytes=0x%X instruments %d..%d file=%d (data at 0x%X, %d avail)"
        % (label, b["bank_id"], b["spu_addr"], b["sample_bytes"], b["first_index"],
           b["first_index"] + b["count"] - 1, len(d), b["data_offset"], b["data_avail"]))
    samples = bank_samples(d, b)
    probs = 0
    for i in b["instruments"]:
        s = samples.get(i["start"])
        a = adsr_decode(i["adsr_lo"], i["adsr_hi"])
        t = adsr_times(i["adsr_lo"], i["adsr_hi"])
        n = "-" if not s else str(len(s["pcm"]))
        lflag = "-" if not s else ("loop" if s["flags"][-1] & 2 else "oneshot") + (
            "" if s["ended"] else "(no end flag)")
        if s is None or not s["ended"]:
            probs += 1
        out("  [%3d] start=0x%06X loop=0x%06X tune=%6d note=%3d ADSR=%04X/%04X A=%s%d.%d D=%d SL=%d S=%s%s%d.%d R=%s%d"
            " (A%.3fs D%.3fs R%.3fs) n=%s %s" % (
                i["index"], i["start"], i["loop"], i["fine_tune"], i["base_note"], i["adsr_lo"], i["adsr_hi"],
                "e" if a["attack_exp"] else "l", a["attack_shift"], a["attack_step"], a["decay_shift"],
                a["sustain_level"], "e" if a["sustain_exp"] else "l", "-" if a["sustain_dec"] else "+",
                a["sustain_shift"], a["sustain_step"], "e" if a["release_exp"] else "l", a["release_shift"],
                t["attack_s"], t["decay_s"], t["release_s"], n, lflag))
    if probs:
        out("  note: %d instruments without a reachable end-flag block" % probs)
    return b, samples


def write_wav(path, pcm, rate=44100, loop=None):
    data = struct.pack("<%dh" % len(pcm), *pcm)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    chunks = b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    if loop is not None:
        smpl = struct.pack("<9I", 0, 0, int(1e9 / rate), 60, 0, 0, 0, 1, 0) + \
            struct.pack("<6I", 0, 0, loop[0], loop[1], 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def cmd_wave_export(d, label, outdir, out):
    b = parse_bank(d)
    samples = bank_samples(d, b)
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for i in b["instruments"]:
        s = samples.get(i["start"])
        if not s or not s["pcm"]:
            continue
        name = os.path.join(outdir, "inst%03d_%06X.wav" % (i["index"], i["start"]))
        if os.path.exists(name):
            continue
        loop = None
        if s["flags"][-1] & 2:
            ls = s["loop_sample"]
            if ls is None and i["loop"] >= i["start"]:
                ls = (i["loop"] - i["start"]) // 16 * 28
            if ls is not None:
                loop = (ls, len(s["pcm"]) - 1)
        write_wav(name, s["pcm"], 44100, loop)
        n += 1
    out("exported %d samples to %s" % (n, outdir))


# ----------------------------------------------------------------- SFX
def parse_sfx_blob(d):
    akao_header(d)
    pairs = [struct.unpack_from("<HH", d, 0x10 + 4 * i) for i in range(256)]
    if len(d) < 0x810:
        raise AudioError("sfx blob too short")
    return {"pairs": pairs, "meta_a": struct.unpack_from("<256H", d, 0x410),
            "chain_count": struct.unpack_from("<256H", d, 0x610), "code": 0x810}


def sfx_spans(d, sb):
    starts = set()
    for a, b in sb["pairs"]:
        for x in (a, b):
            if x != 0xFFFF:
                starts.add(sb["code"] + x)
    st = sorted(starts)
    return [(s, st[i + 1] if i + 1 < len(st) else len(d)) for i, s in enumerate(st)]


def cmd_sfx_info(d, label, out, ops=False):
    sb = parse_sfx_blob(d)
    used = [i for i, p in enumerate(sb["pairs"]) if p != (0xFFFF, 0xFFFF)]
    out("%s: SFX blob, %d defined ids (of 256), code at 0x810, size %d" % (label, len(used), len(d)))
    hist = collections.Counter()
    errs = []
    for s, e in sfx_spans(d, sb):
        L = []
        decode_span(d, s, e, hist, L, errs)
        if ops:
            out("  --- 0x%04X..0x%04X" % (s, e))
            for p, b, t in L:
                out("    %04X  %-12s %s" % (p, b.hex(" "), t))
    for i in used[:8]:
        out("  id %3d pcs=%s chain=%d metaA=%d" % (i, ["%04X" % x if x != 0xFFFF else "-" for x in sb["pairs"][i]],
                                                  sb["chain_count"][i], sb["meta_a"][i]))
    for e in errs[:20]:
        out("  ERROR " + e)
    return sb, hist, errs


def parse_yos(d, tag):
    """room subfile 9 (yos1) / 10 (yos0): tag, u32 size-8, u32 count, u32 off[count+1] relative to +8."""
    if d[:4] != tag:
        raise AudioError("bad magic %r" % d[:4])
    size, count = struct.unpack_from("<II", d, 4)
    if (size + 8 - len(d)) & 0xFFFF:
        raise AudioError("size field %d != %d (mod 64K)" % (size + 8, len(d)))
    if count > 64:
        raise AudioError("count %d" % count)
    offs = [struct.unpack_from("<I", d, 12 + 4 * i)[0] + 8 for i in range(count + 1)]
    if offs[-1] != len(d) or any(offs[i] > offs[i + 1] for i in range(count)):
        raise AudioError("offset table inconsistent")
    blocks = []
    for i in range(count):
        b = d[offs[i]:offs[i + 1]] if offs[i] != 8 else b""
        if not b:
            continue            # empty slot (rooms without sound: count 1, zero-length block)
        if b[:4] != b"AKAO":
            raise AudioError("block %d not AKAO" % i)
        blocks.append(b)
    return blocks


def parse_yos1_block(b):
    """Room SFX program frame: AKAO, u16 id(=1), +8 u16 bank id, +0x14 two u16 offsets (0xFFFF none) relative
    to +0x18; code follows."""
    pcs = struct.unpack_from("<HH", b, 0x14)
    bank = struct.unpack_from("<H", b, 8)[0]
    starts = sorted(0x18 + x for x in pcs if x != 0xFFFF)
    return {"bank_id": bank, "pcs": pcs, "starts": starts}


def cmd_room_sound(game, room, out, ops=True):
    cpt = cc_container.parse_cpt(game.read("map/mapbin/%s.bin" % room))
    y1 = parse_yos(cpt[9], b"yos1")
    y0 = parse_yos(cpt[10], b"yos0")
    out("room %s: yos1 %d SFX frames, yos0 %d banks" % (room, len(y1), len(y0)))
    for i, b in enumerate(y0):
        bk = parse_bank(b)
        out("  yos0[%d] bank id=%d spu=0x%X bytes=0x%X inst %d..%d" % (
            i, bk["bank_id"], bk["spu_addr"], bk["sample_bytes"], bk["first_index"], bk["first_index"] + bk["count"] - 1))
    for i, b in enumerate(y1):
        f = parse_yos1_block(b)
        out("  yos1[%d] bank=%d pcs=%s len=%d" % (i, f["bank_id"], ["%04X" % x for x in f["pcs"]], len(b)))
        if ops:
            L = []
            errs = []
            st = f["starts"] + [len(b)]
            for a, e in zip(st, st[1:]):
                decode_span(b, a, e, None, L, errs, tail_ok=(e == len(b)))
            for p, bb, t in L:
                out("    %04X  %-12s %s" % (p, bb.hex(" "), t))
            for e in errs:
                out("    ERROR " + e)


# ----------------------------------------------------------------- stats
def cmd_stats(game, out):
    fails = []
    warns = []
    allhist = collections.Counter()

    def attempt(name, fn):
        try:
            return fn()
        except Exception as e:  # noqa
            fails.append((name, "%s: %s" % (type(e).__name__, e)))

    # BGM
    bgm = game.glob("sound/bgm/BGM", ".sab")
    csvmap = {}
    cp = os.path.join(game.dir, "research", "data", "bgm_map.csv")
    if os.path.exists(cp):
        for r in csv.DictReader(open(cp, newline="")):
            csvmap[r["sab_file"]] = r
    ok = 0
    mism = 0
    for n in bgm:
        def f(n=n):
            d = game.read(n)
            s = parse_sab(d)
            m = s["materials"][0]
            h = m["hca"]
            if not h["crc_ok"] or h["ciph"] != 0 or m["codec"] != 7:
                raise AudioError("hca crc/ciph/codec")
            if int(m["frames_in_file"]) != h["frame_count"]:
                raise AudioError("frame count %s vs file %s" % (h["frame_count"], m["frames_in_file"]))
            r = csvmap.get(os.path.basename(n))
            if r and (int(r["total_samples"]) != h["total_samples"] or int(r["loop_start"]) != m["loop_start"]
                      or int(r["loop_end"]) != m["loop_end"]):
                raise AudioError("differs from bgm_map.csv")
            return 1
        if attempt(n, f):
            ok += 1
    out("sab: %d files, %d ok (HCA crc, ciph=0, frame count, bgm_map.csv match)" % (len(bgm), ok))
    # music
    mus = game.glob("sound/music/music", ".dat")
    ok = 0
    nch = 0
    for n in mus:
        def f(n=n):
            d = game.read(n)
            h = parse_music(d)
            errs = []
            for s, e in music_spans(h):
                decode_span(d, s, e, allhist, None, errs)
            if errs:
                raise AudioError("; ".join(errs[:3]))
            return h
        h = attempt(n, f)
        if h:
            ok += 1
            nch += len(h["channels"])
    out("music: %d files, %d ok, %d channels decoded to span ends" % (len(mus), ok, nch))
    # waves (all wave####.dat anywhere, plus exwave)
    waves = [n for n in game.names if n.endswith(".dat") and "/wave" in n and
             os.path.basename(n).startswith("wave")]
    ok = 0
    ninst = 0
    for n in waves:
        def f(n=n):
            d = game.read(n)
            b = parse_bank(d)
            smp = bank_samples(d, b)
            bad = [k for k, v in smp.items() if v is None or not v["ended"]]
            if bad:
                warns.append((n, "%d sample start(s) without reachable end flag" % len(bad)))
            return b
        b = attempt(n, f)
        if b:
            ok += 1
            ninst += b["count"]
    out("wave: %d files, %d ok, %d instruments" % (len(waves), ok, ninst))
    # sfx
    sfx = [n for n in game.names if n.startswith("sound/effect/") and n.endswith(".dat")]
    ok = 0
    for n in sfx:
        def f(n=n):
            d = game.read(n)
            sb, h, errs = cmd_sfx_info(d, n, lambda *_: None)
            allhist.update(h)
            if errs:
                raise AudioError("; ".join(errs[:3]))
            return 1
        if attempt(n, f):
            ok += 1
    out("sfx blobs: %d files, %d ok" % (len(sfx), ok))
    # rooms
    rooms = game.glob("map/mapbin/", ".bin")
    ok = 0
    nfr = nbk = 0
    for n in rooms:
        def f(n=n):
            cpt = cc_container.parse_cpt(game.read(n))
            y1 = parse_yos(cpt[9], b"yos1")
            y0 = parse_yos(cpt[10], b"yos0")
            errs = []
            for b in y0:
                bk = parse_bank(b)
                data = b[bk["data_offset"]:bk["data_offset"] + bk["sample_bytes"]]
                for s in set(i["start"] for i in bk["instruments"]):
                    if s < len(data):
                        _, fl, _ = adpcm_decode(data[s:])
                        if not (fl and fl[-1] & 1):
                            errs.append("sample without end")
            for b in y1:
                fr = parse_yos1_block(b)
                st = fr["starts"] + [len(b)]
                for a, e in zip(st, st[1:]):
                    decode_span(b, a, e, allhist, None, errs, tail_ok=(e == len(b)))
            bad = [e for e in errs if e == "sample without end"]
            if bad:
                raise AudioError("; ".join(bad[:3]))
            if errs:
                warns.append((n, "yos1 code: " + "; ".join(errs[:3])))
            return len(y1), len(y0)
        r = attempt(n, f)
        if r:
            ok += 1
            nfr += r[0]
            nbk += r[1]
    out("rooms: %d rooms, %d ok, %d yos1 frames, %d yos0 banks" % (len(rooms), ok, nfr, nbk))
    out("")
    out("warnings: %d" % len(warns))
    for n, m in warns[:12]:
        out("  %-40s %s" % (n, m))
    out("failures: %d" % len(fails))
    for n, m in fails[:60]:
        out("  %-40s %s" % (n, m))
    out("")
    out("opcode histogram (all music + sfx + room frames):")
    for k, v in sorted(allhist.items(), key=lambda kv: (kv[0].isdigit(), kv[0])):
        name = k
        if k.startswith("FE") and len(k) == 4:
            name = "%s %s" % (k, FE_OPS.get(int(k[2:], 16), ("?",))[0])
        elif len(k) == 2 and k not in ("FF",) and all(c in "0123456789ABCDEF" for c in k):
            name = "%s %s" % (k, OPS.get(int(k, 16), ("?",))[0])
        out("  %-34s %d" % (name, v))
    return fails


# ----------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=default_game_dir(), help="RDE install dir (contains data/cdrom.dat)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, subs in (("sab", ["info"]), ("music", ["info"]), ("wave", ["info", "export"]), ("sfx", ["info"])):
        p = sub.add_parser(name)
        sp = p.add_subparsers(dest="sub", required=True)
        for s in subs:
            q = sp.add_parser(s)
            q.add_argument("file")
            if s == "export":
                q.add_argument("outdir")
            if name in ("music", "sfx"):
                q.add_argument("--no-ops", action="store_true")
                q.add_argument("--ops", action="store_true")
    sub.add_parser("ops")
    sub.add_parser("stats")
    a = ap.parse_args(argv)
    out = print
    try:
        if a.cmd == "ops":
            print_optable(out)
        elif a.cmd == "stats":
            fails = cmd_stats(Game(a.game), out)
            return 1 if fails else 0
        else:
            if a.cmd == "sfx" and not os.path.exists(a.file) and "/" not in a.file:
                cmd_room_sound(Game(a.game), a.file, out)
                return 0
            d, label = load_arg(a.file, a.game)
            if a.cmd == "sab":
                cmd_sab_info(d, label, out)
            elif a.cmd == "music":
                cmd_music_info(d, label, out, ops=not a.no_ops)
            elif a.cmd == "wave" and a.sub == "info":
                cmd_wave_info(d, label, out)
            elif a.cmd == "wave":
                cmd_wave_export(d, label, a.outdir, out)
            elif a.cmd == "sfx":
                cmd_sfx_info(d, label, out, ops=a.ops)
    except AudioError as e:
        print("error:", e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
