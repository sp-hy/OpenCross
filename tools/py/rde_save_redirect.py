"""Run the Steam RDE (CHRONOCROSS.exe) on a forged or copied save without touching the real save folder.

RDE keeps the full path of its save archive in one string that every archive read and write opens, and keeps memory
card 1 as a 128 KB image in memory. This tool launches the game, waits for the title screen, then points that string
at a file you name and optionally replaces the card 1 image, so Continue shows the slots of your file and any write
(auto-save, save point) lands in your file. Run it under a copy of a save, never the real folder.

usage:
  python -X utf8 tools/py/rde_save_redirect.py --save PATH/save.dat [--card1 FILE.mcd] [--title-wait 25]
         [--keys "enter,w2,enter"] [--shot out.png] [--attach] [--keep-open] [--lang N]
  --save        save archive to use; refused when it lies inside Documents\\My Games
  --card1       128 KB card image (starts with "MC") that replaces the in-memory card 1
  --title-wait  seconds from launch until the title screen is up (default 25)
  --attach      patch an already running game instead of launching one (no wait)
  --keys        comma list sent to the window after patching (a key is tapped, wN waits N seconds,
                hold:KEY:SECONDS holds a key)
  --shot        screenshot of the window after the keys (640x512)
  --lang        game language, 0 en 1 fr 2 it 3 de 4 es 5 jp: written to Language= of the scratch config.ini before boot (the game reads it
                only at boot, so it cannot be combined with --attach)
  --graphics    art mode, 0 original 1 HD: written to GraphicsMode= of the scratch config.ini before boot (read only at boot)
  --keep-open   leave the game running; otherwise only the process this tool started is killed
Library: launch(), Rde(pid).redirect(path), Rde.replace_card1(data), check_save_path(path).
Run from the repo root (it imports tools/rde_win.py for the window helpers).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

import win32api
import win32con
import win32process

IMAGE_BASE = 0x400000
SAVE_PATH_STRING = 0x012B1F14
CARD1_IMAGE = 0x009EF200
CARD_SIZE = 0x20000
STEAM_APP_ID = "1133760"
SAFE_BUFFER = 4000

SAVE_ROOT_NAME = 0x006C05A2      # image address of 'My Games' in the save folder format string
SANDBOX = "OCRdeRun"             # same length; Documents\\OCRdeRun replaces Documents\\My Games for a tool run
GAME_SUBDIR = "CHRONO CROSS  THE RADICAL DREAMERS EDITION"
CREATE_SUSPENDED = 4

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
k32.VirtualProtectEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GAME_DIR = os.path.dirname(REPO)
TOOLS_DIR = os.path.join(REPO, "tools")


def check_save_path(path):
    """Exit with an error when path is inside the real save folder (Documents\\My Games)."""
    full = os.path.normcase(os.path.realpath(path))
    if os.path.normcase(os.path.join("Documents", "My Games")) in full:
        raise SystemExit("refusing: %s is inside Documents\\My Games; use a copy" % path)
    return os.path.abspath(path)


def documents():
    """The folder the game builds its save path on (the shell's Personal folder, wherever it was moved to)."""
    path = ctypes.create_unicode_buffer(wt.MAX_PATH)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, path):
        raise SystemExit("no Documents folder")
    return path.value


def real_save_hashes():
    """{file: sha1} of everything in the real save folder, to prove a run left it alone."""
    root = os.path.join(documents(), "My Games", GAME_SUBDIR)
    out = {}
    for folder, _, files in os.walk(root):
        for name in files:
            full = os.path.join(folder, name)
            with open(full, "rb") as fh:
                out[full] = hashlib.sha1(fh.read()).hexdigest()
    return out


def check_real_saves(before):
    after = real_save_hashes()
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    if changed:
        raise SystemExit("REAL SAVE FOLDER CHANGED during the run: " + ", ".join(changed))


def reset_sandbox(save=None, language=None, graphics=None):
    """Empties the scratch folder the patched game uses, then gives each Steam id the test save and the real config (with Language=
    set to `language` and GraphicsMode= to `graphics` when given), so the boot, which runs before any redirect, reads and writes only
    these copies."""
    root = os.path.join(documents(), SANDBOX, GAME_SUBDIR)
    shutil.rmtree(os.path.dirname(root), ignore_errors=True)
    real = os.path.join(documents(), "My Games", GAME_SUBDIR, "Steam")
    for steam_id in os.listdir(real):
        folder = os.path.join(root, "Steam", steam_id)
        os.makedirs(folder)
        config = os.path.join(real, steam_id, "config.ini")
        if os.path.isfile(config):
            scratch = os.path.join(folder, "config.ini")
            shutil.copyfile(config, scratch)
            if language is not None or graphics is not None:
                with open(scratch, "r", encoding="utf-8") as fh:
                    text = fh.read()
                if language is not None:
                    text = re.sub(r"(?m)^Language=.*$", "Language=%d" % language, text, count=1)
                if graphics is not None:
                    text = re.sub(r"(?m)^GraphicsMode=.*$", "GraphicsMode=%d" % graphics, text, count=1)
                with open(scratch, "w", encoding="utf-8", newline="") as fh:
                    fh.write(text)
        if save and os.path.isfile(save):
            shutil.copyfile(save, os.path.join(folder, "save.dat"))


def remove_sandbox():
    shutil.rmtree(os.path.join(documents(), SANDBOX), ignore_errors=True)


def sandbox_save_path(process):
    """Replaces 'My Games' in the game's one save folder format string, while the process is still suspended, so no game code ever
    sees the real folder. Same length, in memory only."""
    info = (ctypes.c_uint64 * 6)()
    handle = int(process._handle)
    if ntdll.NtQueryInformationProcess(wt.HANDLE(handle), 0, info, ctypes.sizeof(info), None):
        raise OSError("no process information")
    base_bytes = ctypes.create_string_buffer(4)
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(info[1] + 0x10), base_bytes, 4, None):
        raise OSError(ctypes.get_last_error(), "read of the process environment block")
    addr = struct.unpack("<I", base_bytes.raw)[0] + SAVE_ROOT_NAME - IMAGE_BASE
    old_name, new_name = "My Games".encode("utf-16-le"), SANDBOX.encode("utf-16-le")
    now = ctypes.create_string_buffer(len(old_name))
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), now, len(old_name), None) or now.raw != old_name:
        raise SystemExit("the save folder string is not where this exe version keeps it: refusing to run")
    protect = wt.DWORD()
    if not k32.VirtualProtectEx(handle, ctypes.c_void_p(addr), len(new_name), 0x04, ctypes.byref(protect)):
        raise OSError(ctypes.get_last_error(), "protect")
    if not k32.WriteProcessMemory(handle, ctypes.c_void_p(addr), new_name, len(new_name), None):
        raise OSError(ctypes.get_last_error(), "write of the save folder string")
    k32.VirtualProtectEx(handle, ctypes.c_void_p(addr), len(new_name), protect.value, ctypes.byref(protect))
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), now, len(new_name), None) or now.raw != new_name:
        raise SystemExit("the save folder string did not change: refusing to run")


def launch(save=None, startupinfo=None, language=None, graphics=None):
    """Starts CHRONOCROSS.exe from the game folder in the scratch save folder and returns the Popen. Never starts it unpatched."""
    env = dict(os.environ, SteamAppId=STEAM_APP_ID, SteamGameId=STEAM_APP_ID)
    reset_sandbox(save, language, graphics)
    process = subprocess.Popen([os.path.join(GAME_DIR, "CHRONOCROSS.exe")], cwd=GAME_DIR, env=env, startupinfo=startupinfo, creationflags=CREATE_SUSPENDED)
    try:
        sandbox_save_path(process)
    except BaseException:
        process.kill()
        raise
    ntdll.NtResumeProcess(wt.HANDLE(int(process._handle)))
    return process


class Rde:
    """Memory access to one running CHRONOCROSS.exe."""

    def __init__(self, pid):
        self.pid = pid
        self.h = win32api.OpenProcess(win32con.PROCESS_ALL_ACCESS, False, pid)
        self.hv = int(self.h)
        self.base = None
        for module in win32process.EnumProcessModulesEx(self.h, 0x03):
            if win32process.GetModuleFileNameEx(self.h, module).lower().endswith("\\chronocross.exe"):
                self.base = module
        if self.base is None:
            raise SystemExit("module base of CHRONOCROSS.exe not found")

    def va(self, image_va):
        return self.base + image_va - IMAGE_BASE

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.hv, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
            raise OSError(ctypes.get_last_error(), "read %x" % addr)
        return buf.raw

    def write(self, addr, data):
        got = ctypes.c_size_t()
        if not k32.WriteProcessMemory(self.hv, ctypes.c_void_p(addr), data, len(data), ctypes.byref(got)):
            raise OSError(ctypes.get_last_error(), "write %x" % addr)

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def save_path(self):
        """The current path string (an MSVC std::string: inline up to 15 bytes, else a pointer)."""
        a = self.va(SAVE_PATH_STRING)
        size, cap = self.u32(a + 0x10), self.u32(a + 0x14)
        ptr = self.u32(a) if cap > 15 else a
        return self.read(ptr, size).decode("mbcs"), ptr, size, cap

    def redirect(self, path):
        a = self.va(SAVE_PATH_STRING)
        raw = os.path.abspath(path).encode("mbcs")
        _, ptr, _, cap = self.save_path()
        if len(raw) <= cap and cap > 15:
            self.write(ptr, raw + b"\0")
        else:
            mem = k32.VirtualAllocEx(self.hv, None, 4096, 0x3000, 0x04)
            self.write(mem, raw + b"\0")
            self.write(a, struct.pack("<I", mem))
            self.write(a + 0x14, struct.pack("<I", SAFE_BUFFER))
        self.write(a + 0x10, struct.pack("<I", len(raw)))

    def card1(self):
        return self.read(self.va(CARD1_IMAGE), CARD_SIZE)

    def replace_card1(self, data):
        if len(data) != CARD_SIZE or data[:2] != b"MC":
            raise SystemExit("card 1 image must be 128 KB and start with MC")
        if self.card1()[:2] != b"MC":
            raise SystemExit("card 1 image not found in the game (not at the title yet?)")
        self.write(self.va(CARD1_IMAGE), data)


def send_keys(tokens):
    """Tap keys / wait in the game window: a key is tapped, wN waits N seconds."""
    sys.path.insert(0, TOOLS_DIR)
    import pydirectinput
    import rde_win
    pydirectinput.PAUSE = 0.02
    window = rde_win.find()
    rde_win.front(window)
    time.sleep(0.3)
    for token in tokens:
        if token.startswith("w") and token[1:].replace(".", "", 1).isdigit():
            time.sleep(float(token[1:]))
        elif token.startswith("hold:"):
            _, key, seconds = token.split(":")
            pydirectinput.keyDown(key)
            time.sleep(float(seconds))
            pydirectinput.keyUp(key)
            time.sleep(0.3)
        else:
            pydirectinput.keyDown(token)
            time.sleep(0.12)
            pydirectinput.keyUp(token)
            time.sleep(0.3)


def screenshot(path):
    sys.path.insert(0, TOOLS_DIR)
    import rde_win
    rde_win.shot(path, (640, 512))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--save", required=True)
    ap.add_argument("--card1")
    ap.add_argument("--title-wait", type=float, default=25)
    ap.add_argument("--keys", default="")
    ap.add_argument("--shot")
    ap.add_argument("--attach", action="store_true")
    ap.add_argument("--keep-open", action="store_true")
    ap.add_argument("--lang", type=int)
    ap.add_argument("--graphics", type=int)
    args = ap.parse_args()

    if (args.lang is not None or args.graphics is not None) and args.attach:
        raise SystemExit("--lang and --graphics are read at boot: they cannot be combined with --attach")
    save = check_save_path(args.save)
    if not os.path.isfile(save):
        raise SystemExit("save file not found: " + save)
    card = None
    if args.card1:
        with open(args.card1, "rb") as f:
            card = f.read()

    proc = None
    if args.attach:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq CHRONOCROSS.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        pids = [int(line.strip('"').split('","')[1]) for line in out.splitlines() if line.lower().startswith('"chronocross.exe"')]
        if not pids:
            raise SystemExit("CHRONOCROSS.exe is not running")
        pid = pids[0]
    else:
        before = real_save_hashes()
        proc = launch(save, language=args.lang, graphics=args.graphics)
        pid = proc.pid
        print("started pid", pid)
        time.sleep(args.title_wait)
    try:
        rde = Rde(pid)
        rde.redirect(save)
        print("save path now", rde.save_path()[0])
        if card is not None:
            rde.replace_card1(card)
            print("card 1 replaced")
        if args.keys:
            send_keys(args.keys.split(","))
        if args.shot:
            screenshot(args.shot)
            print("shot", args.shot)
    finally:
        if proc is not None and not args.keep_open:
            proc.kill()
            proc.wait()
            print("killed pid", pid)
            remove_sandbox()
            check_real_saves(before)


if __name__ == "__main__":
    main()
