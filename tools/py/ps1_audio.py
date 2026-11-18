"""PS1 reference audio plus sound-request trace, recorded in stock DuckStation.

    python -X utf8 tools/py/ps1_audio.py --state tmp/ref/states/bg00_menu.sav --out tmp/x2c/fireball --cast 5 --frames 1100
    (--cast N: the battle menu is driven condition by condition like ps1_scene.py, N Downs in the element list; the action
    frame is the frame after the cast-confirming press's first frame, and only hits from there on are traced)
    python -X utf8 tools/py/ps1_audio.py --state S.sav --out OUT --press "Confirm:12@40,Confirm:12@80,Down:20@300" --frames 600
    (Confirm / Cancel are the build's own buttons: Circle / Cross on the Japanese build, Cross / Circle on the USA one)

Phase 1 (stepped, frame-exact): load the state, apply the presses, save a state at the action frame, then keep stepping
--frames MORE frames with the sound breakpoints active, logging every hit with its exact frame. Phase 2 (free run, no
breakpoints): load the action state, start DuckStation's audio capture and let the game run for --frames/60 s of wall
clock. The capture is wall-clock, so any breakpoint stop would distort it, and the free run has NO INPUT. The emulator is
deterministic, so the sounds in audio.wav line up with phase 1: audio time = (frame - action_frame) / 60.
--action-frame defaults to the end of the last press.

Output folder: audio.wav (44.1 kHz stereo s16), audio.raw (the same samples, headerless), trace.txt, action.sav, manifest.txt.
trace.txt lines: `frame pc a0 a1 a2 a3 ra` (hex registers at the entry; frame counts game frames from the loaded state).
"""
import argparse
import glob
import os
import re
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps1_ram
import ps1ref
import ps1_scene
from ps1_capture import Steps, STD_ROW0
from ps1_ram import RAM
from ps1ref import Session, BUTTONS

FPS = 60.0


def parse_presses(spec):
    """'Confirm:12@40,Down:20@300' -> [(start_frame, mask, hold)]"""
    out = []
    for item in [x for x in spec.split(",") if x]:
        m = re.match(r"^([A-Za-z0-9+]+):(\d+)@(\d+)$", item)
        if not m:
            raise ValueError("bad press %r (want Button:hold@start)" % item)
        mask = 0
        for b in m.group(1).split("+"):
            mask |= BUTTONS[b]
        out.append((int(m.group(3)), mask, int(m.group(2))))
    return sorted(out)


class Tracer(Session):
    """Session that logs register state at the sound entry points."""

    def __init__(self, *a, trace_file=None, **k):
        self.trace_file = trace_file
        self.live = True
        super().__init__(*a, **k)

    def on_stop(self, pc):
        if not self.live:
            return True
        r = self.gdb.regs()
        self.trace_file.write("%d %08x %x %x %x %x %x\n" % (self.frame, pc, r[4], r[5], r[6], r[7], r[31]))
        return True


def free_run(g, seconds):
    """Continue with no breakpoints for `seconds` of wall-clock time, then pause."""
    g.gdb._send("c")
    time.sleep(seconds)
    g.gdb.interrupt()


def wav_to_raw(wav_path, raw_path):
    with wave.open(wav_path) as w:
        info = (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes())
        data = w.readframes(w.getnframes())
    with open(raw_path, "wb") as f:
        f.write(data)
    return info


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--state", required=True, help="stock DuckStation state to start from")
    ap.add_argument("--out", required=True, help="output folder")
    ap.add_argument("--press", default="", help="Button:hold@start,... (frames from the loaded state)")
    ap.add_argument("--cast", type=int, default=-1, help="element cast key sequence (0 = first entry)")
    ap.add_argument("--frames", type=int, default=300, help="free-run game frames to record (trace this many frames after the action frame; record frames/60 s)")
    ap.add_argument("--action-frame", type=int, default=-1, help="frame to save action.sav and start recording (default: last press end)")
    ap.add_argument("--pcs", default="", help="hex PCs to trace (default: the build's sound-request entries, ps1_ram sound_pcs; "
                    "add --anim for the per-frame animation sound calls)")
    ap.add_argument("--anim", action="store_true", help="also trace the per-frame animation sound calls (they fire every frame)")
    ap.add_argument("--disc")
    ap.add_argument("--ds-dir", default=ps1ref.DS_DIR)
    ap.add_argument("--port", type=int, default=ps1ref.GDB_PORT)
    a = ap.parse_args(argv)

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    RAM.use(ps1_ram.region_of_disc(a.disc or ps1ref.default_disc()))
    presses = parse_presses(a.press)
    action = a.action_frame if a.action_frame >= 0 else max([s + h for s, m, h in presses] or [0])
    tpath = os.path.join(out, "trace.txt")
    action_sav = os.path.join(out, "action.sav")
    wav_path = os.path.join(out, "audio.wav")

    with open(tpath, "w") as tf:
        # Phase 1: stepped, input injected, sound breakpoints logged to `action + frames`
        g = Tracer(a.disc, a.state, ds_dir=a.ds_dir, port=a.port, trace_file=tf)
        # The session has picked the build from the disc, so the default PCs are that build's.
        pcs = [int(x, 16) for x in a.pcs.split(",") if x] or list(RAM.sound_pcs)
        if a.anim:
            pcs += RAM.anim_pcs
        try:
            if a.cast >= 0:
                # Menu driven by the same RAM-verified steps as ps1_scene.py; tracing starts at the cast-confirming press.
                st = Steps(g)
                row0 = [int(x) for x in STD_ROW0.split(",")]
                elem = row0[(ps1_scene.LIST_OPEN_CELL + a.cast) % len(row0)]
                ps1_scene.Scene.weak_hit(None, g, st, True)
                ps1_scene.Scene.element_cast(None, g, st, elem, a.cast)
                g.run(12)
                presses.append((g.frame, BUTTONS["Confirm"], 4))
                # The state is saved after the first confirm frame (the game has taken the press), so the free run needs no input.
                action = g.frame + 1
                menu_rows = st.rows
            for pc in pcs:
                g.add_breakpoint(pc)
            saved = False
            while g.frame < action + a.frames:
                g.live = g.frame >= action
                if g.frame == action and not saved:
                    g.save_state(action_sav)
                    saved = True
                f = g.frame
                held = 0
                for s, mask, hold in presses:
                    if s <= f < s + hold:
                        held |= mask
                g.step(held)
            if not saved:
                g.save_state(action_sav)
            p1_frames = g.frame
        finally:
            g.close()

    # Phase 2: free run from the action state, no breakpoints, nothing pressed
    g = Session(a.disc, action_sav, ds_dir=a.ds_dir, port=a.port, realtime=True)   # wall-clock capture needs 60 Hz pacing
    try:
        g.gdb.breakpoint(RAM.pad_reader, False)
        for f in glob.glob(os.path.join(g.videos, "*")):
            os.remove(f)
        g.audio_capture(True)
        t0 = time.time()
        free_run(g, a.frames / FPS + 0.1)
        t1 = time.time()
        g.audio_capture(False)
        time.sleep(0.5)
        g.take_audio(wav_path)
    finally:
        g.close()
    info = wav_to_raw(wav_path, os.path.join(out, "audio.raw"))
    secs = info[3] / float(info[2])
    with open(os.path.join(out, "manifest.txt"), "w") as m:
        m.write("state %s\npress %s\ncast %d\nphase1_frames %d\naction_frame %d\nfree_frames %d\nfree_wall_s %.3f\n"
                % (os.path.abspath(a.state), a.press, a.cast, p1_frames, action, a.frames, t1 - t0))
        m.write("wav channels %d width %d rate %d samples %d seconds %.3f\n" % (info + (secs,)))
        m.write("pcs %s (%s build)\nnote free run has no input and no breakpoints; audio time = (trace frame - action_frame)/60\n"
                % (",".join("%08x" % pc for pc in pcs), RAM.region))
    print("audio.wav %.2f s (expected %.2f s)  trace lines %d" % (secs, a.frames / FPS, sum(1 for _ in open(tpath))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
