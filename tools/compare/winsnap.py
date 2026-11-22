"""Windows-only helpers to drive and capture a GUI process: find its window, post keys,
and grab the client area with PrintWindow (works while the window is covered).
Standard library + Pillow."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import time

from PIL import Image

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.SetProcessDPIAware()

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
PW_CLIENTONLY, PW_RENDERFULLCONTENT = 1, 2

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def find_window(pid: int, timeout: float = 10.0) -> int:
    end = time.time() + timeout
    while time.time() < end:
        found = []

        def cb(hwnd, _):
            p = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):
                found.append(hwnd)
            return True

        user32.EnumWindows(EnumWindowsProc(cb), 0)
        if found:
            return found[0]
        time.sleep(0.1)
    raise TimeoutError(f"no window for pid {pid}")


def client_size(hwnd: int) -> tuple[int, int]:
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD), ("biClrImportant", wt.DWORD)]


def grab_client(hwnd: int) -> Image.Image:
    w, h = client_size(hwnd)
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT)
    bih = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)


def post_key(hwnd: int, vk: int, down: bool) -> None:
    lparam = 1 if down else (1 | 0xC0000000)
    user32.PostMessageW(hwnd, WM_KEYDOWN if down else WM_KEYUP, vk, lparam)
