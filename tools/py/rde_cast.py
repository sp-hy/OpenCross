"""Reach a battle in the Steam RDE from a forged save and step through it with screenshots, behind every other window.

    python -X utf8 tools/py/rde_cast.py forge --save tmp/save.dat --out DIR --row0 2,1,29,36 [--room 53 --entrance 1]
    python -X utf8 tools/py/rde_cast.py start --out DIR                 starts RDE on DIR/save.dat + DIR/card.mcd, prints the pid
    python -X utf8 tools/py/rde_cast.py keys  --pid N --out DIR --keys "START,w2,DOWN,w1,CIRCLE" [--shot NAME]
    python -X utf8 tools/py/rde_cast.py burst --pid N --out DIR --seconds S --every MS [--keys ...]   numbered shots while the keys run
    python -X utf8 tools/py/rde_cast.py poke  --pid N --out DIR --poke ADDR=VALUE[,ADDR=VALUE]   writes guest bytes (hex addresses), prints them back
    python -X utf8 tools/py/rde_cast.py stop  --pid N --out DIR         kills that process and checks the real save folder

A key is a pad button name (START CIRCLE CROSS UP ...), wN waits N seconds. The hashes of the real save folder are stored by `start` and
compared by `stop`. Shots are Windows.Graphics.Capture frames, which the desktop compositor hands over, so with Windows HDR on their
colours are only comparable after calibrating on a static frame.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import numpy as np
from PIL import Image

import rde_bg
import rde_save_redirect as redirect_tool


def forge(args):
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, "save.dat")
    command = [sys.executable, "-X", "utf8", os.path.join(HERE, "cc_save_forge.py"), args.save, out, "--slot", "2", "--room", str(args.room), "--entrance", str(args.entrance),
               "--party", args.party, "--char", "0:" + args.stats, "--grid", "0:0:" + args.row0, "--mcd", os.path.join(args.out, "card.mcd")]
    subprocess.run(command, check=True)
    print("forged", out)


def start(args):
    game = rde_bg.launch(os.path.join(args.out, "save.dat"), wait=args.wait, card1=os.path.join(args.out, "card.mcd"))
    with open(os.path.join(args.out, "real_saves.json"), "w") as file:
        json.dump(game.real_saves, file)
    print("pid", game.pid)


def run_keys(game, keys):
    for key in [k for k in keys.split(",") if k]:
        if key[0] == "w" and key[1:].replace(".", "").isdigit():
            time.sleep(float(key[1:]))
        else:
            game.press(getattr(rde_bg, "PAD_" + key))


def shot(frames, path):
    frame = frames.latest(960)
    if frame is not None:
        Image.fromarray(np.asarray(frame)[:, :, ::-1]).save(path)
    return frame is not None


def keys(args):
    game = rde_bg.attach(args.pid)
    frames = rde_bg.Frames(game).start()
    run_keys(game, args.keys)
    time.sleep(0.5)
    if args.shot:
        print("shot", shot(frames, os.path.join(args.out, args.shot + ".png")))
    frames.stop()


def burst(args):
    game = rde_bg.attach(args.pid)
    frames = rde_bg.Frames(game).start()
    time.sleep(0.5)
    run_keys(game, args.keys)
    begin = time.time()
    count = 0
    while time.time() - begin < args.seconds:
        shot(frames, os.path.join(args.out, "%s_%06d.png" % (args.shot or "b", int((time.time() - begin) * 1000))))
        count += 1
        time.sleep(args.every / 1000.0)
    frames.stop()
    print("shots", count)


def poke(args):
    game = rde_bg.attach(args.pid)
    for item in args.poke.split(","):
        addr, value = item.split("=")
        host = game.rde.va(rde_bg.GUEST_RAM_HOST) + (int(addr, 16) - 0x80000000)
        print(addr, "was", game.guest(int(addr, 16), 1).hex(), end=" ")
        game.rde.write(host, bytes([int(value, 16)]))
        print("now", game.guest(int(addr, 16), 1).hex())


def stop(args):
    subprocess.run(["taskkill", "/PID", str(args.pid), "/F"], capture_output=True)
    with open(os.path.join(args.out, "real_saves.json")) as file:
        before = json.load(file)
    redirect_tool.remove_sandbox()
    after = redirect_tool.real_save_hashes()
    print("real save folder", "unchanged" if json.loads(json.dumps(after)) == before else "CHANGED")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["forge", "start", "keys", "burst", "poke", "stop"])
    parser.add_argument("--save")
    parser.add_argument("--out", required=True)
    parser.add_argument("--row0", default="2,1,29,36")
    parser.add_argument("--room", type=int, default=53)
    parser.add_argument("--entrance", type=int, default=1)
    parser.add_argument("--party", default="0")
    parser.add_argument("--stats", default="hp=1023,atk=1,def=127,hit=127,evd=0,mag=1,mdef=127")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--keys", default="")
    parser.add_argument("--poke", default="")
    parser.add_argument("--shot")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--every", type=float, default=250)
    parser.add_argument("--wait", type=float, default=25)
    args = parser.parse_args()
    {"forge": forge, "start": start, "keys": keys, "burst": burst, "poke": poke, "stop": stop}[args.command](args)


if __name__ == "__main__":
    main()
