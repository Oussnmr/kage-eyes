"""Isolated, authenticated Kage LVGL browser-simulator endpoints.

This module never sends a command to the ESP32 and never mutates face profiles.
Profiles only store validated geometry (32 integers); they do not execute code.
"""
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse
import json
import os
from pathlib import Path
import re
import tempfile
import threading

SIM_DIR = Path(__file__).with_name("typing_simulator_dist")
SIM_PAGE = Path(__file__).with_name("typing_simulator.html")
PROFILES_PATH = Path(os.getenv(
    "KAGE_TYPING_PROFILES_PATH",
    str(Path(__file__).with_name("typing_profiles.json")),
))
_guard = threading.RLock()
_name_pattern = re.compile(r"^[a-zA-Z0-9_-]{1,40}$")
FIELDS = 32
_LEGACY_FIELDS = 28


def _normalise_values(values):
    """Validate current profiles and losslessly upgrade the former 28-field layout."""
    if not isinstance(values, list) or any(type(value) is not int for value in values):
        return None
    if len(values) == _LEGACY_FIELDS:
        # Original layout ended the arm geometry with left/centre/right X, touch
        # Y and thickness. Insert the five-region positions without changing the
        # old values or the animation timings that follow them.
        values = (values[:21] + [365, 43, values[21], 218, 282, values[22]]
                  + values[23:])
    if len(values) != FIELDS or any(abs(value) > 20000 for value in values):
        return None
    return values


def _load():
    try:
        data = json.loads(PROFILES_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    profiles = {}
    migrated = False
    for name, values in data.items():
        if not _name_pattern.fullmatch(name):
            continue
        normalised = _normalise_values(values)
        if normalised is None:
            continue
        migrated |= normalised != values
        profiles[name] = normalised
    if migrated:
        _persist(profiles)
    return profiles


def _persist(profiles):
    PROFILES_PATH.parent.mkdir(parents=True, exist_ok=True)
    name = None
    try:
        with tempfile.NamedTemporaryFile("w", dir=PROFILES_PATH.parent,
                                         prefix="typing-", suffix=".tmp",
                                         encoding="utf-8", delete=False) as handle:
            name = handle.name
            json.dump(profiles, handle, ensure_ascii=False, indent=2)
        os.replace(name, PROFILES_PATH)
    finally:
        if name and Path(name).exists():
            os.unlink(name)


def make_typing_router(require_kage_key):
    router = APIRouter()

    @router.get("/control/simulator", response_class=HTMLResponse)
    def simulator_page():
        return HTMLResponse(SIM_PAGE.read_text(encoding="utf-8"),
                            headers={"Cache-Control": "no-store, max-age=0"})

    @router.get("/control/simulator/assets/{filename}")
    def simulator_asset(filename: str):
        if filename not in ("kage_lvgl.js", "kage_lvgl.wasm"):
            raise HTTPException(status_code=404, detail="Asset inconnu")
        file = SIM_DIR / filename
        if not file.is_file():
            raise HTTPException(status_code=503,
                                detail="LVGL WebAssembly non compilé. Consulte le workflow typing-simulator.")
        media = "application/wasm" if filename.endswith(".wasm") else "text/javascript"
        return FileResponse(file, media_type=media,
                            headers={"Cache-Control": "no-store"})

    @router.get("/control/simulator/profiles")
    def list_profiles(request: Request):
        require_kage_key(request)
        with _guard:
            return {"profiles": _load()}

    @router.put("/control/simulator/profiles/{name}")
    def save_profile(name: str, values: list[int], request: Request):
        require_kage_key(request)
        if not _name_pattern.fullmatch(name) or name == "default":
            raise HTTPException(422, "Nom invalide ou réservé")
        if _normalise_values(values) is None or len(values) != FIELDS:
            raise HTTPException(422, "Un profil doit contenir exactement 32 entiers bornés")
        with _guard:
            data = _load()
            if name not in data and len(data) >= 40:
                raise HTTPException(400, "Nombre maximal de profils atteint")
            data[name] = values
            _persist(data)
        return {"ok": True, "name": name}

    @router.delete("/control/simulator/profiles/{name}")
    def delete_profile(name: str, request: Request):
        require_kage_key(request)
        if not _name_pattern.fullmatch(name) or name == "default":
            raise HTTPException(422, "Nom invalide ou réservé")
        with _guard:
            data = _load()
            if name not in data:
                raise HTTPException(404, "Profil introuvable")
            del data[name]
            _persist(data)
        return {"ok": True}

    return router
