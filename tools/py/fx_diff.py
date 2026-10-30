"""fx_diff.py DIR [--step 5] [--offset N|auto] [--cast-index N] [--window A B] : the mechanical diff of one effect cast, ps1 (PS1) vs OpenCross.

    DIR = tmp\\r34\\<id>   with   ref\\states\\state_NNNNNN.sav (+ ref\\frames\\frame_NNNNNN.png)   and   ours\\ours.log (+ ours\\shots\\shot_NNNNNN.png)
    ours.log = tools\\ours_cast34.ps1 (OC_FX_OBJECTS / OC_FX_SEQ / OC_FX_UNITS = 5): `fxseq:`, `fx objects`, `fx units` + unit lines, `fx camera` lines.

Step 1 extracts every ps1 state once into DIR\\ref\\tables.json (so the states can be deleted afterwards): sequence tasks, live objects (all, with
the per-emitter first object), unit model blocks, camera, fog, warp. Step 2 parses ours.log into the same shape. Step 3 finds the offset
(our tick = ps1 refresh time + offset; the median over common sequence names of our start tick - ps1 start (= refresh time - elapsed); fallback
the first live object on both sides), then writes DIR\\diff.md (verdict block, per-frame differences, the full table collapsed) and
DIR\\shots.png (ps1 frame | our shot at the first draw and the peak object count). Missing categories give a `-` verdict, never an error.

Time axis: the PS1 battle runs one game frame (= one capture frame) per 2-3 refreshes while our ticks are refreshes, so the reference states are
placed by the battle refresh counter they carry (`bf` in tables.json; old captures read it from the manifest `watch` rows), as refresh time = counter -
counter of the first state; a capture without any counter falls back to the run frame. Every ps1 time in diff.md is that refresh time (`R`), the
frame files keep their run-frame numbers (`F`).

Tolerances: object count +-2 per emitter, first-object numbers 5 % (abs floor 8), unit position / yaw 64, camera eye / target 64, fog 5 %.
Options (the plain call runs the diff):
  --extract           tables only (ref\\tables.json from the states; no ours needed) + `cam_pick:` appended to ref\\manifest.txt from camera-shot sequence-task tags
                      (ch1N / tgtN / aprN / numbered shots; the pack's own first sequence and `main` excluded) unless the manifest already has a non-empty one
  --info              prints `last_frame: N` (the ps1 capture length in run frames), `last_refresh: R` (its span in refreshes = the number of our ticks the capture covers), `states: M` and `dt: t:N,t:N` (the PS1 loop iteration length in vsyncs along the
                      'main' elapsed t, estimated from how the elapsed counter steps between the 5-frame samples)
  --cam-score LOG..   per our probe log: how many sampled ps1 camera frames match (eye / target within 64) -> `LOG: match/compared shift S` (best of the shifts -1 / 0 / +1 sample), then `best: <index>`, `clear: 1|0` (>= 50 % or >= 2x the runner-up), `shift: S`, `mode: exact|shape` (shape = the eye turned about the target), `score: ok/n`
  --set-cam-pick S    writes `cam_pick: S` into the manifest (replacing an empty line)
  --prune             after the diff: delete ref\\states\\*.sav (tables.json exists), keep every 8th ref frame + the two used, keep the two ours shots used
  Stage emitters (anything live in ps1 before the first sequence starts: `near`, `far`) are reported as `stage`, not counted; fog / warp are
  compared only from the first op (fog on / warp on) on either side.
Python 3.13 with numpy / PIL / zstandard (the ps1 side) - run as `python -I tools\\py\\fx_diff.py tmp\\r34\\7`.
"""
import json
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TOL_COUNT = 2
TOL_REL = 0.05
TOL_ABS = 8
TOL_POS = 64
TOL_POOL = 300   # cam_score: a pool member is a 60-90 tick shot, so a 1-2 sample timing / anchor-height lag (e.g. a 350 u lag on Fireball) must not lose it to a wrong pick


# ------------------------------------------------------------------------------------------------------------------ ps1 side
def extract_ref(refdir):
    """Reads every state in refdir/states with fx_ref / ps1_state; returns {frame: tables}."""
    import struct
    import fx_ref as fr
    from ps1_state import State, frame_of, list_states
    out = {}
    sd = os.path.join(refdir, "states")
    for p in list_states([sd]):
        f = frame_of(p)
        try:
            st = State(p)
        except Exception as e:  # a state we cannot read: skip it
            print("fx_diff: state %s unreadable (%s)" % (p, e))
            continue
        t = {"seqs": [], "objs": [], "units": {}, "cam": None, "fog": None, "warp": None, "bf": st.u32(0x800E0518)}
        try:
            reg = fr.Registry(st)
            base, ts = fr.tasks(st)
            for e, cb, d in ts:
                if cb == fr.CB_SEQ:
                    seq = st.u32(d + 4)
                    t["seqs"].append({"name": reg.name_of_data(seq), "tag": fr.tag(st.u32(d + 0x10)), "elapsed": st.s16(d + 0x14), "wait": st.s16(d + 0x18)})
                elif cb == fr.CB_OBJECT:
                    o = d
                    vel = [struct.unpack_from("<i", st.ram, st._o(o + k))[0] >> 6 for k in (0x10, 0x14, 0x18)]
                    t["objs"].append({
                        "em": reg.name_of_data(st.u32(o + 0xC)), "spr": reg.name_of_node(st.u32(o + 0x1C)),
                        "pos": [st.s16(o + k + 2) for k in (0, 4, 8)], "vel": vel, "sc": list(fr.s16x3(st, o + 0x50)),
                        "ang": list(fr.s16x3(st, o + 0x5C)), "col": [st.u16(o + 0x68), st.u16(o + 0x6A), st.u16(o + 0x6C)],
                        "life": st.u16(o + 0x7C), "fl": st.u32(o + 0x88)})
        except Exception as e:
            print("fx_diff: %s: effect tables unreadable (%s)" % (os.path.basename(p), e))
        try:
            for u in range(12):
                c = 0x800E31B0 + 0x30 * u
                m = st.u32(c)
                if not (0x80000000 <= m < 0x80800000):
                    continue
                pos = [struct.unpack_from("<i", st.ram, st._o(m + k))[0] >> 12 for k in (0x84, 0x88, 0x8C)]
                t["units"][str(u)] = {"fl": st.u16(m + 0x7E), "col": [st.u8(m + 0x2C), st.u8(m + 0x2D), st.u8(m + 0x2E)], "pos": pos,
                                      "yaw": st.s16(m + 0xA4 + 2), "rot": list(fr.s16x3(st, m + 0xA4)), "scale": list(fr.s16x3(st, m + 0xAC)),
                                      "clip": st.s16(m + 0x16), "motion": st.s16(m + 0x18)}
        except Exception as e:
            print("fx_diff: %s: unit blocks unreadable (%s)" % (os.path.basename(p), e))
        try:
            t["cam"] = {"eye": [st.s16(0x80071F68 + 2 * i) for i in range(3)], "tgt": [st.s16(0x80071F70 + 2 * i) for i in range(3)], "H": st.s16(0x80071F84)}
            t["fog"] = {"near": st.u16(0x8008DBB8), "far": st.u16(0x8008DBB2), "rgb": [st.u8(0x8008DBB4), st.u8(0x8008DBB5), st.u8(0x8008DBB6)], "on": st.u8(0x800721E0)}
            t["warp"] = {"on": st.u8(0x800720D8), "ang": list(fr.s16x3(st, 0x800720E0)), "tr": list(fr.s16x3(st, 0x800720E8)),
                         "col": [st.u8(0x800720F0), st.u8(0x800720F1), st.u8(0x800720F2)], "off": [st.s16(0x800720F4), st.s16(0x800720F6)],
                         "abr": st.u8(0x800720FC), "slow_n": st.u8(0x80072100), "slow_x": st.u8(0x800720FF)}
        except Exception as e:
            print("fx_diff: %s: camera / fog / warp unreadable (%s)" % (os.path.basename(p), e))
        out[f] = t
    return out


def norm_ref(tabs):
    """ps1 tags are 4 bytes with NUL shown as '.': '2r3.' = ours '2r3'."""
    for t in tabs.values():
        for s in t["seqs"]:
            s["name"] = s["name"].rstrip(". ")  # the 4-byte tag keeps trailing spaces ("0   ", "c2 ") that the ours.log \S+ field drops
        for o in t["objs"]:
            o["em"] = o["em"].rstrip(". ")
            o["spr"] = o["spr"].rstrip(".")
    return tabs


def manifest_refresh_counters(d):
    """{run frame: battle refresh counter} from the `watch` rows of ref\\manifest.txt (captures extracted before the counter was kept in tables.json)."""
    mp = manifest_path(d)
    out = {}
    if os.path.isfile(mp):
        with open(mp, "r", encoding="utf-8-sig", errors="replace") as fh:
            for line in fh:
                m = re.match(r"\s*(\d+) btl_frame=(\d+)", line)
                if m:
                    out[int(m.group(1))] = int(m.group(2))
    return out


def to_refresh_time(d, tabs):
    """Re-keys the per-run-frame tables by refresh time: the PS1 battle runs one game frame (one capture frame) per 2-3 refreshes, and our ticks are
    refreshes, so key = battle refresh counter - the counter of the first state. Each table keeps its run frame as `F` (frame files, --info).
    Sequence elapsed counters advance in refreshes too, so `key - elapsed` is a start on the same axis. Without any counter the run frame stays the key."""
    fallback = manifest_refresh_counters(d)
    for f, t in tabs.items():
        if "bf" not in t and f in fallback:
            t["bf"] = fallback[f]
    frames = sorted(tabs)
    if not frames or any("bf" not in tabs[f] for f in frames):
        for f in frames:
            tabs[f]["F"] = f
        return tabs
    first = tabs[frames[0]]["bf"]
    out = {}
    for f in frames:
        key = tabs[f]["bf"] - first
        while key in out:
            key += 1
        tabs[f]["F"] = f
        out[key] = tabs[f]
    return out


def load_ref(d, force=False):
    jp = os.path.join(d, "ref", "tables.json")
    sd = os.path.join(d, "ref", "states")
    have_states = os.path.isdir(sd) and any(f.endswith(".sav") for f in os.listdir(sd))
    if os.path.isfile(jp) and not force:
        newest = max((os.path.getmtime(os.path.join(sd, f)) for f in os.listdir(sd) if f.endswith(".sav")), default=0) if have_states else 0
        if os.path.getmtime(jp) >= newest:
            with open(jp) as fh:
                return to_refresh_time(d, norm_ref({int(k): v for k, v in json.load(fh).items()}))
    if not have_states:
        return {}
    tabs = extract_ref(os.path.join(d, "ref"))
    with open(jp, "w") as fh:
        json.dump({str(k): v for k, v in tabs.items()}, fh)
    return to_refresh_time(d, norm_ref(tabs))


# ------------------------------------------------------------------------------------------------------------------ ours side
RE_HDR = re.compile(r"fx objects tick (\d+) fxtick (\d+): (\d+) objects")
RE_OBJ = re.compile(r"\bem (\S+) +spr (\S+) t(\d+) pos\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) vel\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) sc\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) "
                    r"ang\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) col\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) rgb\s+\d+\s+\d+\s+\d+ life\s+(-?\d+) bias\s+-?\d+ fr\s+-?\d+ fl ([0-9a-f]+)")
RE_UH = re.compile(r"fx units tick (\d+) fxtick (\d+)")
RE_UNIT = re.compile(r"\bunit\s+(\d+) flags7e ([0-9a-f]+) ghost (\d) col\s+(\d+)\s+(\d+)\s+(\d+) pos\s+(-?\d+)\s+(-?\d+)\s+(-?\d+) yaw\s+(\d+) clip (-?\d+) motion ([-\d.]+) \((-?\d+)/256\)")
RE_CAM = re.compile(r"fx camera tick (\d+) eye (-?\d+) (-?\d+) (-?\d+) tgt (-?\d+) (-?\d+) (-?\d+) H (-?\d+) fog (\d) near (-?\d+) far (-?\d+) rgb (-?\d+) (-?\d+) (-?\d+) "
                    r"warp on (\d) ang (-?\d+) (-?\d+) (-?\d+) trans (-?\d+) (-?\d+) (-?\d+) col (-?\d+) (-?\d+) (-?\d+) off (-?\d+) (-?\d+) abr (\d+) slow (\d+)/(\d+)")
RE_MARK = re.compile(r"fx tickmark (\d+) (\d+)")
RE_SEQ = re.compile(r"fxseq: t(\d+) (\S+) #(\d+) op ([0-9A-F]+) wait (-?\d+)")


RE_CREDIT = re.compile(r"battle fx: pack birth credit (\d+) at frame (\d+)")
OURS_CREDIT = 0  # the ticks our pack `main` was run ahead at its start (BattleView::kPackCredit); 0 = an older log


def parse_ours(path):
    """Returns {tick: tables} (objs / units / cam / fog / warp per sampled tick) and {seq name: [first tick, last tick, commands]}."""
    global OURS_CREDIT
    OURS_CREDIT = 0
    credit_at = None
    samples = {}
    seqs = {}
    pend = []  # (name, fxtick-relative t) before the first header is known
    base = None
    marked = False
    cur = None
    mode = None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = RE_CREDIT.search(line)
            if m:
                OURS_CREDIT, credit_at = int(m.group(1)), int(m.group(2))
                continue
            m = RE_SEQ.search(line)
            if m:
                pend.append((int(m.group(1)), m.group(2)))
                continue
            m = RE_MARK.search(line)
            if m:  # the fxseq lines since the last mark ran in this tick (their t is task-local)
                for _, name in pend:
                    e = seqs.setdefault(name, [int(m.group(1)), int(m.group(1)), 0])
                    e[0] = min(e[0], int(m.group(1)))
                    e[1] = max(e[1], int(m.group(1)))
                    e[2] += 1
                pend = []
                marked = True
                continue
            m = RE_HDR.search(line)
            if m:
                tick, fxt = int(m.group(1)), int(m.group(2))
                base = tick - fxt
                cur = samples.setdefault(tick, {"seqs": [], "objs": [], "units": {}, "cam": None, "fog": None, "warp": None})
                mode = "obj"
                continue
            m = RE_UH.search(line)
            if m:
                tick, fxt = int(m.group(1)), int(m.group(2))
                base = tick - fxt
                cur = samples.setdefault(tick, {"seqs": [], "objs": [], "units": {}, "cam": None, "fog": None, "warp": None})
                mode = "unit"
                continue
            m = RE_CAM.search(line)
            if m:
                g = [int(float(x)) for x in m.groups()]
                cur = samples.setdefault(g[0], {"seqs": [], "objs": [], "units": {}, "cam": None, "fog": None, "warp": None})
                cur["cam"] = {"eye": g[1:4], "tgt": g[4:7], "H": g[7]}
                cur["fog"] = {"on": g[8], "near": g[9], "far": g[10], "rgb": g[11:14]}
                cur["warp"] = {"on": g[14], "ang": g[15:18], "tr": g[18:21], "col": g[21:24], "off": g[24:26], "abr": g[26], "slow_n": g[28], "slow_x": g[27]}
                mode = None
                continue
            if cur is not None and mode == "obj":
                m = RE_OBJ.search(line)
                if m:
                    v = m.groups()
                    n = [int(x) for x in v[3:18]]
                    cur["objs"].append({"em": v[0], "spr": v[1], "pos": n[0:3], "vel": n[3:6], "sc": n[6:9], "ang": n[9:12], "col": n[12:15], "life": int(v[18]), "fl": int(v[19], 16)})
            elif cur is not None and mode == "unit":
                m = RE_UNIT.search(line)
                if m:
                    v = m.groups()
                    cur["units"][v[0]] = {"fl": int(v[1], 16), "ghost": int(v[2]), "col": [int(v[3]), int(v[4]), int(v[5])], "pos": [int(v[6]), int(v[7]), int(v[8])],
                                          "yaw": int(v[9]), "clip": int(v[10]), "motion": int(v[12])}
    for tn, name in (pend if not marked else []):  # old log without tickmarks: task-local t + the first header's base (inexact)
        t = tn + (base or 0)
        e = seqs.setdefault(name, [t, t, 0])
        e[0] = min(e[0], t)
        e[1] = max(e[1], t)
        e[2] += 1
    if OURS_CREDIT and credit_at is not None:  # the pack start is delayed by the hand-over (kHandOverDelay) and caught up: the sequences started in the catch-up began OURS_CREDIT ticks earlier
        for e in seqs.values():
            if credit_at <= e[0] <= credit_at + 2:
                e[0] -= OURS_CREDIT
    return samples, seqs


# ------------------------------------------------------------------------------------------------------------------ comparison
def close(a, b, rel=TOL_REL, floor=TOL_ABS):
    return abs(a - b) <= max(floor, rel * max(abs(a), abs(b)))


def by_em(objs):
    g = {}
    for o in objs:
        g.setdefault(o["em"], []).append(o)
    return g


def ref_seq_starts(ref):
    """{name: earliest (refresh time - elapsed)} over all states, and {name: (first seen time, last seen time)}."""
    st = {}
    seen = {}
    for f in sorted(ref):
        for s in ref[f]["seqs"]:
            n = s["name"]
            st[n] = min(st.get(n, 1 << 30), f - s["elapsed"])
            a = seen.get(n, (f, f))
            seen[n] = (min(a[0], f), max(a[1], f))
    return st, seen


CHOSEN_BIRTH = [None]   # the root birth find_offset picked when the capture holds several packs
CAST_INDEX = [None]     # --cast-index N: align on the Nth (0 = first) root birth instead of the best-scoring one


def ref_seq_births(ref):
    """{name: [birth refresh times]} : one per run of consecutive samples in which the sequence is live (a gap or an elapsed counter that went
    back = a new birth; a counter that falls behind the refresh time, as in the effects' slow motion, is the same birth)."""
    out = {}
    last = {}
    for f in sorted(ref):
        names = set()
        for s in ref[f]["seqs"]:
            n = s["name"]
            names.add(n)
            if n not in last or last[n] is None or s["elapsed"] < last[n]:
                out.setdefault(n, []).append(f - s["elapsed"])
            last[n] = s["elapsed"]
        for n in list(last):
            if n not in names:
                last[n] = None
    return out


MAIN_CREDIT = 3  # ps1 Anti-Red per-frame states: main elapsed 7 at the first visible frame = first-run delta 3 + the pass delta 4, i.e. its start frame reads ~3-6 early; fog / camera of the same frames agree with ours at main-start - 3


def find_offset(ref, ours, oseqs):
    rs, _ = ref_seq_starts(ref)
    # A sequence task born inside an update is run at once with the update's
    # delta (+0x14 elapsed, +0x18 wait) and again by the task loop of the same pass, so a CHILD (op 0x2F) starts with elapsed ~2 deltas
    # (ps1 eft1 elapsed 8 beside main 7 at the first visible frame) and its 'start' (refresh time - elapsed) comes out 4-8 early. The
    # pack's own `main` / `own` (born by the hand-over) are the roots: align on them, not on the median of all names.
    roots = [n for n in rs if n in oseqs and n in ("main", "own")]
    if roots:
        # A ps1 capture can hold SEVERAL packs (Cuscus 100: Serge's element 1, then its reflection; Schoolmates 169 likewise) while ours plays
        # one. Every birth of a root is a candidate; the one under which most other sequences of both sides start together wins (ties: the later birth).
        births = ref_seq_births(ref)
        cands = sorted({b for n in roots for b in births.get(n, [rs[n]])})
        def score(b):
            raw_b = oseqs[roots[0]][0] - b - MAIN_CREDIT
            ok = 0
            for n in oseqs:
                if n in roots:
                    continue
                if any(abs((oseqs[n][0] - raw_b) - c) <= 8 for c in births.get(n, [])):
                    ok += 1
            return ok
        if len(cands) > 1:
            if CAST_INDEX[0] is not None:
                best = cands[min(CAST_INDEX[0], len(cands) - 1)]
            else:
                best = max(cands, key=lambda b: (score(b), b))
            CHOSEN_BIRTH[0] = best
            rs = dict(rs)
            for n in roots:
                rs[n] = best
        raw = int(round(statistics.median([oseqs[n][0] - rs[n] for n in roots]))) - MAIN_CREDIT   # main's elapsed already carries its birth credit
        return raw, "pack %s start (our first command tick - ps1 refresh time + elapsed)" % "/".join(roots)
    diffs = [oseqs[n][0] - rs[n] for n in rs if n in oseqs]
    if diffs:
        # A plain median: the root path's birth credit does not hold here (Flag-Bearer 177 reads one sample better at 190 than at this 193,
        # Canon Cannon 298 five frames worse at 190 than at 195) - pass `--offset N` where a capture needs it.
        return int(round(statistics.median(diffs))), "median of %d common sequence starts (our first command tick - ps1 refresh time + elapsed)" % len(diffs)
    rf = next((f for f in sorted(ref) if ref[f]["objs"]), None)
    of = next((t for t in sorted(ours) if ours[t]["objs"]), None)
    if rf is not None and of is not None:
        return of - rf, "first live object (ps1 frame %d, our tick %d)" % (rf, of)
    return 0, "no common sequence or object: 0"


def nearest(ours, tick, step):
    best = None
    for dt in range(0, step // 2 + 1):
        for s in (tick - dt, tick + dt):
            if s in ours:
                return s
    return best


SHOT_RE = re.compile(r"^(ch\d+|tgt\w*|apr\w*|\d+\w?)$")


def manifest_path(d):
    return os.path.join(d, "ref", "manifest.txt")


def manifest_cam_pick(d):
    mp = manifest_path(d)
    if not os.path.isfile(mp):
        return ""
    with open(mp, "r", encoding="utf-8-sig", errors="replace") as fh:
        for line in fh:
            m = re.match(r"\s*cam_pick:\s*(.*)$", line)
            if m and m.group(1).strip():
                return m.group(1).strip()
    return ""


def set_cam_pick(d, pick):
    """Replaces every cam_pick: line (empty ones included) by one line with `pick`; no manifest = nothing."""
    mp = manifest_path(d)
    if not os.path.isfile(mp) or not pick:
        return
    with open(mp, "r", encoding="utf-8", newline="") as fh:
        raw = fh.read()
    eol = "\r\n" if "\r\n" in raw else "\n"
    out, done = [], False
    for line in raw.split(eol):
        if re.match(r"\s*\ufeff?cam_pick:", line):
            if not done:
                out.append("cam_pick: " + pick)
                done = True
            continue
        out.append(line)
    if not done:
        k = next((i for i, l in enumerate(out) if l.startswith("frames:")), len(out))
        out.insert(k, "cam_pick: " + pick)
    with open(mp, "w", encoding="utf-8", newline="") as fh:
        fh.write(eol.join(out))


def tag_cam_pick(ref):
    """Camera-shot names from the sequence-task tags / names, by first appearance over the ps1 frames. The pack's own first sequence (ch03 ...)
    and `main` are the effect sequences, not camera shots."""
    first = None
    seen = []
    for f in sorted(ref):
        for s in ref[f]["seqs"]:
            if first is None:
                first = s["name"]
            for n in (s["tag"].rstrip("."), s["name"]):
                if n and n != first and n != "main" and SHOT_RE.match(n) and n not in seen:
                    seen.append(n)
    return ",".join(seen)


def dt_schedule(ref):
    """The PS1 game-loop iteration length dt (vsyncs since the last frame) along the 'main' elapsed counter, as "t:N,..".

    A sequence's elapsed counter advances by dt per loop iteration, so across a 5-video-frame sample step it moves by dt * (floor or ceil of
    5 / dt) iterations: 3 or 6 for dt 3, 4 or 6 for dt 2, 4 or 8 for dt 4, always 5 for dt 1. A step of 5 is also what a 2 / 3 mix gives, so it
    counts for every dt >= 2 at half weight. Each sample step takes the dt that explains most of its 5 neighbours (ties: the larger)."""
    pts = []
    for f in sorted(ref):
        for s in ref[f].get("seqs", []):
            if s.get("name") == "main":
                pts.append((ref[f]["F"], s["elapsed"]))
                break
    steps = [(pts[i][1], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1) if pts[i + 1][0] - pts[i][0] == 5]

    def score(dd, delta):
        lo, hi = 5 // dd, -(-5 // dd)
        if delta % dd == 0 and lo <= delta // dd <= hi and not (dd > 1 and delta == 5):
            return 1.0
        return 0.5 if delta == 5 and dd > 1 else 0.0

    out = []
    for i, (e, _) in enumerate(steps):
        nb = steps[max(0, i - 2):i + 3]
        best = max(range(1, 6), key=lambda dd: (sum(score(dd, x) for _, x in nb), dd))
        if not out or out[-1][1] != best:
            out.append((e if out else 0, best))
    return ",".join("%d:%d" % p for p in out)


def tools_only(d, do_extract, do_info, setpick):
    ref = {}
    if do_extract or do_info:
        ref = load_ref(d, force=False)
    if do_extract:
        if not ref:
            print("fx_diff: no ps1 tables (ref\\tables.json / ref\\states) in %s" % d)
        else:
            print("extract: %d states -> %s" % (len(ref), os.path.join(d, "ref", "tables.json")))
            have = manifest_cam_pick(d)
            if have:
                print("cam_pick: %s (already in the manifest)" % have)
            else:
                pick = tag_cam_pick(ref)
                if pick:
                    set_cam_pick(d, pick)
                    print("cam_pick: %s (from the sequence-task tags; appended to the manifest)" % pick)
                else:
                    print("cam_pick: - (no camera-shot tags among the ps1 sequence tasks; ours_cast34.ps1 probes the camera pool)")
    if do_info:
        last = max(t["F"] for t in ref.values()) if ref else 0
        if not last:
            fd = os.path.join(d, "ref", "frames")
            if os.path.isdir(fd):
                last = max([int(x[6:12]) for x in os.listdir(fd) if x.startswith("frame_")] or [0])
        print("last_frame: %d" % last)
        print("last_refresh: %d" % (max(ref) if ref else 0))
        print("states: %d" % len(ref))
        print("dt: %s" % dt_schedule(ref))
    if setpick is not None:
        set_cam_pick(d, setpick)
        print("cam_pick: %s" % setpick)
    return 0


def default_window(ref):
    rs0, seen0 = ref_seq_starts(ref)
    if rs0:
        return [min(rs0.values()) - 5, max(b for a, b in seen0.values()) + 10]
    return None


def cam_score(d, ref, logs, step):
    """Per probe log, over the time shifts -1 / 0 / +1 sample (the ps1 charge camera runs a few frames ahead of ours): the EXACT score (eye and
    target within TOL_POS) and the SHAPE score (target y within TOL_POS and x / z within 2x, the eye radius about the target and its height within TOL_POS: the eye turned about the vertical axis through the target; the charge shots are
    anchored to the unit yaws, which the harness's pre-cast state does not reproduce - ps1 u0 yaw 1327 vs ours 1024 = the same 303 the camera is turned by).
    Prints `LOG: ok/n shift S` and `LOG: shape ok/n shift S yaw Y`, then `best: <index>`, `clear: 1|0` (best >= 2x the runner-up, or >= 50 %),
    `shift: S`, `mode: exact|shape`, `score: ok/n` of the winner (the exact scores rank when one is clear, else the shape scores)."""
    import math
    ex, sh = [], []
    for i, lp in enumerate(logs):
        if not os.path.isfile(lp):
            print("%s: missing" % lp)
            ex.append((0.0, 0, 0, 0))
            sh.append((0.0, 0, 0, 0, 0))
            continue
        ours, oseqs = parse_ours(lp)
        off, _ = find_offset(ref, ours, oseqs)
        win = default_window(ref)
        rex, rsh = [], []
        for s_ in (0, -1, 1):
            ok = ok2 = n = 0
            yaws = []
            for f in sorted(ref):
                if win and not (win[0] <= f <= win[1]):
                    continue
                tk = nearest(ours, f + off + s_ * step, step)
                ot = ours.get(tk) if tk is not None else None
                rc = ref[f]["cam"]
                if not ot or not ot["cam"] or not rc:
                    continue
                n += 1
                oe, ot_ = ot["cam"]["eye"], ot["cam"]["tgt"]
                tg = max(abs(rc["tgt"][k] - ot_[k]) for k in range(3)) <= TOL_POS
                tgw = max(abs(rc["tgt"][k] - ot_[k]) for k in range(3)) <= TOL_POOL
                if (tg and max(abs(rc["eye"][k] - oe[k]) for k in range(3)) <= TOL_POS) or (tgw and max(abs(rc["eye"][k] - oe[k]) for k in range(3)) <= TOL_POOL):
                    ok += 1
                tg2 = abs(rc["tgt"][1] - ot_[1]) <= TOL_POS and math.hypot(rc["tgt"][0] - ot_[0], rc["tgt"][2] - ot_[2]) <= 2 * TOL_POS   # the turned shot's pivot moves a little too
                if tg2:
                    rx, rz = rc["eye"][0] - rc["tgt"][0], rc["eye"][2] - rc["tgt"][2]
                    ox, oz = oe[0] - ot_[0], oe[2] - ot_[2]
                    if abs(math.hypot(rx, rz) - math.hypot(ox, oz)) <= TOL_POS and abs(rc["eye"][1] - oe[1]) <= TOL_POS:
                        ok2 += 1
                        yaws.append(round((math.degrees(math.atan2(rz, rx) - math.atan2(oz, ox)) + 180) % 360 - 180))
            yaws.sort()
            yaw = yaws[len(yaws) // 2] if yaws else 0
            rex.append((ok / n if n else 0.0, ok, n, s_))
            rsh.append((ok2 / n if n else 0.0, ok2, n, s_, yaw))
        a_, b_ = max(rex, key=lambda t: t[0]), max(rsh, key=lambda t: t[0])   # ties keep shift 0 (first)
        ex.append(a_)
        sh.append(b_)
        print("%s: %d/%d shift %+d" % (lp, a_[1], a_[2], a_[3]))
        print("%s: shape %d/%d shift %+d yaw %+d deg" % (lp, b_[1], b_[2], b_[3], b_[4]))

    def pick(sc):
        if not sc:
            return -1, False
        order = sorted(range(len(sc)), key=lambda i: -sc[i][0])
        top = sc[order[0]][0]
        second = sc[order[1]][0] if len(order) > 1 else 0.0
        return order[0], top > 0 and (top >= 0.5 or top >= 2 * second)

    bi, clear = pick(ex)
    mode, sc = "exact", ex
    if not clear:
        bi2, clear2 = pick(sh)
        if clear2:
            bi, clear, mode, sc = bi2, clear2, "shape", sh
    if bi < 0:
        print("best: -1")
        return 0
    print("best: %d" % bi)
    print("clear: %d" % (1 if clear else 0))
    print("shift: %+d" % sc[bi][3])
    print("mode: %s" % mode)
    print("score: %d/%d" % (sc[bi][1], sc[bi][2]))
    if mode == "shape":
        print("yaw: %+d deg" % sh[bi][4])
    return 0


def prune(d, used_ref, used_ours):
    """T2b: states go once tables.json exists; ref frames thin to every 8th + the ones shots.png used; ours shots keep only the used ones."""
    removed = freed = 0

    def rm(path):
        nonlocal removed, freed
        try:
            freed += os.path.getsize(path)
            os.remove(path)
            removed += 1
        except OSError:
            pass
    sd = os.path.join(d, "ref", "states")
    if os.path.isfile(os.path.join(d, "ref", "tables.json")) and os.path.isdir(sd):
        for f in os.listdir(sd):
            if f.endswith(".sav"):
                rm(os.path.join(sd, f))
    fd = os.path.join(d, "ref", "frames")
    if used_ref is not None and os.path.isdir(fd):
        for f in os.listdir(fd):
            m = re.match(r"frame_(\d+)\.png$", f)
            if m and int(m.group(1)) % 8 != 0 and f not in used_ref:
                rm(os.path.join(fd, f))
            elif m and f not in used_ref:  # the kept frames shrink to PS1 size (320 x 224 class): ~0.4 MB -> ~0.1 MB each
                try:
                    from PIL import Image
                    fp = os.path.join(fd, f)
                    im = Image.open(fp)
                    if im.height > 260:
                        before = os.path.getsize(fp)
                        im = im.convert("RGB").resize((max(1, im.width // 2), max(1, im.height // 2)), Image.LANCZOS)
                        im.save(fp, optimize=True)
                        freed += before - os.path.getsize(fp)
                except Exception:
                    pass
    od = os.path.join(d, "ours", "shots")
    if used_ours is not None and os.path.isdir(od):
        for f in os.listdir(od):
            if f.endswith(".png") and f not in used_ours:
                rm(os.path.join(od, f))
    print("prune: %d files, %.1f MB freed" % (removed, freed / 1048576.0))


def main(argv):
    args = list(argv)
    if not args:
        print(__doc__)
        return 1

    def opt(name, n=1, conv=int, default=None):
        if name in args:
            i = args.index(name)
            vals = args[i + 1:i + 1 + n]
            del args[i:i + 1 + n]
            return [conv(v) if v != "auto" else v for v in vals] if n > 1 else (conv(vals[0]) if vals[0] != "auto" else "auto")
        return default
    step = opt("--step", default=5)
    offset = opt("--offset", default="auto")
    window = opt("--window", 2, default=None)
    CAST_INDEX[0] = opt("--cast-index")
    do_extract = "--extract" in args
    do_info = "--info" in args
    do_prune = "--prune" in args
    for fl in ("--extract", "--info", "--prune"):
        while fl in args:
            args.remove(fl)
    score_logs = None
    if "--cam-score" in args:
        i = args.index("--cam-score")
        score_logs = args[i + 1:]
        del args[i:]
    setpick = opt("--set-cam-pick", conv=str)
    d = args[0]
    if do_extract or do_info or setpick is not None:
        return tools_only(d, do_extract, do_info, setpick)
    ref = load_ref(d)
    if score_logs is not None:
        return cam_score(d, ref, score_logs, step)
    lp = os.path.join(d, "ours", "ours.log")
    ours, oseqs = parse_ours(lp) if os.path.isfile(lp) else ({}, {})
    if not ref:
        print("fx_diff: no ps1 tables (ref\\tables.json / ref\\states) in %s" % d)
    if not ours:
        print("fx_diff: no ours.log samples in %s" % d)
    off_note = "given"
    if offset == "auto":
        offset, off_note = find_offset(ref, ours, oseqs)
    frames = sorted(ref)
    win_note = "all states"
    if not window:  # default: the effect's span = first sequence start - 5 .. last sequence seen + 10 (the stage's idle emitters outside it are not the cast)
        window = default_window(ref)
        if window and CHOSEN_BIRTH[0] is not None and CHOSEN_BIRTH[0] - 15 > window[0]:   # ours plays the LAST pack of a multi-pack capture
            window[0] = CHOSEN_BIRTH[0] - 15
    if window:
        frames = [f for f in frames if window[0] <= f <= window[1]]
        win_note = "ps1 refresh times %d..%d" % (window[0], window[1])

    # stage baseline (T2b): emitters live in ps1 before the first sequence starts (`near`, `far` ...) are the stage, not the cast
    rs_all, _ = ref_seq_starts(ref)
    first_seq = min(rs_all.values()) if rs_all else None
    stage = set()
    if first_seq is not None:
        for f in sorted(ref):
            if f < first_seq:
                stage.update(o["em"] for o in ref[f]["objs"])
    # a pruned capture (states from the cast on, e.g. scenes 447 / 479 / 392 / 402) has no pre-sequence frame, so the baseline above is empty
    # and the stage's own scenery emitters were counted as 'ps1-only near N'. Their names are fixed (far / near / roof / ring).
    stage.update(n for n in ("near", "far", "roof", "ring") if any(o["em"] == n for f in ref for o in ref[f]["objs"]))
    stage_peak = [0, 0]  # ps1 / ours objects of the stage emitters, peak over the compared frames
    # fog / warp are compared from the first op on either side
    fog_from = min([f for f in sorted(ref) if ref[f]["fog"] and (ref[f]["fog"]["on"] or ref[f]["fog"]["near"] or ref[f]["fog"]["far"])] +
                   [t - offset for t in sorted(ours) if ours[t]["fog"] and ours[t]["fog"]["on"]] or [1 << 30])
    warp_from = min([f for f in sorted(ref) if ref[f]["warp"] and ref[f]["warp"]["on"]] +
                    [t - offset for t in sorted(ours) if ours[t]["warp"] and ours[t]["warp"]["on"]] or [1 << 30])
    rows = []  # per ps1 frame
    v = {k: [0, 0] for k in ("seq", "cnt", "first", "units", "cam", "fog", "warp")}  # [bad, compared]
    vnotes = {k: [] for k in v}
    diffs_by_frame = {}

    def note(f, k, msg):
        diffs_by_frame.setdefault(f, []).append("%s: %s" % (k, msg))
        v[k][0] += 1
        if len(vnotes[k]) < 6:
            vnotes[k].append("R%d %s" % (f, msg))

    # sequences
    rs, seen = ref_seq_starts(ref)
    births = ref_seq_births(ref)   # a capture with several packs - compare a name with the birth nearest to ours
    for n_ in list(rs):
        if len(births.get(n_, [])) > 1 and n_ in oseqs:
            rs[n_] = min(births[n_], key=lambda b_: abs(b_ - (oseqs[n_][0] - offset)))
    seq_rows = []
    if ref and oseqs:
        names = sorted(set(rs) | set(oseqs))
        for n in names:
            r0 = rs.get(n)
            o0 = oseqs[n][0] - offset if n in oseqs else None
            if n in oseqs and n not in rs:
                short = oseqs[n][1] - oseqs[n][0] < step
                seq_rows.append((n, "-", o0, "ours only" + (" (short-lived, below the sample step: ignored)" if short else "")))
                if short:
                    continue
                v["seq"][0] += 1
                v["seq"][1] += 1
                vnotes["seq"].append("%s only in ours (start R%d)" % (n, o0))
            elif n in rs and n not in oseqs:
                seq_rows.append((n, r0, "-", "ps1 only"))
                v["seq"][0] += 1
                v["seq"][1] += 1
                vnotes["seq"].append("%s only in ps1 (start R%d)" % (n, r0))
            else:
                ok = abs(r0 - o0) <= max(step, 2)
                seq_rows.append((n, r0, o0, "ok" if ok else "start differs by %d" % (o0 - r0)))
                v["seq"][1] += 1
                if not ok:
                    v["seq"][0] += 1
                    vnotes["seq"].append("%s starts ps1 R%d vs ours R%d" % (n, r0, o0))

    full = []
    peak = (0, None)
    first_draw = None
    for f in frames:
        rt = ref[f]
        tk = nearest(ours, f + offset, step) if ours else None
        ot = ours.get(tk) if tk is not None else None
        line = {"F": f, "tick": tk}
        rn = len(rt["objs"])
        on = len(ot["objs"]) if ot else None
        line["objs"] = (rn, on)
        if rt["seqs"] and first_draw is None:  # the cast's first frame (idle stage emitters before it are not the effect)
            first_draw = f
        if rn > peak[0]:
            peak = (rn, f)
        # object counts + first object
        if ot is not None and (rn or on):
            gr, go = by_em(rt["objs"]), by_em(ot["objs"])
            stage_peak[0] = max(stage_peak[0], sum(len(gr.pop(e)) for e in [e for e in gr if e in stage]))
            stage_peak[1] = max(stage_peak[1], sum(len(go.pop(e)) for e in [e for e in go if e in stage]))
            for e in sorted(set(gr) | set(go)):
                a, b = len(gr.get(e, [])), len(go.get(e, []))
                v["cnt"][1] += 1
                if abs(a - b) > TOL_COUNT:
                    note(f, "cnt", "%s ps1 %d ours %d" % (e, a, b))
                if e in gr and e in go and abs(a - b) <= TOL_COUNT:
                    x, y = gr[e][0], go[e][0]
                    for key in ("pos", "sc", "col"):
                        for i in range(3):
                            v["first"][1] += 1
                            if not close(x[key][i], y[key][i]):
                                note(f, "first", "%s first.%s[%d] ps1 %d ours %d" % (e, key, i, x[key][i], y[key][i]))
                    v["first"][1] += 1
                    if not close(x["life"], y["life"], floor=2):
                        note(f, "first", "%s first.life ps1 %d ours %d" % (e, x["life"], y["life"]))
        # units
        if ot is not None and ot["units"] and rt["units"]:
            for u, a in sorted(rt["units"].items(), key=lambda kv: int(kv[0])):
                b = ot["units"].get(u)
                if b is None:
                    continue
                v["units"][1] += 1
                bad = []
                if max(abs(a["pos"][i] - b["pos"][i]) for i in range(3)) > TOL_POS:
                    bad.append("pos ps1 %s ours %s" % (a["pos"], b["pos"]))
                dy = (a["yaw"] - b["yaw"]) & 0xFFF
                if min(dy, 4096 - dy) > TOL_POS:
                    bad.append("yaw ps1 %d ours %d" % (a["yaw"] & 0xFFF, b["yaw"]))
                if (a["fl"] ^ b["fl"]) & 0x0301:
                    bad.append("flags7e ps1 %04x ours %04x" % (a["fl"], b["fl"]))
                if max(abs(a["col"][i] - b["col"][i]) for i in range(3)) > 8:
                    bad.append("col ps1 %s ours %s" % (a["col"], b["col"]))
                if bad:
                    note(f, "units", "u%s " % u + "; ".join(bad))
        # camera / fog / warp
        if ot is not None and ot["cam"] and rt["cam"]:
            v["cam"][1] += 1
            ce, oe, ct, ot_ = rt["cam"]["eye"], ot["cam"]["eye"], rt["cam"]["tgt"], ot["cam"]["tgt"]
            if max(abs(ce[i] - oe[i]) for i in range(3)) > TOL_POS or max(abs(ct[i] - ot_[i]) for i in range(3)) > TOL_POS:
                note(f, "cam", "eye ps1 %s ours %s tgt ps1 %s ours %s H %d/%d" % (ce, oe, ct, ot_, rt["cam"]["H"], ot["cam"]["H"]))
        if ot is not None and ot["fog"] and rt["fog"] and f >= fog_from:
            v["fog"][1] += 1
            a, b = rt["fog"], ot["fog"]
            if not (close(a["near"], b["near"]) and close(a["far"], b["far"]) and all(close(a["rgb"][i], b["rgb"][i], floor=4) for i in range(3))):
                note(f, "fog", "ps1 near %d far %d rgb %s ours near %d far %d rgb %s" % (a["near"], a["far"], a["rgb"], b["near"], b["far"], b["rgb"]))
        if ot is not None and ot["warp"] and rt["warp"] and f >= warp_from:
            v["warp"][1] += 1
            a, b = rt["warp"], ot["warp"]
            bad = (bool(a["on"]) != bool(b["on"])) or (a["on"] and (max(abs(a["ang"][i] - b["ang"][i]) for i in range(3)) > TOL_POS or max(abs(a["tr"][i] - b["tr"][i]) for i in range(3)) > TOL_POS))
            if bad:
                note(f, "warp", "ps1 on %d ang %s tr %s ours on %d ang %s tr %s" % (a["on"], a["ang"], a["tr"], b["on"], b["ang"], b["tr"]))
        full.append(line)

    def verdict(k):
        bad, n = v[k]
        if n == 0:
            return "-"
        return "MATCH (%d compared)" % n if bad == 0 else "DIFF (%d of %d)" % (bad, n)

    names = [("seq", "sequences"), ("cnt", "object counts"), ("first", "first-object numbers"), ("units", "unit positions / yaw / flags / colour"),
             ("cam", "camera eye / target"), ("fog", "fog"), ("warp", "warp")]
    md = ["# fx_diff %s" % d.replace("\\", "/"), "",
          "offset (our tick = ps1 refresh time + offset): **%s** (%s); ps1 states %d (refresh times %s..%s), compared window: %s, our samples %d, step %d" % (
              offset, off_note, len(ref), min(ref) if ref else "-", max(ref) if ref else "-", win_note, len(ours), step), "",
          "## Verdict", ""]
    for k, label in names:
        md.append("- %s: **%s**" % (label, verdict(k)))
        for s in vnotes[k][:4]:
            md.append("    - %s" % s)
    md.insert(md.index("## Verdict") + 2, "- stage emitters (baseline, not counted): %s; peak objects ps1 %d ours %d" % (", ".join(sorted(stage)) or "none", stage_peak[0], stage_peak[1]))
    md += ["", "## Sequences (ps1 start = refresh time - elapsed; ours = first command tick - offset)", "", "| name | ps1 | ours | note |", "|---|---|---|---|"]
    for r in seq_rows:
        md.append("| %s | %s | %s | %s |" % r)
    md += ["", "## Differences per frame", "(category lines camera / fog / warp / units first, then the counts; capped at 12 lines per frame, the uncapped list is diff_full.md)", ""]
    prio = {"cam": 0, "fog": 1, "warp": 2, "units": 3, "seq": 4, "cnt": 5, "first": 6}

    def ordered(f):
        return sorted(diffs_by_frame[f], key=lambda s: prio.get(s.split(":", 1)[0], 9))   # stable: the within-category order is kept

    if not diffs_by_frame:
        md.append("(none)")
    full_md = ["# fx_diff %s (uncapped differences per frame)" % d.replace("\\", "/"), ""]
    for f in sorted(diffs_by_frame):
        hdr = "- R%d (tick %s)" % (f, next((l["tick"] for l in full if l["F"] == f), "-"))
        md.append(hdr)
        full_md.append(hdr)
        lst = ordered(f)
        for s in lst[:12]:
            md.append("    - %s" % s)
        if len(lst) > 12:
            md.append("    - ... %d more (diff_full.md)" % (len(lst) - 12))
        for s in lst:
            full_md.append("    - %s" % s)
    md += ["", "<details><summary>Full per-frame table</summary>", "", "| ps1 R | our tick | objs ps1 | objs ours | eye ps1 | eye ours | tgt ps1 | tgt ours | fog ps1 | fog ours | warp ps1/ours |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    for l in full:
        rt = ref[l["F"]]
        ot = ours.get(l["tick"]) if l["tick"] is not None else None
        def g(t, k, sub):
            return "-" if not t or not t.get(k) else " ".join(str(x) for x in t[k][sub]) if isinstance(t[k][sub], list) else str(t[k][sub])
        def fg(t):
            return "-" if not t or not t.get("fog") else "%d/%d %s" % (t["fog"]["near"], t["fog"]["far"], t["fog"]["rgb"])
        md.append("| %d | %s | %d | %s | %s | %s | %s | %s | %s | %s | %s/%s |" % (
            l["F"], l["tick"] if l["tick"] is not None else "-", l["objs"][0], l["objs"][1] if l["objs"][1] is not None else "-",
            g(rt, "cam", "eye"), g(ot, "cam", "eye"), g(rt, "cam", "tgt"), g(ot, "cam", "tgt"), fg(rt), fg(ot),
            rt["warp"]["on"] if rt.get("warp") else "-", ot["warp"]["on"] if ot and ot.get("warp") else "-"))
    md += ["", "</details>", ""]
    with open(os.path.join(d, "diff_full.md"), "w", encoding="utf8") as fh:
        fh.write("\n".join(full_md + [""]))
    with open(os.path.join(d, "diff.md"), "w", encoding="utf8") as fh:
        fh.write("\n".join(md))
    print("\n".join(md[:md.index("## Sequences (ps1 start = refresh time - elapsed; ours = first command tick - offset)")]))

    # shots.png
    try:
        from PIL import Image, ImageDraw
        picks = [f for f in (first_draw, peak[1]) if f is not None]
        used_ref, used_ours = set(), set()
        if len(picks) == 2 and picks[0] == picks[1]:
            picks = picks[:1]
        rowsimg = []
        missing_pane = False
        for f in picks:
            tk = f + offset
            tk -= tk % step if step else 0
            fr = ref[f]["F"]
            rp = os.path.join(d, "ref", "frames", "frame_%06d.png" % fr)
            if not os.path.isfile(rp):  # nearest ps1 frame on disk
                fd = os.path.join(d, "ref", "frames")
                cand = [int(x[6:12]) for x in os.listdir(fd) if x.startswith("frame_")] if os.path.isdir(fd) else []
                if cand:
                    rp = os.path.join(fd, "frame_%06d.png" % min(cand, key=lambda c: abs(c - fr)))
            op = os.path.join(d, "ours", "shots", "shot_%06d.png" % tk)
            used_ref.add(os.path.basename(rp))
            used_ours.add(os.path.basename(op))
            ims = []
            for pth, lab in ((rp, "ps1 R%d F%d" % (f, fr)), (op, "ours tick %d" % tk)):
                if os.path.isfile(pth):
                    im = Image.open(pth).convert("RGB")
                    im = im.resize((int(im.width * 360 / im.height), 360))
                else:
                    im = Image.new("RGB", (480, 360), (40, 0, 0))
                if not os.path.isfile(pth):
                    missing_pane = True
                ImageDraw.Draw(im).text((6, 6), lab + (" (missing)" if not os.path.isfile(pth) else ""), fill=(255, 255, 0))
                ims.append(im)
            row = Image.new("RGB", (sum(i.width for i in ims), 360))
            x = 0
            for i in ims:
                row.paste(i, (x, 0))
                x += i.width
            rowsimg.append(row)
        if rowsimg:
            sheet = Image.new("RGB", (max(r.width for r in rowsimg), 360 * len(rowsimg)))
            for i, r in enumerate(rowsimg):
                sheet.paste(r, (0, 360 * i))
            if not (missing_pane and os.path.isfile(os.path.join(d, "shots.png"))):  # a pruned re-run keeps the sheet it made from the full set
                sheet.save(os.path.join(d, "shots.png"))
    except Exception as e:
        print("fx_diff: shots.png skipped (%s)" % e)
        used_ref = used_ours = None
    if do_prune:
        prune(d, used_ref, used_ours)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
