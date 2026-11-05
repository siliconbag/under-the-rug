"""Read Explorer desktop icon positions without changing the desktop layout."""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes


def _desktop_listview(user):
    user.FindWindowW.argtypes = (wintypes.LPCWSTR, wintypes.LPCWSTR)
    user.FindWindowW.restype = wintypes.HWND
    user.FindWindowExW.argtypes = (wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR)
    user.FindWindowExW.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user.SendMessageTimeoutW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM,
                                         wintypes.LPARAM, wintypes.UINT, wintypes.UINT,
                                         ctypes.POINTER(ctypes.c_size_t))
    user.SendMessageTimeoutW.restype = wintypes.LPARAM
    user.ClientToScreen.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
    desktop = 0
    hosts = [user.FindWindowW("Progman", None)]
    worker = 0
    while True:
        worker = user.FindWindowExW(0, worker, "WorkerW", None)
        if not worker:
            break
        hosts.append(worker)
    for host in hosts:
        view = user.FindWindowExW(host, 0, "SHELLDLL_DefView", None)
        if view:
            desktop = user.FindWindowExW(view, 0, "SysListView32", None)
            if desktop:
                break
    return desktop


def position_above_desktop(hwnd: int) -> bool:
    """Place a top-level window just above Explorer's icon host in the Z order."""
    if sys.platform != "win32":
        return False
    user = ctypes.WinDLL("user32", use_last_error=True)
    desktop = _desktop_listview(user)
    if not desktop:
        return False
    user.GetAncestor.argtypes = (wintypes.HWND, wintypes.UINT)
    user.GetAncestor.restype = wintypes.HWND
    user.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT)
    host = user.GetAncestor(desktop, 2)  # GA_ROOT
    user.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
    user.ShowWindow(hwnd, 4)  # SW_SHOWNOACTIVATE, including after Win+D
    return bool(user.SetWindowPos(hwnd, host, 0, 0, 0, 0,
                                  0x0001 | 0x0002 | 0x0010 | 0x0040))


def set_click_through(hwnd: int, enabled: bool) -> None:
    if sys.platform != "win32":
        return
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.GetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int)
    user.GetWindowLongPtrW.restype = ctypes.c_ssize_t
    user.SetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t)
    user.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wintypes.UINT)
    style = user.GetWindowLongPtrW(hwnd, -20)
    updated = style | 0x20 if enabled else style & ~0x20  # WS_EX_TRANSPARENT
    if updated != style:
        user.SetWindowLongPtrW(hwnd, -20, updated)
        user.SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                          0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020)


def icon_positions() -> list[tuple[int, int]]:
    if sys.platform != "win32":
        return []
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    desktop = _desktop_listview(user)
    if not desktop:
        return []
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.VirtualAllocEx.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                      wintypes.DWORD, wintypes.DWORD)
    kernel.VirtualAllocEx.restype = ctypes.c_void_p
    kernel.VirtualFreeEx.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD)
    kernel.ReadProcessMemory.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                         ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)

    pid = wintypes.DWORD()
    user.GetWindowThreadProcessId(desktop, ctypes.byref(pid))
    process = kernel.OpenProcess(0x0400 | 0x0008 | 0x0010, False, pid.value)
    if not process:
        return []
    remote = None
    try:
        remote = kernel.VirtualAllocEx(process, None, ctypes.sizeof(wintypes.POINT), 0x1000 | 0x2000, 0x04)
        if not remote:
            return []
        result = ctypes.c_size_t()
        if not user.SendMessageTimeoutW(desktop, 0x1004, 0, 0, 0x0002, 300, ctypes.byref(result)):
            return []
        count = min(int(result.value), 2000)
        positions = []
        for index in range(count):
            if not user.SendMessageTimeoutW(desktop, 0x1010, index, remote, 0x0002, 300, ctypes.byref(result)):
                continue
            point = wintypes.POINT()
            size = ctypes.c_size_t()
            if kernel.ReadProcessMemory(process, remote, ctypes.byref(point), ctypes.sizeof(point), ctypes.byref(size)):
                user.ClientToScreen(desktop, ctypes.byref(point))
                positions.append((point.x, point.y))
        return positions
    finally:
        if remote:
            kernel.VirtualFreeEx(process, remote, 0, 0x8000)
        kernel.CloseHandle(process)
