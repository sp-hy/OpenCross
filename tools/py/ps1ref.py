"""Drive the original PlayStation Chrono Cross in stock DuckStation as a behaviour reference.

Mechanism: DuckStation's built-in GDB server (settings `[Debug] EnableGDBServer`) gives memory access, breakpoints and
pause/continue; a breakpoint on the game's per-frame pad reader turns that into frame stepping, and writing the raw pad
buffer at that moment injects controller input exactly one game frame at a time. Screenshots, save states and audio
capture are DuckStation's own hotkeys, posted to its (unfocused, never minimised) window as key messages.

    from ps1ref import Session
    with Session(disc) as g:                    # boots the disc, attaches, pauses at the first frame hook
        g.run(120)                              # 120 game frames, no input
        g.press("Start", 12)                    # hold Start for 12 frames
        g.run_until(lambda w: w["ovl_id"] == 6) # frames until the field overlay is loaded
        g.screenshot("tmp/x/frame.png")
        g.save_state("tmp/x/state.sav")

Command line: `python tools/py/ps1ref.py boot [--disc CUE] [--state SAV] [--frames N] [--press Button:hold[@frame],..]
[--shot-every N --out DIR] [--state-every N] [--watch]` runs one scripted session; `python tools/py/ps1ref.py watch` prints
the watch table of a running, attached emulator.
"""
import ctypes
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from ctypes import wintypes

from backstop import Backstop

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DS_DIR = os.path.join(ROOT, "tools", "duckstation")
GDB_PORT = 2345

# Game RAM map (JP SLPS-02364 main executable; the RDE and SLPM re-release carry the same executable).
PAD_RAW = 0x800856A0        # raw 34-byte pad-1 buffer the pad library fills every frame: [0]=status [1]=type [2..3]=buttons (active low)
PAD_READER = 0x8002B62C     # per-frame pad reader; the frame hook (one hit = one game frame of input)
PAD_HELD = 0x800855E0       # the game's decoded pad words: held, pressed, released, repeat (u16 each)

# Raw PS1 pad button bits (standard digital pad halfword, active low on the wire).
BUTTONS = {
    "Select": 0x0001, "L3": 0x0002, "R3": 0x0004, "Start": 0x0008,
    "Up": 0x0010, "Right": 0x0020, "Down": 0x0040, "Left": 0x0080,
    "L2": 0x0100, "R2": 0x0200, "L1": 0x0400, "R1": 0x0800,
    "Triangle": 0x1000, "Circle": 0x2000, "Cross": 0x4000, "Square": 0x8000,
}

# Watch table: name -> (address, width, signed). Same names as the save-state reader so logs stay comparable.
WATCH = {
    "ovl_id": (0x800A9F00, 4, False), "mt_ovl": (0x801D2800, 4, False), "ovl": (0x800856F0, 4, False),
    "loc": (0x80077C26, 2, False), "btl_id": (0x80075906, 2, False), "area": (0x800790FC, 2, False),
    "btl_frame": (0x800E0518, 4, False), "tgt_064b": (0x800E064B, 1, False), "phase_fc40": (0x800DFC40, 1, False),
    "menu_064a": (0x800E064A, 1, False), "cur_064e": (0x800E064E, 1, False), "e05da": (0x800E05DA, 2, False),
    "act_kind": (0x800E0666, 1, False), "act_str": (0x800E0667, 1, False), "act_elem": (0x800E0668, 2, False),
    "act_actor": (0x800E0634, 1, False), "tgt0": (0x800E0637, 1, False),
    "pad_held": (0x800855E0, 2, False), "pad_pressed": (0x800855E2, 2, False),
    "title_var": (0x801F377C, 1, False), "mt_mode": (0x801F5488, 1, False), "mt_cur": (0x801F54A8, 1, False),
    "ld_cur": (0x801F4FE4, 4, False), "ld_card": (0x801F502C, 4, False),
    "fx_load": (0x800720F8, 4, False),
    "ring0": (0x800DFF98, 1, False), "ring1": (0x800DFF9C, 1, False), "ring2": (0x800DFFA0, 1, False),
    "party0": (0x800786F4, 1, False), "party1": (0x800786F5, 1, False), "party2": (0x800786F6, 1, False),
    "u0_hp": (0x800E06A0 + 0xB8, 2, True), "u0_gauge": (0x800E06A0 + 0x16E, 2, True), "u0_sta": (0x800E06A0 + 0x19E, 2, True),
    "u3_hp": (0x800E06A0 + 0x410 * 3 + 0xB8, 2, True), "u4_hp": (0x800E06A0 + 0x410 * 4 + 0xB8, 2, True),
    "u5_hp": (0x800E06A0 + 0x410 * 5 + 0xB8, 2, True),
}


GAMES_DIR = os.path.join(DS_DIR, "games")   # the convention: disc images live here (git-ignored with the emulator)


def _find_disc(which, region=None):
    """Disc image lookup. `which` is 1 or 2; `region` is "japan" (default, the version RDE embeds and the one the RAM map
    applies to) or "usa" (English text; different executable, so only screenshots/states, no RAM-driven stepping).
    Order: OC_PS1_DISC (a file, or a folder to search), tools/duckstation/games/, then ~/Documents/DuckStation/games/.
    File names must contain "Disc 1" / "Disc 2" (or "disc1" / "disc2") and the region word; .cue, .chd are accepted."""
    region = (region or os.environ.get("OC_PS1_REGION") or "japan").lower()
    env = os.environ.get("OC_PS1_DISC", "")
    folders = []
    if env and os.path.isfile(env):
        if which == 1:
            return env
        folders.append(os.path.dirname(env))
    elif env:
        folders.append(env)
    folders += [GAMES_DIR, os.path.join(os.path.expanduser("~"), "Documents", "DuckStation", "games")]
    tags = ("disc %d" % which, "disc%d" % which)
    region_words = {"japan": ("japan", "(j)", "jp"), "usa": ("usa", "(u)", "us)")}.get(region, (region,))
    for folder in folders:
        if not os.path.isdir(folder):
            continue
        names = sorted(os.listdir(folder))
        for ext in (".cue", ".chd"):
            for n in names:
                low = n.lower()
                if low.endswith(ext) and any(t in low for t in tags) and any(w in low for w in region_words):
                    return os.path.join(folder, n)
    return None


def default_disc(region=None):
    return _find_disc(1, region)


def disc2(region=None):
    """Disc 2 image, or None. Needed only for scenes the game reaches after the disc change."""
    return _find_disc(2, region)


class Gdb:
    """Minimal GDB remote-protocol client for DuckStation's server (no vCont, no X; m/M/g/Z0/c/s/interrupt)."""

    def __init__(self, port=GDB_PORT, timeout=20.0):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # one packet per step; Nagle would add ~40 ms
        self.s.settimeout(timeout)
        self.status()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _send(self, data):
        self.s.sendall(b"$" + data.encode() + b"#%02x" % (sum(data.encode()) & 0xFF))

    def _recv(self, timeout=None):
        if timeout is not None:
            self.s.settimeout(timeout)
        buf = b""
        while True:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("gdb server closed")
            buf += chunk
            if b"$" in buf:
                end = buf.find(b"#", buf.index(b"$"))
                if end != -1 and len(buf) >= end + 3:
                    break
        self.s.sendall(b"+")
        return buf[buf.index(b"$") + 1:buf.rfind(b"#")].decode(errors="replace")

    def cmd(self, data, timeout=None):
        self._send(data)
        return self._recv(timeout)

    def status(self):
        return self.cmd("?")

    def read(self, addr, n):
        out = b""
        while n:
            k = min(n, 4096)
            r = self.cmd("m%x,%x" % (addr, k))
            if r.startswith("E"):
                raise IOError("read %08x: %s" % (addr, r))
            out += bytes.fromhex(r)
            addr += k
            n -= k
        return out

    def write(self, addr, data):
        r = self.cmd("M%x,%x:%s" % (addr, len(data), data.hex()))
        if r != "OK":
            raise IOError("write %08x: %s" % (addr, r))

    def u(self, addr, width, signed=False):
        return int.from_bytes(self.read(addr, width), "little", signed=signed)

    def regs(self):
        r = self.cmd("g")
        words = [r[i:i + 8] for i in range(0, len(r), 8)]
        return [None if "x" in w else int.from_bytes(bytes.fromhex(w), "little") for w in words]

    def pc(self):
        return self.regs()[37]

    def breakpoint(self, addr, on=True):
        r = self.cmd("%s0,%x,4" % ("Z" if on else "z", addr))
        if r != "OK":
            raise IOError("breakpoint %08x: %s" % (addr, r))

    def cont(self, timeout=120.0):
        """Continue until a breakpoint stops the system; returns the stop reply (e.g. 'S05')."""
        return self.cmd("c", timeout)

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self._recv()


class Window:
    """DuckStation's top-level window: hotkeys are posted as key messages, so the window stays unfocused."""

    VK = {"F1": (0x70, 0x3B), "F2": (0x71, 0x3C), "F3": (0x72, 0x3D), "F4": (0x73, 0x3E), "F10": (0x79, 0x44),
          "F12": (0x7B, 0x58)}

    def __init__(self, pid):
        self.u32 = ctypes.windll.user32
        self.hwnd = None
        self.pid = pid
        for _ in range(100):
            self.hwnd = self._find()
            if self.hwnd:
                break
            time.sleep(0.2)
        if not self.hwnd:
            raise RuntimeError("DuckStation window not found")
        # Restored without activation (a minimised window renders 8x8 screenshots) and put behind every other window.
        self.u32.ShowWindow(self.hwnd, 4)
        self.to_back()

    def to_back(self):
        self.u32.SetWindowPos(self.hwnd, 1, 0, 0, 0, 0, 0x0013)

    def _find(self):
        found = []
        proc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

        def cb(h, _):
            pid = wintypes.DWORD()
            self.u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.pid and self.u32.IsWindowVisible(h) and self.u32.GetWindowTextLengthW(h):
                found.append(h)
            return True
        self.u32.EnumWindows(proc(cb), 0)
        return found[0] if found else None

    def key(self, name):
        vk, sc = self.VK[name]
        self.u32.PostMessageW(self.hwnd, 0x100, vk, (sc << 16) | 1)
        time.sleep(0.03)
        self.u32.PostMessageW(self.hwnd, 0x101, vk, (sc << 16) | 0xC0000001)


def _set_speed(ds_dir, realtime):
    """Pacing keys in the portable settings.ini (read at boot): unthrottled for stepping, 60 Hz for free runs."""
    path = os.path.join(ds_dir, "settings.ini")
    if not os.path.isfile(path):
        return
    text = open(path, encoding="utf-8").read()
    for section, key, val in (("Main", "EmulationSpeed", "1" if realtime else "0"), ("Main", "SyncToHostRefreshRate", "false"),
                              ("Display", "OptimalFramePacing", "false"), ("Display", "VSync", "false")):
        text, n = re.subn(r"^%s = .*$" % key, "%s = %s" % (key, val), text, flags=re.M)
        if not n:
            text = text.replace("[%s]\n" % section, "[%s]\n%s = %s\n" % (section, key, val), 1)
    open(path, "w", encoding="utf-8", newline="\n").write(text)


def _wait_new_file(folder, before, timeout=10.0, min_size=200, pattern="*"):
    t0 = time.time()
    while time.time() - t0 < timeout:
        new = [f for f in glob.glob(os.path.join(folder, pattern)) if f not in before]
        if new:
            f = max(new, key=os.path.getmtime)
            for _ in range(50):
                if os.path.getsize(f) >= min_size:
                    break
                time.sleep(0.1)
            time.sleep(0.2)
            return f
        time.sleep(0.1)
    raise TimeoutError("no new file in " + folder)


class Session:
    """One DuckStation run: boot (disc or state), attach, frame-step with injected input, capture."""

    def __init__(self, disc=None, state=None, ds_dir=DS_DIR, port=GDB_PORT, boot_timeout=60.0, realtime=False):
        """realtime=False runs the emulator unthrottled (frame stepping is then bound only by the stop/continue round trip,
        about 4x faster); realtime=True keeps 60 Hz pacing, which a wall-clock audio capture on a free run needs."""
        self.ds_dir = ds_dir
        self.proc = None
        self.backstop = None
        _set_speed(ds_dir, realtime)
        self.exe = next(iter(glob.glob(os.path.join(ds_dir, "duckstation-qt*.exe"))), None)
        if not self.exe:
            raise RuntimeError("DuckStation not installed; run tools\\duckstation.ps1 -Setup")
        self.disc = disc or default_disc()
        if not self.disc:
            raise RuntimeError("no disc image: put your Chrono Cross (Japan) Disc 1 .cue/.chd (and Disc 2) into %s, or set OC_PS1_DISC" % GAMES_DIR)
        self.shots = os.path.join(ds_dir, "screenshots")
        self.states = os.path.join(ds_dir, "savestates")
        self.videos = os.path.join(ds_dir, "videos")
        for d in (self.shots, self.states, self.videos):
            os.makedirs(d, exist_ok=True)
        args = [self.exe, "-batch", "-nogui", "-fastboot"]
        if state:
            args += ["-statefile", os.path.abspath(state)]
        args += ["--", self.disc]
        # DuckStation activates its window when it comes up; foreground changes stay locked until the window is found and shown
        # without activation, so a capture never takes the focus from the desktop.
        user32 = ctypes.windll.user32
        user32.LockSetForegroundWindow(1)
        try:
            self._start(args, ds_dir, port, boot_timeout)
        finally:
            user32.LockSetForegroundWindow(2)

    def _start(self, args, ds_dir, port, boot_timeout):
        self.proc = subprocess.Popen(args, cwd=ds_dir, creationflags=0x08000000)  # CREATE_NO_WINDOW for the console
        self.backstop = Backstop(self.proc.pid)
        self.frame = 0
        self.held = 0
        self.gdb = None
        t0 = time.time()
        while True:
            try:
                self.gdb = Gdb(port)
                break
            except OSError:
                if self.proc.poll() is not None or time.time() - t0 > boot_timeout:
                    self.close()
                    raise RuntimeError("DuckStation did not start its GDB server")
                time.sleep(0.5)
        self.win = Window(self.proc.pid)
        self.gdb.breakpoint(PAD_READER, True)
        self._to_hook()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self.gdb:
            try:
                self.gdb.breakpoint(PAD_READER, False)
            except Exception:
                pass
            self.gdb.close()
            self.gdb = None
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)
        if self.backstop:
            self.backstop.stop()
            self.backstop = None

    # -- frame stepping --------------------------------------------------------------------------------------------
    def _to_hook(self):
        """Continue until the pad reader is entered (= the next game frame samples its input)."""
        while True:
            self.gdb.cont()
            if not self.extra_breakpoints:
                return          # the frame hook is the only breakpoint, so this stop is it
            pc = self.gdb.pc()
            if pc == PAD_READER:
                return
            if not self.on_stop(pc):
                return

    extra_breakpoints = ()

    def add_breakpoint(self, addr):
        """Extra breakpoint; stops at it call on_stop(pc) (override to log registers) and keep running."""
        self.gdb.breakpoint(addr, True)
        self.extra_breakpoints = tuple(self.extra_breakpoints) + (addr,)

    def on_stop(self, pc):
        """Called for stops that are not the frame hook; return True to keep running."""
        return True

    def _inject(self, held):
        buttons = (~held) & 0xFFFF
        self.gdb.write(PAD_RAW, bytes([0x00, 0x41]) + buttons.to_bytes(2, "little"))

    def step(self, held=None):
        """Advance one game frame with the given held button mask (default: the current hold)."""
        if held is not None:
            self.held = held
        if self.held or self._last_injected:
            self._inject(self.held)     # a released pad needs one more write so the buffer shows no buttons
        self._last_injected = bool(self.held)
        self.frame += 1
        self._to_hook()

    _last_injected = False

    def run(self, frames, held=0):
        for _ in range(frames):
            self.step(held)

    def press(self, buttons, hold=6, gap=0):
        """Hold the named button(s) for `hold` frames, then release for `gap` frames."""
        mask = 0
        for b in buttons.split("+"):
            mask |= BUTTONS[b]
        self.run(hold, mask)
        self.run(gap, 0)

    def run_until(self, cond, limit=3600, held=0):
        """Step until cond(watch) is true; returns the frame count used, or raises after `limit` frames."""
        for n in range(limit):
            if cond(self.watch()):
                return n
            self.step(held)
        raise TimeoutError("condition not met within %d frames" % limit)

    # -- reading ---------------------------------------------------------------------------------------------------
    def watch(self, names=None):
        out = {}
        for k in (names or WATCH):
            addr, width, signed = WATCH[k]
            out[k] = self.gdb.u(addr, width, signed)
        return out

    def read(self, addr, n):
        return self.gdb.read(addr, n)

    def write(self, addr, data):
        self.gdb.write(addr, data)

    def poke(self, addr, value, width=1):
        self.gdb.write(addr, int(value).to_bytes(width, "little", signed=value < 0))

    # -- capture (the emulator is paused at the frame hook, so these are frame-exact) ------------------------------
    def screenshot(self, path):
        before = set(glob.glob(os.path.join(self.shots, "*")))
        self.win.key("F10")
        f = _wait_new_file(self.shots, before)
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        shutil.move(f, path)
        return path

    def save_state(self, path):
        # The hotkey rewrites the selected slot file in place (and keeps the previous one as a backup), so the folder is
        # emptied first: whatever appears afterwards is this save, never an older one.
        for old in glob.glob(os.path.join(self.states, "*")):
            os.remove(old)
        self.win.key("F2")
        # The emulator writes "<slot>.sav.<random>" first and renames it to "<slot>.sav": only the final name counts.
        f = _wait_new_file(self.states, set(), timeout=20.0, min_size=100000, pattern="*.sav")
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        shutil.move(f, path)
        return path

    def audio_capture(self, on):
        """Toggle DuckStation's audio capture (hotkey F12; `[MediaCapture] AudioContainer = wav`)."""
        self.win.key("F12")
        time.sleep(0.3)

    def take_audio(self, path):
        files = glob.glob(os.path.join(self.videos, "*"))
        if not files:
            raise FileNotFoundError("no capture in " + self.videos)
        f = max(files, key=os.path.getmtime)
        shutil.move(f, path)
        return path


def _parse_presses(spec):
    """'Start:12@60,Circle:6@120' -> [(frame, mask, hold)] sorted."""
    out = []
    for item in [x for x in spec.split(",") if x]:
        m = re.match(r"^([A-Za-z0-9+]+):(\d+)(?:@(\d+))?$", item)
        if not m:
            raise ValueError("bad press " + item)
        mask = 0
        for b in m.group(1).split("+"):
            mask |= BUTTONS[b]
        out.append((int(m.group(3) or 0), mask, int(m.group(2))))
    return sorted(out)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("boot", help="boot and run a scripted session")
    b.add_argument("--disc")
    b.add_argument("--state")
    b.add_argument("--frames", type=int, default=600)
    b.add_argument("--press", default="", help="Button:hold@frame,... (frames count from attach)")
    b.add_argument("--shot-every", type=int, default=0)
    b.add_argument("--state-every", type=int, default=0)
    b.add_argument("--out", default=os.path.join(ROOT, "tmp", "ps1ref"))
    b.add_argument("--watch", action="store_true", help="print the watch table every 60 frames")
    b.add_argument("--audio", action="store_true", help="capture audio for the whole run into OUT/audio.wav")
    a = ap.parse_args(argv)
    if a.cmd == "boot":
        presses = _parse_presses(a.press)
        os.makedirs(a.out, exist_ok=True)
        with Session(a.disc, a.state) as g:
            print("attached; disc", g.disc, "pc", hex(g.gdb.pc()))
            if a.audio:
                g.audio_capture(True)
            active = []
            for f in range(a.frames):
                while presses and presses[0][0] <= f:
                    _, mask, hold = presses.pop(0)
                    active.append([mask, hold])
                held = 0
                for p in active:
                    held |= p[0]
                    p[1] -= 1
                active = [p for p in active if p[1] > 0]
                g.step(held)
                if a.shot_every and f % a.shot_every == 0:
                    g.screenshot(os.path.join(a.out, "frames", "frame_%06d.png" % f))
                if a.state_every and f % a.state_every == 0:
                    g.save_state(os.path.join(a.out, "states", "state_%06d.sav" % f))
                if a.watch and f % 60 == 0:
                    w = g.watch(["ovl_id", "loc", "btl_frame", "pad_held", "pad_pressed", "title_var", "mt_ovl"])
                    print(f, " ".join("%s=%x" % kv for kv in w.items()), flush=True)
            if a.audio:
                g.audio_capture(False)
                g.take_audio(os.path.join(a.out, "audio.wav"))
            print("done", g.frame, "frames")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
