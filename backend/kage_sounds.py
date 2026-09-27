"""Tiny non-blocking local sound cues used while testing Kage on the PC."""

from __future__ import annotations

import ctypes
import itertools
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


SOUND_DIRECTORY = Path(r"C:\Kage\ui_sounds")
SOUNDS = {
    "thinking": ("minimize-006.mp3", 0.30),
    "triple_open": ("maximize-007.mp3", 0.30),
    "triple_close": ("minimize-008.mp3", 0.30),
    "wake_listening": ("confirmation-001.mp3", 0.29),
    "cancel_thinking": ("error-005.mp3", 0.35),
    "stop_speaking": ("drop-004.mp3", 0.30),
    "home_command": ("confirmation-002.mp3", 0.30),
    "sleep_listening": ("minimize-004.mp3", 0.30),
    "recording_loop": ("zen-recording.mp3", 1.00),
}

_executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="kage-sfx")
_counter = itertools.count()
_mci_lock = threading.Lock()


def _mci(command: str) -> None:
    # Windows MCI plays MP3 natively and returns immediately for `play`.
    ctypes.windll.winmm.mciSendStringW(command, None, 0, None)


def _play(name: str) -> None:
    spec = SOUNDS.get(name)
    if not spec:
        return
    filename, duration = spec
    path = SOUND_DIRECTORY / filename
    if not path.is_file():
        return
    alias = f"kage_sfx_{next(_counter)}"
    try:
        # Serialise only the MCI open/close commands. Playback itself remains
        # asynchronous and the caller never waits for audio.
        with _mci_lock:
            _mci(f'open "{path}" type mpegvideo alias {alias}')
            _mci(f"setaudio {alias} volume to 650")
            _mci(f"play {alias} from 0")
        time.sleep(duration + 0.5)
    finally:
        with _mci_lock:
            _mci(f"close {alias}")


def play_sound(name: str) -> None:
    """Schedule a cue immediately; audio never blocks or gates Kage actions."""
    _executor.submit(_play, name)


def start_loop(name: str):
    """Start a seamless MCI loop and return its handle, or None."""
    spec = SOUNDS.get(name)
    if not spec:
        return None
    path = SOUND_DIRECTORY / spec[0]
    if not path.is_file():
        return None
    alias = f"kage_loop_{next(_counter)}"
    with _mci_lock:
        _mci(f'open "{path}" type mpegvideo alias {alias}')
        _mci(f"setaudio {alias} volume to 420")
        _mci(f"play {alias} from 0 repeat")
    return alias


def stop_loop(alias) -> None:
    """Stop a loop handle safely; repeated cleanup is harmless."""
    if not alias:
        return
    with _mci_lock:
        _mci(f"stop {alias}")
        _mci(f"close {alias}")
