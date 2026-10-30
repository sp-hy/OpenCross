"""Drive the Steam RDE (CHRONOCROSS.exe) in the background: no focus change, no keyboard input, nothing on top of other windows.

The game starts without activation and sits at the bottom of the z-order. Input does not go through the keyboard: the in-memory
keyboard-to-pad function is patched to return a word this tool writes, so what the user types never reaches the game, and the game
files are not touched. Frames come from Windows.Graphics.Capture by window handle, which works while the window is covered (not
while it is minimised). Run under a copy of a save, like rde_save_redirect.py; run from the repo root.

Library:
  game = launch(save, card1=None)   start RDE, redirect the save, replace memory card 1, take over input
  game = attach(pid)          the same for a running game
  game.press(PAD_START)       hold buttons for a time, then release
  game.hold(buttons)          set the held buttons (0 = none)
  game.guest(addr, size)      read guest RAM; game.u16(addr), game.u32(addr)
  game.close()                kill the process this tool started
  Frames(game).start() ... .latest(width) ... .stop()   newest live frame as BGR, or .log of (seconds, mean luma)

RDE reads memory card 1 once at boot, so forged slots need `card1` (a 128 KB image from cc_save_forge.py --mcd); a redirected save
alone still shows the slots of the real file. A covered window alternates the live picture with a stale one: latest() keeps the
brighter of the last two frames, and a luma log needs the stale value filtered out. System loopback audio also holds whatever else
the PC plays; use the process's own audio session meter.
"""
import ctypes
import os
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import win32con
import win32gui
import win32process

import rde_save_redirect as redirect_tool

KEYBOARD_TO_PAD = 0x0042A250     # image address of the function that returns the pressed-buttons word
GUEST_RAM_HOST = 0x00A2F28C      # image address of guest address 0x80000000

PAD_SELECT, PAD_L3, PAD_R3, PAD_START = 0x0001, 0x0002, 0x0004, 0x0008
PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT = 0x0010, 0x0020, 0x0040, 0x0080
PAD_L2, PAD_R2, PAD_L1, PAD_R1 = 0x0100, 0x0200, 0x0400, 0x0800
PAD_TRIANGLE, PAD_CIRCLE, PAD_CROSS, PAD_SQUARE = 0x1000, 0x2000, 0x4000, 0x8000

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
MEM_COMMIT_RESERVE, PAGE_READWRITE = 0x3000, 0x04


def window_of(pid):
    """The game window of one process (several games can run at once)."""
    found = []

    def check(hwnd, _):
        if win32gui.GetWindowText(hwnd) == "CHRONO CROSS" and win32process.GetWindowThreadProcessId(hwnd)[1] == pid:
            found.append(hwnd)

    win32gui.EnumWindows(check, None)
    return found[0] if found else None


class Game:
    def __init__(self, pid, process=None):
        self.pid = pid
        self.process = process
        self.rde = redirect_tool.Rde(pid)
        self.hwnd = window_of(pid)
        self.cell = None

    def to_back(self):
        flags = win32con.SWP_NOMOVE | win32con.SWP_NOSIZE | win32con.SWP_NOACTIVATE
        win32gui.SetWindowPos(self.hwnd, win32con.HWND_BOTTOM, 0, 0, 0, 0, flags)

    def take_input(self):
        """Makes the keyboard-to-pad function return the word at a private cell: movzx eax, word ptr [cell]; ret."""
        self.cell = k32.VirtualAllocEx(self.rde.hv, None, 4, MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not self.cell or self.cell >= 1 << 32:
            raise OSError(ctypes.get_last_error(), "no cell for the pad word")
        self.rde.write(self.cell, bytes(4))
        self.rde.write(self.rde.va(KEYBOARD_TO_PAD), b"\x0f\xb7\x05" + struct.pack("<I", self.cell) + b"\xc3")

    def hold(self, buttons):
        self.rde.write(self.cell, struct.pack("<H", ((buttons & 0xFF) << 8) | (buttons >> 8)))

    def press(self, buttons, seconds=0.12, gap=0.12):
        self.hold(buttons)
        time.sleep(seconds)
        self.hold(0)
        time.sleep(gap)

    def guest(self, addr, size):
        return self.rde.read(self.rde.va(GUEST_RAM_HOST) + (addr - 0x80000000), size)

    def u16(self, addr):
        return struct.unpack("<H", self.guest(addr, 2))[0]

    def u32(self, addr):
        return struct.unpack("<I", self.guest(addr, 4))[0]

    def close(self):
        if self.process:
            self.process.kill()


def give_focus_back(hwnd):
    """Returns the foreground to `hwnd`, which a background process may do through the task-switch call."""
    ctypes.windll.user32.SwitchToThisWindow(hwnd, True)


def launch(save, wait=25, card1=None):
    """Starts RDE behind everything; `wait` = seconds until the title is up, when the save path can be redirected. The game takes
    the foreground once, when its window appears; it goes straight back to the window that had it."""
    previous = win32gui.GetForegroundWindow()
    redirect_tool.check_save_path(save)
    env = dict(os.environ, SteamAppId=redirect_tool.STEAM_APP_ID, SteamGameId=redirect_tool.STEAM_APP_ID)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = win32con.SW_SHOWNOACTIVATE
    process = subprocess.Popen([os.path.join(redirect_tool.GAME_DIR, "CHRONOCROSS.exe")], cwd=redirect_tool.GAME_DIR, env=env, startupinfo=startup)
    start = time.time()
    while not window_of(process.pid):
        if time.time() - start > 30:
            process.kill()
            raise SystemExit("no RDE window")
        time.sleep(0.01)
    game = Game(process.pid, process)
    try:
        game.take_input()
        while time.time() - start < wait:
            if win32gui.GetForegroundWindow() == game.hwnd:
                give_focus_back(previous)
                game.to_back()
            time.sleep(0.01)
        game.to_back()
    except Exception:
        process.kill()
        raise
    game.rde.redirect(os.path.abspath(save))
    if card1:
        with open(card1, "rb") as image:
            game.rde.replace_card1(image.read())
    return game


def attach(pid):
    game = Game(pid)
    game.to_back()
    game.take_input()
    return game


class Frames:
    """Newest frame of the window and a (seconds, mean luma) log of every frame the capture delivers."""

    def __init__(self, game):
        from windows_capture import WindowsCapture
        self.capture = WindowsCapture(cursor_capture=False, draw_border=False, window_hwnd=game.hwnd)
        self.frame = None
        self.previous = None
        self.log = []
        self.count = 0
        self.start_time = time.perf_counter()
        self.lock = threading.Lock()
        self.capture.event(self.on_frame_arrived)
        self.capture.event(self.on_closed)
        self.control = None

    def on_frame_arrived(self, frame, control):
        import numpy as np
        pixels = np.ascontiguousarray(frame.frame_buffer[:, :, :3])
        with self.lock:
            self.previous = self.frame
            self.frame = pixels
            self.count += 1
            self.log.append((time.perf_counter() - self.start_time, float(pixels.mean())))

    def on_closed(self):
        pass

    def start(self):
        self.control = self.capture.start_free_threaded()
        return self

    def stop(self):
        self.control.stop()

    def latest(self, width=240):
        import cv2
        with self.lock:
            pixels, older = self.frame, self.previous
        if pixels is None:
            return None
        if older is not None and older.mean() > pixels.mean():
            pixels = older
        return cv2.resize(pixels, (width, width * pixels.shape[0] // pixels.shape[1]), interpolation=cv2.INTER_AREA)
