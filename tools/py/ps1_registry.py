r"""The effect registry (global resource list) at the first frames of a battle, per scene, read live from the PS1 reference.

    python tools/py/ps1_registry.py [--frames 200] [--every 40] [--out DIR] SCENE [SCENE ...]   (SCENE or A-B range)

One short DuckStation session per scene, started from tmp\ref\states\field53_encounter.sav: the battle id (ps1_ram btl_id) is poked to
0x200|scene, FRAMES frames are stepped, and every EVERY frames the registry is read straight from RAM.
Output: DIR\<scene>.txt (text only): per sample "# frame F btl_frame B ovl O n N" then "type name addr" lines of the registry.
No mash, no extra pokes: the registry of the untouched battle start (the stage = the area of the base state). Disc 1 scenes only.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fx_ref import Registry  # noqa: E402
from ps1_ram import RAM  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENC_STATE = os.path.join(ROOT, "tmp", "ref", "states", "field53_encounter.sav")


def sample(g, frame):
    """Header line + registry lines of the live RAM at this frame."""
    w = g.watch()
    reg = Registry(g.read).nodes
    lines = ["# frame %d btl_frame %d ovl %d n %d" % (frame, w["btl_frame"], w["ovl_id"], len(reg))]
    for (a, t, nm, d, typ) in reg:
        lines.append("%d\t%s\t%x" % (t, nm, a))
    return lines


def one(scene, out, frames, every, state, disc=None, ds_dir=None, port=2345):
    dst = os.path.join(out, "%d.txt" % scene)
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        return "skip"
    import ps1ref
    lines = []
    kw = {"ds_dir": ds_dir} if ds_dir else {}
    with ps1ref.Session(disc=disc, state=state, port=port, **kw) as g:
        g.poke(RAM.btl_id, 0x200 | scene, 2)
        for f in range(1, frames + 1):
            g.step()
            if f % every == 0:
                lines += sample(g, f)
    with open(dst, "w", encoding="utf8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    return "samples=%d" % sum(1 for l in lines if l.startswith("#"))


def parse_scenes(items):
    scenes = []
    for s in items:
        if "-" in s:
            lo, hi = s.split("-")
            scenes += list(range(int(lo), int(hi) + 1))
        else:
            scenes.append(int(s))
    return scenes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("scenes", nargs="+", help="scene numbers or A-B ranges")
    ap.add_argument("--frames", type=int, default=200)
    ap.add_argument("--every", type=int, default=40)
    ap.add_argument("--state", default=ENC_STATE, help="base field state (default tmp/ref/states/field53_encounter.sav)")
    ap.add_argument("--disc", default=None, help="disc image (default: ps1ref's)")
    ap.add_argument("--out", default=os.path.join(ROOT, "tmp", "r38", "D2", "ps1"))
    ap.add_argument("--ds-dir", default=None)
    ap.add_argument("--port", type=int, default=2345)
    a = ap.parse_args(argv)
    scenes = parse_scenes(a.scenes)
    os.makedirs(a.out, exist_ok=True)
    for s in scenes:
        print(s, one(s, a.out, a.frames, a.every, a.state, a.disc, a.ds_dir, a.port), flush=True)


if __name__ == "__main__":
    main()
