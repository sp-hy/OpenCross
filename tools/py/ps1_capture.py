"""Reference captures of the original PlayStation game in stock DuckStation.

    python -X utf8 tools/py/ps1_capture.py prep  [--save tmp/save.dat] [--tag TAG] [--row0 e,e,..] [--disc 1|2|PATH | --disc2]
    python -X utf8 tools/py/ps1_capture.py scene <name> [--state-every N] [--dump-every N] [--disc 1|2|PATH]
    python -X utf8 tools/py/ps1_capture.py cast  batch|summon|tech|enemy|attack [--tag T] [--ids a,b] [--char N] ...

prep     forges the memory card, boots the disc, drives the title screen and the Load menu and writes the base states
         tmp/ref/states/{field53_encounter,bg00_prestart,bg00_menu}[_TAG].sav (+ manifest[_TAG].txt with the frame of every step).
scene    one named scene (element casts, serge_attack, bg00_zen0, bg21_cam4, golem, frogprince) -> tmp/ref/<scene>/.
cast     any battle effect -> tmp/r34/<id>/ref/{states,frames,manifest.txt} (batch / summon / tech / enemy / attack).

--disc    1 (default), 2 (the disc 2 image found by ps1ref.disc2()) or an image path. On disc 2 the forged card marks the save as
         a disc-2 game (script variable 0x42 = 1; the game refuses a disc-1 save there), goes to disc 2's own per-game card file
         (SLPM-87396_1.mcd) and the default tag is "d2", so the base
         states are *_d2.sav. A saved state re-inserts the disc it was made on, so disc-2 states must be made on disc 2.

Every menu step is a press followed by run_until on a RAM value (never the PNGs); the frame each step happened at is
written to the manifest. One emulator at a time.
"""
import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ps1ref  # noqa: E402
from ps1ref import Session  # noqa: E402

ROOT = ps1ref.ROOT
REF = os.path.join(ROOT, "tmp", "ref")
STATES = os.path.join(REF, "states")
MCD = os.path.join(ps1ref.DS_DIR, "memcards", "SLPM-87395_1.mcd")
FORGE = os.path.join(HERE, "cc_save_forge.py")
SERGE_STATS = "hp=1023,atk=1,def=127,hit=127,evd=0,mag=1,mdef=127"
STD_ROW0 = "1,29,36,12,19,32,2,43"
DISC = None      # image path for the Sessions of this module (None = disc 1)
DISC_NO = 1


def set_disc(spec):
    """spec: None / "1" (disc 1), "2" (ps1ref.disc2()) or an image path; sets DISC / DISC_NO and returns the path (None = default)."""
    global DISC, DISC_NO
    spec = None if spec in (None, "", "1", 1) else str(spec)
    if spec == "2":
        DISC = ps1ref.disc2()
        if not DISC:
            raise SystemExit("no disc 2 image found in %s (name containing 'Disc 2'), or give --disc PATH" % ps1ref.GAMES_DIR)
        DISC_NO = 2
    elif spec:
        DISC = spec
        low = os.path.basename(spec).lower()
        DISC_NO = 2 if ("disc 2" in low or "disc2" in low) else 1
    else:
        DISC, DISC_NO = None, 1
    return DISC


def log(msg):
    print("[%s] %s" % (datetime.datetime.now().strftime("%H:%M:%S"), msg), flush=True)


def rel(p):
    return os.path.relpath(p, ROOT)


def forge(save, row0, char=0, party=None, extra=(), grid1="37,43,19,1"):
    """Forged memory card: slot 2 of the owner's save copy at room 53 entrance 1, standard party and grid row."""
    save = save if os.path.isabs(save) else os.path.join(ROOT, save)
    if not os.path.isfile(save):
        raise SystemExit("save copy %s not found" % save)
    cmd = [sys.executable, "-X", "utf8", FORGE, save, "x", "--slot", "2", "--room", "53", "--entrance", "1",
           "--party", str(char if party is None else party)] + list(extra) + [
           "--char", "%d:%s" % (char, SERGE_STATS)]
    if row0:
        cmd += ["--grid", "%d:0:%s" % (char, row0)]
    if grid1 and row0:
        cmd += ["--grid", "%d:1:%s" % (char, grid1)]
    mcd = MCD
    if DISC_NO == 2:
        cmd += ["--var", "0x42=1"]        # story variable 0x42 = disc number - 1: Continue accepts the save on disc 2
        mcd = os.path.join(os.path.dirname(MCD), "SLPM-87396_1.mcd")   # the per-game card is named by the disc's serial
    cmd += ["--mcd", mcd, "--mcd-only"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("forge failed: " + r.stdout + r.stderr)
    log("forged memory card %s (row 0 = %s)" % (rel(mcd), row0))


class Steps:
    """Frame log of a driven sequence: every press and what it waited for, for the manifest."""

    def __init__(self, g):
        self.g = g
        self.rows = []

    def add(self, label, waited=None):
        self.rows.append("step: %-52s frame %6d%s" % (label, self.g.frame, "" if waited is None else " (waited %d)" % waited))
        log(self.rows[-1][6:])

    def do(self, label, button, cond, limit, hold=6, gap=0, settle=0):
        """Press `button`, then step until cond(watch) holds; fail loudly with the watch table if it never does.
        settle > 0: a menu that is still opening drops presses, so the press is repeated every `settle` frames
        (until `limit` frames have passed) while the condition is unmet."""
        t0 = self.g.frame
        presses = 0
        while True:
            self.g.press(button, hold, gap)
            presses += 1
            span = settle if settle else limit
            try:
                self.g.run_until(cond, min(span, limit - (self.g.frame - t0)) or 1)
                break
            except TimeoutError:
                if not settle or self.g.frame - t0 >= limit:
                    raise RuntimeError("step '%s' (%s) did not reach its condition within %d frames; frame %d, watch %s" % (
                        label, button, limit, self.g.frame, self.g.watch()))
        self.rows.append("step: %-52s frame %6d (waited %d, %d press%s)" % (label, self.g.frame, self.g.frame - t0, presses, "" if presses == 1 else "es"))
        log(self.rows[-1][6:])
        return self.g.frame - t0

    def wait(self, label, cond, limit, held=0):
        try:
            n = self.g.run_until(cond, limit, held)
        except TimeoutError:
            raise RuntimeError("wait '%s' did not reach its condition within %d frames; frame %d, watch %s" % (
                label, limit, self.g.frame, self.g.watch()))
        self.add(label, n)
        return n


def boot_to_load(g, st, file_no=3):
    """Cold boot to the Load menu with the forged card in slot 1 and the cursor on `file_no` (1-based; CHRONOnn is file nn + 1), then load it."""
    g.run(40)
    # Start skips the opening movie; the title ignores Start while it fades in, so press until the title answers
    # (title_var leaves 0 once the title took the press).
    for _ in range(40):
        g.press("Start", 6)
        done = False
        for _ in range(24):
            if g.watch(["title_var"])["title_var"]:
                done = True
                break
            g.step(0)
        if done:
            break
    else:
        raise RuntimeError("title never answered Start; watch %s" % g.watch())
    st.add("title accepted Start")
    st.wait("title menu up (title_var 1, mt_ovl 10)", lambda w: w["title_var"] == 1 and w["mt_ovl"] == 10, 400)
    g.run(30)
    g.press("Down", 6, 20)
    st.add("Down to Continue (verified by the card prompt below)")
    st.do("Circle: Continue -> card prompt", "Circle", lambda w: w["mt_mode"] == 1, 400, settle=40)
    st.do("Circle: card 1 -> memory-card check", "Circle", lambda w: w["mt_mode"] == 2, 400, settle=40)
    st.wait("file grid up (mt_mode 32)", lambda w: w["mt_mode"] == 32, 400)
    g.run(10)
    for i in range(1, file_no):
        st.do("Right -> file %d (ld_cur %d)" % (i + 1, i), "Right", lambda w, i=i: w["ld_cur"] == i, 200, gap=10, settle=40)
    st.do("Circle: load file %d" % file_no, "Circle", lambda w: w["mt_mode"] == 33, 200, settle=60)


def write_manifest(path, head, steps):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(head + steps) + "\n")


def save_base(g, name):
    """Base state plus a screenshot of its frame next to it (<name>.png)."""
    g.save_state(os.path.join(STATES, name))
    g.screenshot(os.path.join(STATES, name[:-4] + ".png"))


def prep(a):
    set_disc(a.disc)
    sfx = "_" + (a.tag or ("d2" if DISC_NO == 2 else "")) if (a.tag or DISC_NO == 2) else ""
    row0 = a.row0 or STD_ROW0
    os.makedirs(STATES, exist_ok=True)
    forge(a.save, row0)
    names = {"enc": "field53_encounter%s.sav" % sfx, "pre": "bg00_prestart%s.sav" % sfx, "menu": "bg00_menu%s.sav" % sfx}
    with Session(DISC, ds_dir=getattr(a, "ds_dir", ps1ref.DS_DIR), port=getattr(a, "port", ps1ref.GDB_PORT)) as g:
        st = Steps(g)
        boot_to_load(g, st)
        st.wait("room 53 reached (loc 53)", lambda w: w["loc"] == 53, 3000)
        st.wait("A3 encounter fired (btl_id != 0, still ovl 6)", lambda w: w["btl_id"] != 0 and w["ovl_id"] == 6 and w["loc"] == 53, 3000)
        save_base(g, names["enc"])
        st.add("saved " + names["enc"])
        st.wait("battle overlay loaded, btl_frame 0", lambda w: w["ovl_id"] == 8 and w["btl_frame"] == 0 and w["u0_hp"] == 1023 and w["u0_sta"] == 70, 3000)
        save_base(g, names["pre"])
        st.add("saved " + names["pre"])
        st.wait("first command menu (tgt_064b 1, btl_frame >= 150)", lambda w: w["ovl_id"] == 8 and w["tgt_064b"] == 1 and w["btl_frame"] >= 150, 3000)
        save_base(g, names["menu"])
        st.add("saved " + names["menu"])
        total = g.frame
    write_manifest(os.path.join(STATES, "manifest%s.txt" % sfx), [
        "date: %s" % datetime.datetime.now().isoformat(timespec="seconds"),
        "emulator: stock DuckStation, disc %s" % os.path.basename(DISC or ps1ref.default_disc() or "?"),
        "card: forged slot 2, room 53 entrance 1, grid row 0 = %s" % row0, "total frames from cold boot: %d" % total, ""], st.rows)
    log("base states: " + ", ".join(names.values()))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)   # scene / cast are dispatched below
    p = sub.add_parser("prep")
    p.add_argument("--save", default="tmp/save.dat")
    p.add_argument("--tag", default="")
    p.add_argument("--row0", default="")
    p.add_argument("--disc", default="1", help="1, 2 or an image path")
    p.add_argument("--disc2", action="store_true", help="same as --disc 2")
    if argv and argv[0] == "scene":      # tools/py/ps1_scene.py
        import ps1_scene
        return ps1_scene.main(argv[1:])
    if argv and argv[0] == "cast":       # tools/py/ps1_cast.py
        import ps1_cast
        return ps1_cast.main(argv[1:])
    a = ap.parse_args(argv)
    if a.cmd == "prep":
        if a.disc2:
            a.disc = "2"
        prep(a)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
