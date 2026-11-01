"""Reference casts of battle effects from the original PlayStation game in stock DuckStation.

    python -X utf8 tools/py/ps1_cast.py batch  --tag T --ids a,b [--only id] [--frames N] [--keys "Right:565,Circle:590"]
    python -X utf8 tools/py/ps1_cast.py summon --tag T --ids 43
    python -X utf8 tools/py/ps1_cast.py tech   --char N --ids 163 [--tag T]
    python -X utf8 tools/py/ps1_cast.py enemy  --scene N [--area A] [--frames N] [--poke ADDR=VAL:W;..] [--menu-frames N] [--menu-mash]
                                               [--pick U/E,..] [--window N] [--keep-pass1] [--mash Button] [--max-caps N]
                                               [--disc 1|2|PATH | --disc2]
                                               [--p2-every N --p2-lo A --p2-hi B] [--p2-dump N] [--party-ids a,b] [--forge-args "x|y"]
    python -X utf8 tools/py/ps1_cast.py force  --scene N --ids e[:unit[:g+g]],.. [--char C [--party-ids a,b]] [--unit U] [--target T] [--window N] [--area A] [--menu-frames N]
                                               [--field-poke ADDR=VAL[:W];..] [--start-limit N]
    python -X utf8 tools/py/ps1_cast.py attack --tag T [--keys "Circle:40,Circle:80,..."] [--frames 560]

force queues one target; --target-mask 3+4 writes every unit an all-targets element hits, as the menu does (ocbattleview hits every present unit of the
target's side that the all-target cast mask allows, so give those enemies). force checks every caster, group member and the target against the scene's units before it queues anything (a unit that is absent, hidden or
dead would hang the cast): unit numbers are the script's bind_unit operand minus 0xE0, and the refusal lists the valid units. A cast that does not
start within --start-limit run frames, or a GDB timeout, fails that one cast (captures.txt says so) and the batch goes on. --field-poke writes
field-state memory (field script variables) before the battle id is poked, e.g. 0x80077DE8=2:2 sets field variable 0x1C8. The manifest records
`area:` (battlefield operand) and `party_ids:` (character ids of the party) for tools/ours_cast34.ps1.

Output per element id: tmp/r34/<id>/ref/{states/state_NNNNNN.sav (every 5 run frames), frames/frame_NNNNNN.png (every 4), manifest.txt}.
Run frame 0 = the state the capture stage starts from (the cast's target select up; the confirming Circle is the first input).
--keys Button:frame = extra presses at that run frame (e.g. a creature list opened by the cast).

Down-count rule: the character's grid row 0 is padded to its length N (cc_save_forge.py rowlen); the list cursor (RAM) reads the grid
column of the entry (the row's level-mask bits, which do not start at the same column for every character) and Down walks with wrap
until it reads the column of the wanted id. Every step is read from RAM, never from the PNGs.

The emulator is a private copy (tmp/ds_cast, GDB port 2348), so other drivers can keep using tools/duckstation.
--disc / --disc2: boot disc 2 (ps1ref.disc2()) for the scenes only disc 2 has (342-344, 458-471, 509). The base states are the
*_d2 ones of 'ps1_capture.py prep --disc2' (built on first use); a saved state re-inserts its own disc, so disc-1 states cannot
be used there. Applies to every mode.
"""
import argparse
import datetime
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ps1_capture as pc  # noqa: E402
import ps1_state as ps  # noqa: E402
import ps1ref  # noqa: E402
from ps1ref import BUTTONS, Session  # noqa: E402

ROOT = ps1ref.ROOT
CAST_DS = os.path.join(ROOT, "tmp", "ds_cast")
CAST_PORT = 2348
CAST_MCD = os.path.join(CAST_DS, "memcards", "SLPM-87395_1.mcd")
pc.MCD = CAST_MCD           # the forged card goes into the private emulator's folder
STATES = pc.STATES
OUT = os.path.join(ROOT, "tmp", "r34")
FILL_POOL = [1, 29, 36, 12, 19, 32, 2, 43, 55, 73, 88, 40, 25]
STD_ROW = [1, 29, 36, 12, 19, 32, 2, 43]
SUMMON_RING = {37: 0x20, 38: 0x20, 39: 0x08, 40: 0x08, 41: 0x01, 42: 0x01, 43: 0x10, 44: 0x10, 45: 0x02, 46: 0x02, 47: 0x04, 48: 0x04}
WATCH_FIELDS = "btl_frame,tgt_064b,act_kind,act_actor,tgt0,e05da,elem,fx_load,ovl,ovl0,party0,u0_hp,u3_hp,u4_hp,u5_hp,loc,btl_id,area".split(",")

log = pc.log


class CastError(RuntimeError):
    pass


def sess(state=None):
    return Session(pc.DISC, state=state, ds_dir=CAST_DS, port=CAST_PORT)


class Keys:
    """Scripted input: presses and mashes by run frame (hold 4 frames each)."""

    def __init__(self):
        self.ev = []

    def press(self, button, frame, hold=4):
        self.ev.append((frame, frame + 1, 1 << 30, hold, BUTTONS[button]))

    def mash(self, button, period, lo, hi, hold=4):
        self.ev.append((lo, hi, period, hold, BUTTONS[button]))

    def held(self, k):
        m = 0
        for lo, hi, per, hold, mask in self.ev:
            if lo <= k < hi and (k - lo) % per < hold:
                m |= mask
        return m


def parse_pokes(spec):
    out = []
    for item in [x for x in (spec or "").split(";") if x]:
        a, v = item.split("=")
        w = 1
        if ":" in v:
            v, w = v.split(":")
        out.append((int(a, 0), int(v, 0), int(w)))
    return out


def apply_pokes(g, pokes):
    for addr, val, w in pokes:
        g.poke(addr, val, w)
        log("poke %08X = %d (%d bytes)" % (addr, val, w))


def row_columns(char):
    """Grid columns of row 0 of the character (the set bits of its level mask, ascending): the element list's cursor values."""
    import cc_save_forge
    mask = cc_save_forge.level_masks(char)[0]
    return [b for b in range(16) if mask >> b & 1]


def row_list(char, ids):
    n = int(subprocess.run([sys.executable, "-X", "utf8", pc.FORGE, "rowlen", str(char), "0"], capture_output=True, text=True).stdout.strip())
    row = list(ids)
    for f in FILL_POOL:
        if len(row) >= n:
            break
        if f not in ids:
            row.append(f)
    return row


# ---- base states ------------------------------------------------------------------------------------------------------
def build_base(tag, char, row, party, extra):
    """Forge the card for (char, row, party, extra), boot to the first command menu and save the tagged base states."""
    names = {k: os.path.join(STATES, "%s_%s.sav" % (v, tag)) for k, v in
             (("enc", "field53_encounter"), ("pre", "bg00_prestart"), ("menu", "bg00_menu"))}
    pc.forge(os.path.join("tmp", "save.dat"), ",".join(str(x) for x in row), char, party, extra)
    with sess() as g:
        st = pc.Steps(g)
        pc.boot_to_load(g, st)
        st.wait("room 53 reached (loc 53)", lambda w: w["loc"] == 53, 3000)
        st.wait("A3 encounter fired (btl_id != 0, still ovl 6)", lambda w: w["btl_id"] != 0 and w["ovl_id"] == 6 and w["loc"] == 53, 3000)
        g.save_state(names["enc"])
        st.wait("battle overlay loaded, btl_frame 0", lambda w: w["ovl_id"] == 8 and w["btl_frame"] == 0, 3000)
        g.save_state(names["pre"])
        st.wait("first command menu (tgt_064b 1, btl_frame >= 150)", lambda w: w["ovl_id"] == 8 and w["tgt_064b"] == 1 and w["btl_frame"] >= 150, 3000)
        g.save_state(names["menu"])
        w = g.watch()
        if w["party0"] != char:
            raise RuntimeError("base for tag %s: party0 is %d, wanted character %d" % (tag, w["party0"], char))
        pc.write_manifest(os.path.join(STATES, "manifest_%s.txt" % tag), [
            "date: %s" % datetime.datetime.now().isoformat(timespec="seconds"),
            "card: character %d party %s row 0 = %s extra %s" % (char, party, ",".join(map(str, row)), list(extra)), ""], st.rows)
    log("base states for tag %s: %s" % (tag, ", ".join(os.path.basename(v) for v in names.values())))
    return names


def ensure_base(tag, char, row, party, extra, rebuild):
    """Paths of the (enc, menu) base states for this card; the standard ones when the card matches them, else tagged ones."""
    std = char == 0 and party in (None, 0) and not extra and list(row) == STD_ROW
    if pc.DISC_NO == 2:
        if std:
            tag = "d2"
        elif not tag.endswith("_d2"):
            tag += "_d2"
    if std and pc.DISC_NO == 2:
        names = [os.path.join(STATES, "%s_d2.sav" % v) for v in ("field53_encounter", "bg00_prestart", "bg00_menu")]
        if rebuild or not all(os.path.isfile(p) for p in names):
            log("disc-2 base states missing: running prep --disc2")
            pc.prep(argparse.Namespace(save=os.path.join("tmp", "save.dat"), tag="d2", row0="", disc="2", ds_dir=CAST_DS, port=CAST_PORT))
        return names[0], names[2]
    if std:
        return os.path.join(STATES, "field53_encounter.sav"), os.path.join(STATES, "bg00_menu.sav")
    cfg = "char=%d party=%s extra=%s row=%s" % (char, party, list(extra), ",".join(map(str, row)))
    names = {k: os.path.join(STATES, "%s_%s.sav" % (v, tag)) for k, v in (("enc", "field53_encounter"), ("menu", "bg00_menu"))}
    cfgf = os.path.join(STATES, "bg00_menu_%s.cfg" % tag)
    if not rebuild and all(os.path.isfile(p) for p in names.values()) and os.path.isfile(cfgf) and open(cfgf).read() == cfg:
        return names["enc"], names["menu"]
    build_base(tag, char, row, party, extra)
    open(cfgf, "w").write(cfg)
    return names["enc"], names["menu"]


# ---- capture helpers --------------------------------------------------------------------------------------------------
def write_manifest(refdir, elem, mode, lines):
    sd = os.path.join(refdir, "states")
    paths = ps.list_states([sd])
    hdr = ["date: %s" % datetime.datetime.now().isoformat(timespec="seconds"), "emulator: stock DuckStation (ps1_cast.py)",
           "element: %s" % elem, "mode: %s" % mode] + lines
    m = ps.manifest(paths, int(elem) if str(elem).isdigit() else None).split("\n") if paths else []
    cam = ps.campick(paths) if paths else ""
    w = []
    for p in paths:
        r = ps.State(p).watch()
        w.append("%6d %s" % (ps.frame_of(p), ps.fmt_row({k: r.get(k) for k in WATCH_FIELDS})))
    with open(os.path.join(refdir, "manifest.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(hdr + m + ["cam_pick: " + cam,
                 "frames: frames\\frame_NNNNNN.png = run frame N (every 4); states\\state_NNNNNN.sav = run frame N (every 5); "
                 "run frame 0 = the base state of the stage-2 run (see base:)", "", "watch (tools\\py\\ps1_state.py watch states):"] + w) + "\n")


def prune_capture(refdir, elem, no_prune):
    if no_prune:
        return True
    sd = os.path.join(refdir, "states")
    first, last = ps.window(ps.list_states([sd]), int(elem))
    if first is None:
        log("WARNING: no effect window found in %s (kept everything)" % sd)
        return False
    ps.main(["prune", sd, str(max(0, first - 20)), str(last + 20)])
    return True


def retry(fn, *args):
    """A hotkey posted to the window is occasionally missed: repeat it (the emulator stays paused at the frame hook)."""
    for i in range(4):
        try:
            return fn(*args)
        except TimeoutError:
            log("hotkey capture timed out (try %d)" % (i + 1))
    return fn(*args)


def capture_run(g, refdir, frames, state_every=5, dump_every=4, lo=0, hi=None, keys=None, mark=None, expect=None):
    """Stepped capture from the current frame: state every `state_every` and frame every `dump_every` run frames.
    mark = {run frame: callback(g)}. expect = (element, run frame limit): CastError as soon as the element has not become the running action by
    then. Returns the first run frame with fx_load != 0 and the act_elem seen there."""
    os.makedirs(os.path.join(refdir, "states"), exist_ok=True)
    os.makedirs(os.path.join(refdir, "frames"), exist_ok=True)
    hi = frames if hi is None else hi
    seen = {"fx": None, "elem": None, "kind": None}
    started = expect is None
    for r in range(frames + 1):
        if state_every and lo <= r <= hi and r % state_every == 0:
            retry(g.save_state, os.path.join(refdir, "states", "state_%06d.sav" % r))
            w = g.watch(["act_kind", "act_elem", "e05da", "fx_load"])
            if not started and w["e05da"] == expect[0] and w["act_kind"] == 1:
                started = True
            if not started and r >= expect[1]:
                raise CastError("the forced element %d did not start within %d run frames (e05da %d, act_kind %d, fx_load %d); the caster is probably "
                                "not taking turns" % (expect[0], expect[1], w["e05da"], w["act_kind"], w["fx_load"]))
            if seen["fx"] is None and w["act_kind"] == 1 and w["fx_load"] != 0:
                seen["fx"], seen["elem"], seen["kind"], seen["act_elem"] = r, w["e05da"], w["act_kind"], w["act_elem"]
        if dump_every and r % dump_every == 0:
            retry(g.screenshot, os.path.join(refdir, "frames", "frame_%06d.png" % r))
        if mark and r in mark:
            mark[r](g)
        if r < frames:
            g.step(keys.held(r) if keys else 0)
    return seen


# ---- PC casts (batch / summon / tech) -------------------------------------------------------------------------------------
def drive_cast(g, cid, idx, cols, extra_keys, steps, limit=3000):
    """Menu driver from the first command menu to the confirmed cast, every step gated on RAM (no fixed frame numbers: the enemy's
    turn order differs between runs, so the HIT% box opens at different times).

    Command menu (tgt_064b 1 + menu_064a 1): Circle -> target select (tgt 2) -> Circle -> HIT% box (tgt 3). A box whose gauge is -1
    (u0_gauge) gets Circle on the weak row; once a command menu has been seen with gauge >= 0 the next box has the Element row
    (the box shown right after a hit is that hit's animation and is ignored). Element row = cursor 3 (cur_064e): Up until it is
    there, Circle. The element list shows the cursor as cur_064e = the grid column of the entry (cols: the set bits of the row's level
    mask, ascending; Serge's row starts at column 2, Zappa's at 5); Down walks with wrap ((idx - open) mod n presses,
    each checked against cur_064e). Circle -> target select (tgt 3, e05da = the selected id, checked): that state is returned, the
    confirming Circle is the first input of the capture.
    A cursor move blocks the next press for ~13 frames, so presses are spaced 14 frames apart."""
    B = BUTTONS
    GAP = 14
    cool, armed, in_list = 0, False, False
    walked = 0
    downs = None
    for k in range(limit):
        w = g.watch(["tgt_064b", "menu_064a", "cur_064e", "u0_gauge", "e05da", "act_kind"])
        held = 0
        if cool > 0:
            cool -= 1
        elif in_list and w["tgt_064b"] == 3:
            if w["e05da"] != cid:
                raise CastError("e05da %d at the target select, wanted %d (list index %d, columns %s)" % (w["e05da"], cid, idx, cols))
            steps["rows"].append("step: element %d selected (e05da), target select up, cast not yet confirmed, at frame %d" % (cid, g.frame))
            return k
        elif in_list and w["tgt_064b"] == 2:
            cur = w["cur_064e"]
            if downs is None:
                at = cols.index(cur) if cur in cols else -1
                downs = (idx - at) % len(cols)
                steps["rows"].append("step: element list open at index %d (cur_064e %d), %d Downs to index %d" % (at, cur, downs, idx))
                cool = GAP
            elif cur == cols[idx]:
                held, cool = B["Circle"], GAP
            elif walked > len(cols) + 2:
                raise CastError("Down walked %d times without reaching list index %d (cur_064e %d)" % (walked, idx, cur))
            else:
                held, cool = B["Down"], GAP
                walked += 1
        elif w["tgt_064b"] == 1 and w["menu_064a"] == 1:
            held, cool = B["Circle"], GAP
            if w["u0_gauge"] >= 0:
                armed = True
        elif w["tgt_064b"] == 2 and not in_list:
            held, cool = B["Circle"], GAP
        elif w["tgt_064b"] == 3 and not in_list:
            if w["u0_gauge"] < 0:
                held, cool = B["Circle"], GAP
            elif armed or w["act_kind"] == 0:   # a first menu that never re-arms (tech casts) still gets its target step
                if w["cur_064e"] == 3:
                    held, cool = B["Circle"], GAP
                    steps["elem_sent"] = g.frame
                else:
                    held, cool = B["Up"], GAP
        if steps.get("elem_sent") and not in_list and w["tgt_064b"] == 2 and w["cur_064e"] >= 2 and g.frame > steps["elem_sent"]:
            in_list = True
            steps["rows"].append("step: element list opened at frame %d" % g.frame)
            cool = GAP
            held = 0
        g.step(held)
    raise CastError("cast not confirmed within %d frames; watch %s" % (limit, g.watch()))


def capture_id(a, mode, cid, base, row, work):
    n = len(row)
    idx = row.index(cid)
    frames = a.frames if a.frames else (900 if mode == "summon" else 600)
    ref = os.path.join(OUT, str(cid), "ref")
    if os.path.isdir(ref):
        shutil.rmtree(ref)
    os.makedirs(ref)
    steps = {"rows": []}
    with sess(base) as g:
        w = g.watch(["ovl_id", "tgt_064b", "btl_frame"])
        if w["ovl_id"] != 8 or w["tgt_064b"] != 1:
            raise RuntimeError("base state %s is not at the battle menu: %s" % (base, w))
        if mode == "summon":
            if cid not in SUMMON_RING:
                raise SystemExit("no ring colour known for summon %d" % cid)
            for addr in (0x800DFF98, 0x800DFF9C, 0x800DFFA0):
                g.poke(addr, SUMMON_RING[cid], 4)
        if a.menu_poke:
            apply_pokes(g, parse_pokes(a.menu_poke))
        f0 = g.frame
        drive_cast(g, cid, idx, row_columns(a.char), a.keys, steps)
        w = g.watch(["e05da", "party0", "act_kind"])
        log("id %d: target select reached (stage 1 took %d frames): e05da=%d act_kind=%d party0=%d" % (cid, g.frame - f0, w["e05da"], w["act_kind"], w["party0"]))
        if w["e05da"] != cid:
            raise CastError("e05da %d at the target select, wanted %d" % (w["e05da"], cid))
        if mode == "tech" and w["party0"] != a.char:
            raise RuntimeError("party0 is %d, not character %d" % (w["party0"], a.char))
        keys = Keys()
        keys.press("Circle", 0)
        for item in [x for x in (a.keys or "").split(",") if x]:
            b, f = item.split(":")
            keys.press(b, int(f))
        if a.pre_poke:
            apply_pokes(g, parse_pokes(a.pre_poke))
        bh = g.watch(["u3_hp", "u4_hp", "u5_hp"])
        start_frame = g.frame
        seen = capture_run(g, ref, frames, keys=keys)
    ok = prune_capture(ref, cid, a.no_prune)
    if seen["fx"] is None:
        log("WARNING: id %d: fx_load never became non-zero during act_kind 1" % cid)
    elif seen["elem"] != cid and mode != "summon":
        log("WARNING: id %d: e05da was %d when the effect pack loaded" % (cid, seen["elem"]))
    bhs = " ".join("%s=%s" % kv for kv in bh.items())
    write_manifest(ref, cid, mode, [
        "row 0 of character %d: %s (list index %d, wrap length %d)" % (a.char, ",".join(map(str, row)), idx, n)] + steps["rows"] + [
        "base_hp: %6d %s" % (0, bhs),
        "base: run frame 0 of this capture = the target select of the cast, the confirming Circle is pressed at run frame 0 (stage 1 from %s, %d frames)" % (os.path.basename(base), start_frame - f0),
        "stage-1 e05da: %d" % w["e05da"],
        "ram checks: e05da %s (act_elem %s) with act_kind %s at the first fx_load != 0 (run frame %s)" % (seen["elem"], seen.get("act_elem"), seen["kind"], seen["fx"]),
        "window found: %s" % ok])
    log("captured %d -> %s" % (cid, ref))


def run_pc(a, mode):
    ids = [int(x) for x in a.ids.split(",") if x]
    if not 1 <= len(ids) <= 8:
        raise SystemExit("--ids: 1..8 element ids")
    tag = a.tag or ("t%d" % a.char if mode == "tech" else "")
    if not tag:
        raise SystemExit("--tag is required")
    # (the standard base card's row 0 is STD_ROW; a card whose row differs gets its own tagged base)
    char = a.char
    if char == 0 and not a.party_ids and not a.forge_args and all(i in STD_ROW for i in ids):
        row = list(STD_ROW)
    else:
        row = row_list(char, ids)
    extra = [x for x in (a.forge_args or "").split("|") if x]
    party = a.party_ids or char
    _, menu = ensure_base(tag, char, row, None if party == 0 else party, extra, a.rebuild)
    work = os.path.join(OUT, "_work", tag)
    os.makedirs(work, exist_ok=True)
    todo = [a.only] if a.only is not None else ids
    for cid in todo:
        if cid not in row:
            raise SystemExit("id %d is not in the row %s of tag %s" % (cid, row, tag))
        capture_id(a, mode, cid, menu, row, work)


# ---- attack -----------------------------------------------------------------------------------------------------------
def run_attack(a):
    if not a.tag:
        raise SystemExit("--tag is required")
    ks = a.keys or "Circle:40,Circle:80,Circle:200,Circle:300,Cross:420"
    frames = a.frames or 560
    k = Keys()
    for item in ks.split(","):
        b, f = item.split(":")
        k.press(b, int(f))
    ref = os.path.join(OUT, "attack_%s" % a.tag, "ref")
    if os.path.isdir(ref):
        shutil.rmtree(ref)
    menu = os.path.join(STATES, "bg00_menu.sav")
    with sess(menu) as g:
        w = g.watch(["ovl_id", "tgt_064b"])
        if w["ovl_id"] != 8 or w["tgt_064b"] != 1:
            raise RuntimeError("base state is not at the battle menu: %s" % w)
        capture_run(g, ref, frames, state_every=4, dump_every=4, keys=k)
    write_manifest(ref, "attack", "attack", ["keys: %s" % ks, "base: bg00_menu.sav"])
    log("captured attack (%s) -> %s" % (ks, ref))


# ---- enemy ------------------------------------------------------------------------------------------------------------
def run_enemy(a):
    if a.scene is None:
        raise SystemExit("--scene is required")
    scene = a.scene
    char = a.char
    extra = [x for x in (a.forge_args or "").split("|") if x]
    tag = a.tag or "scene%d" % scene
    if a.party_ids or extra or char or pc.DISC_NO == 2:
        std = not (a.party_ids or extra or char)
        enc, _ = ensure_base(tag, char, list(STD_ROW) if std else row_list(char, []), None if std else (a.party_ids or char), extra, a.rebuild)
    else:
        enc = os.path.join(STATES, "field53_encounter.sav")
    work = os.path.join(OUT, "_work", "scene_%d" % scene)
    scene_dir = os.path.join(OUT, "scene_%d" % scene)
    pass1 = os.path.join(work, "pass1")
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(os.path.join(pass1, "states"))
    os.makedirs(scene_dir, exist_ok=True)
    p1_frames = a.frames or 3000
    mash_k = Keys()
    mash_k.mash(a.mash, 30, 10, p1_frames)
    log("pass 1: %d frames, state every %d, no frames, mash %s" % (p1_frames, a.p1_every, a.mash))
    with sess(enc) as g:
        bid = 0x200 | scene
        g.poke(0x80075906, bid, 2)
        if a.area is not None and a.area >= 0:
            g.poke(0x800790FC, a.area, 2)
        log("battle id poked to 0x%X (area %s)" % (bid, a.area))
        if a.poke or a.var_poke:   # pokes need the running battle (the battle init would overwrite them in the field state)
            menu_k = Keys()
            if a.menu_mash:
                menu_k.mash(a.mash, 30, 10, a.menu_frames)
            cond = ((lambda w: w["ovl_id"] == 8 and 100 <= w["btl_frame"] < 100000) if a.menu_any else
                    (lambda w: w["ovl_id"] == 8 and w["tgt_064b"] == 1 and w["btl_frame"] >= 100))
            for k in range(a.menu_frames + 1):
                if k >= 100 and k % 5 == 0 and cond(g.watch(["ovl_id", "tgt_064b", "btl_frame"])):
                    break
                g.step(menu_k.held(k))
            else:
                raise RuntimeError("no battle menu reached from the poked encounter within %d frames: %s" % (a.menu_frames, g.watch()))
            pokes = parse_pokes(a.poke)
            if a.var_poke:   # battle-script variables: u16 at [0x800E2FC8] + 0x84 + (id & 0xFFFE)
                vb = g.gdb.u(0x800E2FC8, 4)
                for item in [x for x in a.var_poke.split(";") if x]:
                    vid, val = item.split("=")
                    pokes.append((vb + 0x84 + (int(vid, 0) & 0xFFFE), int(val, 0), 2))
            apply_pokes(g, pokes)
        for k in range(p1_frames + 1):
            if k % a.p1_every == 0:
                retry(g.save_state, os.path.join(pass1, "states", "state_%06d.sav" % k))
            if k < p1_frames:
                g.step(mash_k.held(k))
    tsv = os.path.join(scene_dir, "actions.tsv")
    ps.harvest(ps.list_states([os.path.join(pass1, "states")]), tsv)
    rows = []
    for line in open(tsv).read().splitlines()[1:]:
        if line:
            c = line.split("\t")
            rows.append(dict(start=int(c[0]), end=int(c[1]), unit=int(c[2]), elem=int(c[3]), target=int(c[4])))
    log("harvested %d action runs -> %s" % (len(rows), tsv))
    seen, jobs = set(), []
    picks = [x for x in (a.pick or "").split(",") if x]
    for r in rows:
        key = "%d/%d" % (r["unit"], r["elem"])
        if a.skip_existing and os.path.isfile(os.path.join(OUT, "%d_s%d_u%d" % (r["elem"], scene, r["unit"]), "ref", "manifest.txt")):
            continue
        if picks and key not in picks:
            continue
        if key in seen or len(jobs) >= a.max_caps:
            continue
        seen.add(key)
        jobs.append((r, max(0, (r["start"] - a.lead) // a.p1_every * a.p1_every)))
    if not a.keep_pass1:
        need = {"state_%06d.sav" % f0 for _, f0 in jobs}
        for f in os.listdir(os.path.join(pass1, "states")):
            if f not in need:
                os.remove(os.path.join(pass1, "states", f))
    report = []
    mash2 = Keys()
    mash2.mash(a.mash, 30, 10, a.window)
    for r, f0 in jobs:
        name = "%d_s%d_u%d" % (r["elem"], scene, r["unit"])
        ref = os.path.join(OUT, name, "ref")
        if os.path.isdir(ref):
            shutil.rmtree(ref)
        frm = os.path.join(pass1, "states", "state_%06d.sav" % f0)
        log("pass 2: element %d unit %d from pass-1 frame %d -> %s" % (r["elem"], r["unit"], f0, ref))
        rel = r["start"] - f0
        got = {}

        def probe(g):
            w = g.watch(["e05da", "act_actor"])
            got["e"], got["u"] = w["e05da"], w["act_actor"]
        with sess(frm) as g:
            capture_run(g, ref, a.window, state_every=a.p2_every, dump_every=a.p2_dump, lo=a.p2_lo,
                        hi=a.p2_hi if a.p2_hi > 0 else a.window, keys=mash2, mark={rel: probe} if 0 <= rel <= a.window else None)
        e, u = got.get("e", -1), got.get("u", -1)
        repro = e == r["elem"] and u == r["unit"]
        prune_capture(ref, r["elem"], a.no_prune)
        write_manifest(ref, r["elem"], "enemy", [
            "enemy_unit: %d scene %d" % (r["unit"], scene), "target_unit: %d" % r["target"],
            "base: pass-1 state frame %d (scene poke 0x80075906 = 0x%X, area %s, extra pokes: %s)" % (f0, 0x200 | scene, a.area, a.poke),
            "harvest: pass-1 start %d end %d; reproduction at run frame %d: e05da=%d act_actor=%d -> %s" % (
                r["start"], r["end"], rel, e, u, "OK" if repro else "MISMATCH")])
        report.append("%s\telem %d unit %d target %d repro %s" % (name, r["elem"], r["unit"], r["target"], "OK" if repro else "MISMATCH"))
    with open(os.path.join(scene_dir, "captures.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(report) + "\n")
    if not a.keep_pass1:
        shutil.rmtree(work, ignore_errors=True)
    log("done: %d captures" % len(jobs))


# ---- force ------------------------------------------------------------------------------------------------------------
def force_queue(g, unit, element, target, group=None, mask_units=None):
    """Writes one element action into the unit's action queue, as the script's queue_attack op does, so its next turn casts it.
    mask_units = every unit an all-targets element hits, as the menu's target ring writes them; default the one target."""
    vb = g.gdb.u(0x800E2FC8, 4)
    head_addr = vb + unit * 2 + 0x81C
    head = g.gdb.u(head_addr, 2)
    head = head - 0x10000 if head >= 0x8000 else head
    entry = vb + (head + 1) * 8 + unit * 0x28
    g.poke(vb + unit * 0x28 + 0x6B9, 0, 1)
    g.poke(entry + 0x6B8, 2 if group else 1, 1)
    for k in range(6 if group else 0):   # the caster list of a group cast
        g.poke(vb + 0x832 + k, group[k] if k < len(group) else 0xFF, 1)
    g.poke(entry + 0x6B6, element, 2)
    g.poke(entry + 0x6BA, 1, 1)
    mask = 0
    for u in mask_units or [target]:
        mask |= g.gdb.u(0x800DE90C + u * 2, 2)
    g.poke(entry + 0x6B4, mask, 2)
    g.poke(head_addr, head + 1, 2)


UNIT_BLOCK, UNIT_STRIDE, UNIT_MODEL, UNIT_COUNT = 0x800E06A0, 0x410, 0x800E0830, 9
RAM_END = 0x80200000


def read_units(g):
    """One dict per unit slot 0..8 of the running battle: current HP, model pointer, position and the reason the slot cannot act (or None)."""
    out = []
    for u in range(UNIT_COUNT):
        base = UNIT_BLOCK + u * UNIT_STRIDE
        hp = g.gdb.u(base + 0xB8, 2)
        model = g.gdb.u(UNIT_MODEL + u * UNIT_STRIDE, 4)
        row = {"unit": u, "hp": hp, "model": model, "x": 0, "z": 0, "why": None}
        if not (0x80010000 <= model < RAM_END) or model & 3:
            row["why"] = "no model (pointer 0x%08X): the slot is absent" % model
        else:
            row["x"] = g.gdb.u(model + 0x84, 4, True) >> 12
            row["z"] = g.gdb.u(model + 0x8C, 4, True) >> 12
            if hp == 0:
                row["why"] = "hp 0"
            elif g.gdb.u(base + 0x1C8, 2) or g.gdb.u(base + 0x1CC, 2) or g.gdb.u(model + 0x7E, 1) & 1:
                row["why"] = "off-stage / hidden"
        out.append(row)
    return out


def check_units(units, uses, scene):
    """uses = [(role, unit)]: SystemExit naming the first unit that cannot act or be hit, with the list of the valid ones."""
    for role, u in uses:
        row = units[u] if 0 <= u < len(units) else None
        why = "outside the unit slots 0..%d" % (UNIT_COUNT - 1) if row is None else row["why"]
        if why:
            valid = "; ".join("unit %d hp %d x %d z %d" % (r["unit"], r["hp"], r["x"], r["z"]) for r in units if not r["why"])
            raise SystemExit("unit %d (%s) cannot be used in scene %s: %s. Valid units: %s. Unit numbers are the script's bind_unit operand minus 0xE0, "
                             "not the entity number." % (u, role, scene, why, valid or "none"))


def run_force(a):
    extra = [x for x in (a.forge_args or "").split("|") if x]
    party = a.char or extra
    if (a.scene is None and not party) or not a.ids:
        raise SystemExit("--ids and --scene or --char are required")
    scene = a.scene
    tag = a.tag or "scene%s" % scene
    if party:
        enc = ensure_base(tag, a.char, [], a.party_ids or a.char, extra, a.rebuild)[0]
    elif pc.DISC_NO == 2:
        enc = ensure_base(tag, 0, list(STD_ROW), None, [], a.rebuild)[0]
    else:
        enc = os.path.join(STATES, "field53_encounter.sav")
    work = os.path.join(OUT, "_work", "force_%s" % tag)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    menu = os.path.join(work, "menu.sav")
    keys = Keys()
    keys.mash(a.mash, 30, 10, a.menu_frames)
    mask_units = [int(v) for v in a.target_mask.split("+")] if a.target_mask else None
    specs = []
    for spec in [x for x in a.ids.split(",") if x]:
        parts = spec.split(":")
        specs.append((int(parts[0]), int(parts[1]) if len(parts) > 1 else a.unit,
                      [int(v) for v in parts[2].split("+")] if len(parts) > 2 else None))
    with sess(enc) as g:
        if a.field_poke:   # field-state memory (script variables) must be set before the battle loads
            apply_pokes(g, parse_pokes(a.field_poke))
        if scene is not None:
            g.poke(0x80075906, 0x200 | scene, 2)
        if a.area is not None and a.area >= 0:
            g.poke(0x800790FC, a.area, 2)
        for k in range(a.menu_frames + 1):
            if k >= 100 and k % 5 == 0:
                w = g.watch(["ovl_id", "tgt_064b", "btl_frame"])
                if w["ovl_id"] == 8 and w["tgt_064b"] == 1 and w["btl_frame"] >= 100:
                    break
            g.step(keys.held(k))
        else:
            raise RuntimeError("no battle menu reached in scene %d: %s" % (scene, g.watch()))
        units = read_units(g)
        for elem, unit, group in specs:
            check_units(units, [("caster of element %d" % elem, unit)] + [("group member of element %d" % elem, m) for m in group or []] +
                        [("target", a.target)] + [("target", m) for m in mask_units or []], scene)
        area = g.gdb.u(0x800790FC, 2)
        party_ids = a.party_ids or ",".join(str(g.gdb.u(0x800786F4 + i, 1)) for i in range(3))
        retry(g.save_state, menu)
    report = []
    for elem, unit, group in specs:
        name = "%d" % elem if party else "%d_s%d_u%d" % (elem, scene, unit)
        ref = os.path.join(OUT, name, "ref")
        if os.path.isdir(ref):
            shutil.rmtree(ref)
        try:
            with sess(menu) as g:
                force_queue(g, unit, elem, a.target, group, mask_units)
                seen = capture_run(g, ref, a.window, state_every=5, dump_every=4, expect=(elem, min(a.start_limit, a.window)))
        except (CastError, TimeoutError, ConnectionError, OSError, RuntimeError) as e:   # the session closed its emulator; the next cast starts a fresh one
            report.append("%s\telem %d unit %d target %d FAILED %s: %s" % (name, elem, unit, a.target, type(e).__name__, e))
            log(report[-1])
            continue
        ok = seen["elem"] == elem
        prune_capture(ref, elem, a.no_prune)
        write_manifest(ref, elem, "tech" if party else "enemy", ["enemy_unit: %d scene %s" % (unit, scene), "target_unit: %d" % a.target,
                                            "area: %d" % area, "party_ids: %s" % party_ids,
                                            "base: first command menu of scene %s, action queue written for unit %d" % (scene, unit),
                                            "forced cast: e05da=%s at run frame %s -> %s" % (seen["elem"], seen["fx"], "OK" if ok else "MISMATCH")])
        report.append("%s\telem %d unit %d target %d repro %s" % (name, elem, unit, a.target, "OK" if ok else "MISMATCH"))
        log(report[-1])
    os.makedirs(os.path.join(OUT, "scene_%s" % scene), exist_ok=True)
    with open(os.path.join(OUT, "scene_%s" % scene, "captures.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(report) + "\n")
    shutil.rmtree(work, ignore_errors=True)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=["batch", "summon", "tech", "enemy", "attack", "force"])
    ap.add_argument("--unit", type=int, default=3)
    ap.add_argument("--target", type=int, default=0)
    ap.add_argument("--target-mask", default="", help="force: a+b.. the units an all-targets element hits (the default is --target alone)")
    ap.add_argument("--ds-dir", default=None, help="emulator folder (default: the private copy tmp/ds_cast, else tools/duckstation)")
    ap.add_argument("--port", type=int, default=None, help="GDB port of that folder (default 2348 for the private copy, 2345 otherwise)")
    ap.add_argument("--tag", default="")
    ap.add_argument("--ids", default="")
    ap.add_argument("--only", type=int, default=None)
    ap.add_argument("--char", type=int, default=0)
    ap.add_argument("--scene", type=int, default=None)
    ap.add_argument("--area", type=int, default=None)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--window", type=int, default=500)
    ap.add_argument("--lead", type=int, default=150)
    ap.add_argument("--poke", default="")
    ap.add_argument("--var-poke", default="")
    ap.add_argument("--field-poke", default="", help="force: ADDR=VAL[:W];.. written in the field state before the battle id (field script variables)")
    ap.add_argument("--start-limit", type=int, default=400, help="force: run frames the forced cast may take to start before the capture is given up")
    ap.add_argument("--menu-poke", default="")
    ap.add_argument("--pre-poke", default="")
    ap.add_argument("--menu-frames", type=int, default=320)
    ap.add_argument("--menu-mash", action="store_true")
    ap.add_argument("--menu-any", action="store_true")
    ap.add_argument("--pick", default="")
    ap.add_argument("--keep-pass1", action="store_true")
    ap.add_argument("--no-prune", action="store_true")
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--mash", default="Circle")
    ap.add_argument("--max-caps", type=int, default=99)
    ap.add_argument("--p1-every", type=int, default=30)
    ap.add_argument("--p2-every", type=int, default=5)
    ap.add_argument("--p2-lo", type=int, default=0)
    ap.add_argument("--p2-hi", type=int, default=0)
    ap.add_argument("--p2-dump", type=int, default=4)
    ap.add_argument("--party-ids", default="")
    ap.add_argument("--forge-args", default="")
    ap.add_argument("--keys", default="")
    ap.add_argument("--disc", default="1", help="1, 2 or an image path")
    ap.add_argument("--disc2", action="store_true", help="same as --disc 2")
    a = ap.parse_args(argv)
    global CAST_DS, CAST_PORT, CAST_MCD
    if a.ds_dir:
        CAST_DS = os.path.abspath(a.ds_dir)
        CAST_PORT = a.port or (2348 if os.path.basename(CAST_DS) == "ds_cast" else 2345)
    elif not os.path.isdir(CAST_DS):
        CAST_DS, CAST_PORT = ps1ref.DS_DIR, ps1ref.GDB_PORT
    if a.port:
        CAST_PORT = a.port
    CAST_MCD = os.path.join(CAST_DS, "memcards", "SLPM-87395_1.mcd")
    pc.MCD = CAST_MCD
    pc.set_disc("2" if a.disc2 else a.disc)
    if not os.path.isdir(CAST_DS):
        raise SystemExit("private emulator folder %s missing (copy tools/duckstation, set GDBServerPort = %d)" % (CAST_DS, CAST_PORT))
    if a.mode == "force":
        run_force(a)
    elif a.mode == "enemy":
        run_enemy(a)
    elif a.mode == "attack":
        run_attack(a)
    else:
        run_pc(a, a.mode)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
