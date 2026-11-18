"""Scene captures of the original PlayStation game in stock DuckStation.

    python -X utf8 tools/py/ps1_scene.py <scene> [--state-every N] [--dump-every N] [--frames N] [--ds-dir D] [--port P]
                                           [--disc 1|2|PATH] [--region japan|usa]
    python -X utf8 tools/py/ps1_scene.py list

Each scene loads one of the base states (tmp/ref/states, or tmp/ref/states_usa on the USA build, written by
`ps1_capture.py prep`), drives the battle menu with presses that are each verified from RAM, then steps frame by frame and
writes tmp/ref/<scene>/ (tmp/ref/<scene>_usa/ on the USA build)
{frames/frame_NNNNNN.png, states/state_NNNNNN.sav, manifest.txt}. NNNNNN = game frames stepped since the capture started
(0 = the state the capture began at). One emulator at a time per (ds-dir, port).

Scenes: aqua_beam comet black_hole volcano uplift gravitonne aqua_ball (one element cast of Serge's grid row 0),
serge_attack (weak attack), bg00_zen0 (first frames of the bg_00 battle), bg21_cam4 (bg_21 intro: the field encounter
with the battlefield word poked to 21), golem / frogprince (summons: the ring is poked to the summon's colour).
"""
import argparse
import datetime
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ps1ref  # noqa: E402
import ps1_capture as cap  # noqa: E402
import ps1_state  # noqa: E402
from ps1_ram import RAM  # noqa: E402
from ps1ref import Session  # noqa: E402
from ps1_capture import Steps, log, rel, REF, STD_ROW0  # noqa: E402

LIST_OPEN_CELL = 3          # the element list opens on the 4th cell of the row; Down walks the row with wrap-around
FP_ROW0 = "1,29,36,12,19,32,2,37"
FP_GRID1 = "43,19,1"

# kind: elem (one cast), summon (ring poke + cast), attack, prestart, bg21. total/window: run frames stepped, and the part of
# them written out (the old driver wrote dumps and states only inside its window).
SCENES = {
    "volcano": dict(kind="elem", elem=12), "uplift": dict(kind="elem", elem=19), "gravitonne": dict(kind="elem", elem=32),
    "aqua_ball": dict(kind="elem", elem=2), "aqua_beam": dict(kind="elem", elem=1), "comet": dict(kind="elem", elem=29),
    "black_hole": dict(kind="elem", elem=36),
    "golem": dict(kind="summon", elem=43, ring=0x10, base="bg00_menu", row0=STD_ROW0),
    "frogprince": dict(kind="summon", elem=37, ring=0x20, base="bg00fp_menu", row0=FP_ROW0),
    "serge_attack": dict(kind="attack", total=200, window=(30, 130)),
    "bg00_zen0": dict(kind="prestart", total=320, window=(0, 320)),
    "bg21_cam4": dict(kind="bg21", total=200, window=(80, 200)),
}


def downs_for(elem, row0):
    cells = [int(x) for x in row0.split(",")]
    return (cells.index(elem) - LIST_OPEN_CELL) % len(cells)


class Scene:
    def __init__(self, name, a):
        self.name = name
        self.sc = SCENES[name]
        self.a = a
        self.ds = os.path.abspath(a.ds_dir)
        self.out = os.path.join(REF, name + ("_usa" if cap.REGION == "usa" else ""))
        self.events = []
        self.steps = None

    # -- menu driving (every step verified from RAM) ---------------------------------------------------------------
    def weak_hit(self, g, st, then_menu):
        """Attack -> target -> weak hit. then_menu: wait for the command menu to be back (menu_064a 1, gauge 0); the USA build
        keeps the HIT% box open instead, so there the hit taken (gauge 0) is enough."""
        st.do("Confirm: Attack -> target select (tgt_064b 2)", "Confirm", lambda w: w["tgt_064b"] == 2, 120, hold=4)
        g.run(100)
        st.do("Confirm: target -> HIT% box (tgt_064b 3)", "Confirm", lambda w: w["tgt_064b"] == 3, 200, hold=4)
        g.run(45)
        st.add("HIT% box settled (45 frames after tgt_064b 3)")
        if not then_menu:
            return
        if RAM.region == "usa":
            # The USA build keeps the HIT% box open after a hit, with the Element row added; no command menu comes back.
            st.do("Confirm: weak hit taken (u0_gauge 0, box stays open)", "Confirm", lambda w: w["u0_gauge"] == 0 and w["tgt_064b"] == 3, 300, hold=4)
            return
        st.do("Confirm: weak hit lands (u0_gauge 0, tgt_064b 1)", "Confirm", lambda w: w["u0_gauge"] == 0 and w["tgt_064b"] == 1, 300, hold=4)
        st.wait("command menu back (menu_064a 1)", lambda w: w["menu_064a"] == 1 and w["tgt_064b"] == 1, 400)

    def element_cast(self, g, st, elem, downs):
        if g.watch(["tgt_064b"])["tgt_064b"] == 3:
            # HIT% box still open (USA build): Element is its row 3. Presses are dropped while the hit plays, so each Down is
            # repeated until the cursor moves.
            while g.watch(["cur_064e"])["cur_064e"] != 3:
                prev = g.watch(["cur_064e"])["cur_064e"]
                st.do("Down in the HIT%% box (cur_064e %d)" % prev, "Down", lambda w, p=prev: w["cur_064e"] != p, 300, hold=4, gap=6, settle=20)
        else:
            st.do("Down to Element (cur_064e 1)", "Down", lambda w: w["cur_064e"] == 1, 60, hold=4, settle=20)
        st.do("Confirm: element list open (tgt_064b 2)", "Confirm", lambda w: w["tgt_064b"] == 2 and w["menu_064a"] == 0, 120, hold=4, settle=40)
        g.run(30)
        st.add("list settled (cursor cur_064e %d)" % g.watch(["cur_064e"])["cur_064e"])
        for i in range(downs):
            prev = g.watch(["cur_064e"])["cur_064e"]
            st.do("Down %d/%d in the element list" % (i + 1, downs), "Down", lambda w, p=prev: w["cur_064e"] != p, 60, hold=4, gap=6, settle=20)
        g.run(6)
        st.do("Confirm: element -> target (tgt_064b 3)", "Confirm", lambda w: w["tgt_064b"] == 3, 120, hold=4)
        got = g.watch(["e05da"])["e05da"]
        if got != elem:
            raise RuntimeError("selected element %d, wanted %d (watch %s)" % (got, elem, g.watch()))
        st.add("element %d selected (e05da)" % got)
        g.run(30)

    # -- capture ---------------------------------------------------------------------------------------------------
    def capture(self, g, total, window, presses=()):
        """Step `total` frames; PNG every dump_every and a state every state_every frames inside `window`."""
        a = self.a
        for sub in ("frames", "states"):
            os.makedirs(os.path.join(self.out, sub), exist_ok=True)
        pend = sorted(presses)
        seen = {}
        hp0 = None
        held_left = 0
        mask = 0
        for n in range(total + 1):
            if window[0] <= n <= window[1]:
                if n % a.dump_every == 0:
                    g.screenshot(os.path.join(self.out, "frames", "frame_%06d.png" % n))
                if n % a.state_every == 0:
                    g.save_state(os.path.join(self.out, "states", "state_%06d.sav" % n))
            if n == total:
                break
            if pend and pend[0][0] <= n:
                _, b, hold = pend.pop(0)
                held_left, mask = hold, ps1ref.BUTTONS[b]
                seen["press " + b] = n
            w = g.watch(["tgt_064b", "act_kind", "fx_load", "u3_hp"])
            if hp0 is None:
                hp0 = w["u3_hp"]
            for key, cond in (("tgt_064b 5 (cast pose)", w["tgt_064b"] == 5), ("act_kind 1", w["act_kind"] == 1),
                              ("fx_load nonzero", w["fx_load"] != 0), ("u3_hp changed", w["u3_hp"] != hp0)):
                if cond and key not in seen:
                    seen[key] = n
            g.step(mask if held_left > 0 else 0)
            held_left -= 1
        if presses and "tgt_064b 5 (cast pose)" not in seen:
            raise RuntimeError("the cast never started within %d frames (watch %s)" % (total, g.watch()))
        self.events = ["event: %-24s run frame %d" % (k, v) for k, v in seen.items()]
        for e in self.events:
            log(e)

    def manifest(self, lines):
        states = os.path.join(self.out, "states")
        rows = []
        fields = ("btl_frame", "tgt_064b", "act_kind", "e05da", "e3372", "fx_load", "ovl_id", "u0_sta", "u3_hp", "u4_hp", "u5_hp", "loc", "btl_id", "area")
        for p in ps1_state.list_states([states]):
            try:
                row = ps1_state.State(p).watch()
            except Exception as e:  # noqa: BLE001
                rows.append("%s: %s" % (p, e))
                continue
            rows.append("%6d %s" % (ps1_state.frame_of(p), ps1_state.fmt_row({k: row.get(k) for k in fields if k in row})))
        head = ["scene: " + self.name, "date: " + datetime.datetime.now().isoformat(timespec="seconds"),
                "emulator: stock DuckStation, disc %s (%s build)" % (os.path.basename(cap.DISC or ps1ref.default_disc() or "?"), cap.REGION)]
        text = head + lines + self.events + ["", "steps (frame = frames stepped since the base state was loaded):"] + self.steps.rows + \
            ["", "watch (tools\\py\\ps1_state.py watch states):"] + rows
        with open(os.path.join(self.out, "manifest.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(text) + "\n")

    def run(self):
        sc, name, a = self.sc, self.name, self.a
        if os.path.isdir(self.out):
            shutil.rmtree(self.out)
        os.makedirs(self.out)
        kind = sc["kind"]
        base = {"elem": "bg00_menu", "attack": "bg00_menu", "prestart": "bg00_prestart", "bg21": "field53_encounter"}.get(kind) or sc["base"]
        base_path = os.path.join(cap.STATES, base + ("_d2" if cap.DISC_NO == 2 else "") + ".sav")
        if kind == "summon" and not os.path.isfile(base_path):
            build_fp_base(self.ds, a.port, base_path)
        with Session(cap.DISC, state=base_path, ds_dir=self.ds, port=a.port) as g:
            self.steps = st = Steps(g)
            st.add("loaded " + base)
            lines = []
            if kind in ("elem", "summon"):
                if kind == "summon":
                    for slot in range(3):
                        g.poke(RAM.ring + 4 * slot, sc["ring"], 4)
                    st.add("ring poked to 0x%X x3" % sc["ring"])
                row0 = sc.get("row0", STD_ROW0)
                downs = downs_for(sc["elem"], row0)
                self.weak_hit(g, st, True)
                self.element_cast(g, st, sc["elem"], downs)
                st.add("target step settled; the cast Confirm is pressed at run frame 12 of the capture")
                total = a.frames or 360
                if kind == "elem":
                    lines = ["element: %d (pgdat), Serge row 0, Down x%d in the level-1 list" % (sc["elem"], downs),
                             "base: %s.sav + menu steps (see steps)" % base, "encounter: room 53 entrance 1, bg_00",
                             "frames: frames\\frame_NNNNNN.png = run frame N (320x240); the cast Confirm is pressed at run frame 12, the cast pose starts at run frame ~20 (tgt_064b 5), act_kind 1 and the pack load (fx_load) a little later",
                             "states: states\\state_NNNNNN.sav every %d frames (RAM/VRAM: ps1_state.py)" % a.state_every]
                else:
                    lines = ["summon: element %d, ring poked to 0x%X x3 (Down x%d)" % (sc["elem"], sc["ring"], downs),
                             "base: %s.sav + menu steps" % base, "states: every %d frames" % a.state_every]
                self.capture(g, total, (0, total), presses=[(12, "Confirm", 4)])
            elif kind == "attack":
                self.weak_hit(g, st, False)
                total, window = a.frames or sc["total"], sc["window"]
                lines = ["action: weak attack (row 1 of the HIT% box) with the Sea Swallow; Confirm at run frame 40, the hit lands at ~50 (u3_hp drops, u0_gauge 0)",
                         "base: HIT% box up, Serge at the target (bg00_menu.sav + two Confirms)", "encounter: as the element scenes (bg_00)",
                         "window: frames %d..%d written; states every %d frames" % (window[0], window[1], a.state_every)]
                self.capture(g, total, window, presses=[(40, "Confirm", 4)])
            elif kind == "prestart":
                total, window = a.frames or sc["total"], sc["window"]
                lines = ["base: bg00_prestart.sav (battle overlay loaded, battle frame counter 0); battle tick = run frame - 10"]
                self.capture(g, total, window)
            elif kind == "bg21":
                g.poke(RAM.area, 21, 2)
                st.add("battlefield word %08X poked to 21" % RAM.area)
                st.wait("bg_21 battle loaded (ovl 8, area 21, btl_frame 0)",
                        lambda w: w["ovl_id"] == 8 and w["area"] == 21 and w["btl_frame"] == 0 and w["u0_hp"] == 1023 and w["u0_sta"] == 70, 1500)
                g.save_state(os.path.join(cap.STATES, "bg21_prestart.sav"))
                g.screenshot(os.path.join(cap.STATES, "bg21_prestart.png"))
                st.add("saved bg21_prestart.sav")
                total, window = a.frames or sc["total"], sc["window"]
                lines = ["base: bg21_prestart.sav (poked encounter: area 21 -> bg_21); battle tick = run frame - 10; window frames %d..%d, states every %d" % (window[0], window[1], a.state_every)]
                self.capture(g, total, window)
            self.manifest(lines)
        log("done: " + rel(self.out))


def build_fp_base(ds, port, out_path):
    """bg00_menu variant whose grid row 0 holds Frog Prince (37) in the 8th cell: forged card, cold boot, battle menu."""
    cap.CARDS = os.path.join(ds, "memcards")
    cap.forge(os.path.join("tmp", "save.dat"), FP_ROW0, grid1=FP_GRID1)
    with Session(cap.DISC, ds_dir=ds, port=port) as g:
        st = Steps(g)
        cap.boot_to_load(g, st)
        st.wait("room 53 reached (loc 53)", lambda w: w["loc"] == 53, 3000)
        st.wait("first command menu (tgt_064b 1, btl_frame >= 150)", lambda w: w["ovl_id"] == 8 and w["tgt_064b"] == 1 and w["btl_frame"] >= 150, 3000)
        g.save_state(out_path)
        g.screenshot(out_path[:-4] + ".png")
        log("saved " + rel(out_path))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scene", help="scene name or 'list'")
    ap.add_argument("--state-every", type=int, default=4)
    ap.add_argument("--dump-every", type=int, default=1)
    ap.add_argument("--frames", type=int, default=0, help="override the scene's number of captured frames")
    ap.add_argument("--ds-dir", default=ps1ref.DS_DIR)
    ap.add_argument("--port", type=int, default=ps1ref.GDB_PORT)
    ap.add_argument("--disc", default="1", help="1, 2 or an image path (disc 2 uses the *_d2 base states from 'prep --disc2')")
    ap.add_argument("--region", default=os.environ.get("OC_PS1_REGION") or "japan", help="japan or usa (base states of 'prep --region usa')")
    ap.add_argument("--tries", type=int, default=3, help="retries when the emulator process dies mid-run")
    a = ap.parse_args(argv)
    if a.scene == "list":
        print(" ".join(SCENES))
        return 0
    if a.scene not in SCENES:
        raise SystemExit("unknown scene %s (%s)" % (a.scene, " ".join(SCENES)))
    cap.set_disc(a.disc, a.region)
    for attempt in range(a.tries):
        try:
            Scene(a.scene, a).run()
            return 0
        except (ConnectionError, OSError) as e:
            log("emulator connection lost (%s); attempt %d/%d" % (e, attempt + 1, a.tries))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
