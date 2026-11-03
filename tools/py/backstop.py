"""Keeps the windows of a test process (DuckStation, RDE) behind every other window, so a capture never shows over the desktop."""
import ctypes
import threading
from ctypes import wintypes


class Backstop:
    """Keeps every window of one process behind all other windows: a window-event hook sends a window to the back the moment it
    is shown or brought to the front, before the desktop composes it over the windows in front."""

    def __init__(self, pid):
        self.pid = pid
        self.thread_id = None
        ready = threading.Event()
        self.thread = threading.Thread(target=self._run, args=(ready,), daemon=True)
        self.thread.start()
        ready.wait(5)

    def _run(self, ready):
        user32 = ctypes.windll.user32
        self.thread_id = ctypes.windll.kernel32.GetCurrentThreadId()
        hook_t = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.DWORD, wintypes.HWND, wintypes.LONG, wintypes.LONG,
                                    wintypes.DWORD, wintypes.DWORD)

        def on_event(_hook, _event, hwnd, id_object, id_child, _thread, _time):
            if hwnd and id_object == 0 and id_child == 0 and user32.GetAncestor(hwnd, 2) == hwnd and user32.IsWindowVisible(hwnd):
                user32.SetWindowPos(hwnd, 1, 0, 0, 0, 0, 0x0013)
        callback = hook_t(on_event)
        hooks = [user32.SetWinEventHook(event, event, None, callback, self.pid, 0, 0) for event in (0x0003, 0x8002)]
        ready.set()
        message = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(message))
            user32.DispatchMessageW(ctypes.byref(message))
        for hook in hooks:
            user32.UnhookWinEvent(hook)

    def stop(self):
        if self.thread_id:
            ctypes.windll.user32.PostThreadMessageW(self.thread_id, 0x0012, 0, 0)
        self.thread.join(5)
