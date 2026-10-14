"""Small, privacy-preserving Windows raw-input listener for Kage typing.

It observes only that an alphabetic key was pressed.  It never stores, prints,
or forwards the letter itself: the single effect is refreshing Kage's temporary
typing display.  Raw Input lets this coexist with normal applications without
stealing their keystrokes.
"""
from __future__ import annotations

import ctypes as C
from ctypes import wintypes as W
import logging
import os
import threading


_log = logging.getLogger("kage.typing")
_started = False
_guard = threading.Lock()
_excluded_hid = tuple(part.strip().casefold() for part in os.getenv(
    "KAGE_TYPING_EXCLUDED_HID", "vid_514c&pid_4155"
).split(",") if part.strip())
_TRIGGER_KEYS = set(range(0x41, 0x5B)) | {0x08, 0x0D, 0x20}


def start(on_key) -> None:
    """Start one daemon listener; failures leave normal Kage operation intact."""
    global _started
    with _guard:
        if _started:
            return
        _started = True
    threading.Thread(target=_listen, args=(on_key,), name="kage-typing-input",
                     daemon=True).start()


def _listen(on_key) -> None:
    try:
        user32 = C.WinDLL("user32", use_last_error=True)
        kernel32 = C.WinDLL("kernel32", use_last_error=True)
        lresult = C.c_ssize_t
        wndproc = C.WINFUNCTYPE(lresult, W.HWND, W.UINT, W.WPARAM, W.LPARAM)

        class RawInputHeader(C.Structure):
            _fields_ = [("type", W.DWORD), ("size", W.DWORD),
                        ("device", W.HANDLE), ("param", W.WPARAM)]

        class RawKeyboard(C.Structure):
            _fields_ = [("make_code", W.WORD), ("flags", W.WORD),
                        ("reserved", W.WORD), ("virtual_key", W.WORD),
                        ("message", W.UINT), ("extra", W.ULONG)]

        class RawInputDevice(C.Structure):
            _fields_ = [("usage_page", W.WORD), ("usage", W.WORD),
                        ("flags", W.DWORD), ("target", W.HWND)]

        class WindowClass(C.Structure):
            _fields_ = [("style", W.UINT), ("proc", wndproc),
                        ("class_extra", C.c_int), ("window_extra", C.c_int),
                        ("instance", W.HINSTANCE), ("icon", W.HICON),
                        ("cursor", W.HANDLE), ("background", W.HBRUSH),
                        ("menu", W.LPCWSTR), ("name", W.LPCWSTR)]

        user32.DefWindowProcW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
        user32.DefWindowProcW.restype = lresult
        user32.RegisterClassW.argtypes = [C.POINTER(WindowClass)]
        user32.CreateWindowExW.argtypes = [W.DWORD, W.LPCWSTR, W.LPCWSTR, W.DWORD,
                                           C.c_int, C.c_int, C.c_int, C.c_int,
                                           W.HWND, W.HMENU, W.HINSTANCE, C.c_void_p]
        user32.CreateWindowExW.restype = W.HWND
        user32.RegisterRawInputDevices.argtypes = [C.POINTER(RawInputDevice), W.UINT, W.UINT]
        user32.GetRawInputData.argtypes = [W.HANDLE, W.UINT, C.c_void_p,
                                           C.POINTER(W.UINT), W.UINT]
        user32.GetRawInputData.restype = W.UINT
        user32.GetRawInputDeviceInfoW.argtypes = [W.HANDLE, W.UINT, C.c_void_p,
                                                   C.POINTER(W.UINT)]
        user32.GetRawInputDeviceInfoW.restype = W.UINT
        kernel32.GetModuleHandleW.argtypes = [W.LPCWSTR]
        kernel32.GetModuleHandleW.restype = W.HMODULE

        device_names = {}

        def device_name(handle):
            if handle not in device_names:
                length = W.UINT()
                if user32.GetRawInputDeviceInfoW(handle, 0x20000007, None,
                                                 C.byref(length)) == 0xFFFFFFFF:
                    return ""
                value = C.create_unicode_buffer(length.value + 1)
                if user32.GetRawInputDeviceInfoW(handle, 0x20000007, value,
                                                 C.byref(length)) == 0xFFFFFFFF:
                    return ""
                device_names[handle] = value.value.casefold()
            return device_names[handle]

        @wndproc
        def procedure(hwnd, message, wparam, lparam):
            if message == 0x00FF:  # WM_INPUT
                size = W.UINT()
                header_size = C.sizeof(RawInputHeader)
                user32.GetRawInputData(lparam, 0x10000003, None, C.byref(size), header_size)
                if size.value >= header_size + C.sizeof(RawKeyboard):
                    data = C.create_string_buffer(size.value)
                    copied = user32.GetRawInputData(lparam, 0x10000003, data,
                                                     C.byref(size), header_size)
                    if copied != 0xFFFFFFFF:
                        header = RawInputHeader.from_buffer(data)
                        key = RawKeyboard.from_buffer(data, header_size)
                        # Key-down from the PC keyboard only. The key itself is
                        # discarded after mapping a visual zone; no text is stored.
                        source = device_name(header.device)
                        excluded = any(marker in source for marker in _excluded_hid)
                        if (header.type == 1 and not excluded and
                                not (key.flags & 0x0001) and key.virtual_key in _TRIGGER_KEYS):
                            try:
                                on_key(key.virtual_key)
                            except Exception:
                                _log.exception("typing activity callback failed")
            return user32.DefWindowProcW(hwnd, message, wparam, lparam)

        instance = kernel32.GetModuleHandleW(None)
        class_name = "KageTypingActivityRawInput"
        window_class = WindowClass(0, procedure, 0, 0, instance, None, None, None,
                                   None, class_name)
        # ERROR_CLASS_ALREADY_EXISTS is expected after an in-process reload.
        if not user32.RegisterClassW(C.byref(window_class)) and C.get_last_error() != 1410:
            raise C.WinError(C.get_last_error())
        hwnd = user32.CreateWindowExW(0, class_name, "", 0, 0, 0, 0, 0,
                                      W.HWND(-3), None, instance, None)
        if not hwnd:
            raise C.WinError(C.get_last_error())
        keyboard = RawInputDevice(1, 6, 0x100, hwnd)  # generic desktop keyboard + INPUTSINK
        if not user32.RegisterRawInputDevices(C.byref(keyboard), 1, C.sizeof(keyboard)):
            raise C.WinError(C.get_last_error())

        message = W.MSG()
        while user32.GetMessageW(C.byref(message), None, 0, 0) > 0:
            user32.TranslateMessage(C.byref(message))
            user32.DispatchMessageW(C.byref(message))
    except Exception:
        _log.exception("PC keyboard typing listener unavailable")
