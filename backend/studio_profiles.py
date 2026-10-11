"""Editable, bounded Kage face and motion profiles for the control webapp."""

from __future__ import annotations

import json
import re
import threading
import time
from pathlib import Path

from fastapi import HTTPException, Request
from fastapi.responses import HTMLResponse

ROOT = Path(__file__).parent
FACE_PATH = ROOT / "face_studio_profiles.json"
MOTION_PATH = ROOT / "motion_studio_profiles.json"
STUDIO_HTML = ROOT / "motion_studio.html"
PROFILE_NAME = re.compile(r"[a-z0-9_-]{1,32}\Z")
DEFAULT_FACE = {
    "name": "idle", "color": "#4FE3FF", "x": 0, "y": 0,
    "corners": [[-66, -66], [66, -66], [66, 66], [-66, 66]],
    "radius": 32, "mouth_width": 46, "mouth_height": 7, "mouth_y": 258,
    "mouth_style": "line",
}


def _face(name: str, color: str, corners=None, **extra):
    return {**DEFAULT_FACE, "name": name, "color": color,
            "corners": corners or [point[:] for point in DEFAULT_FACE["corners"]], **extra}


FACE_DEFAULTS = {
    "idle": _face("idle", "#4FE3FF"),
    "speaking": _face("speaking", "#4FE3FF"),
    "listening": _face("listening", "#42F58D"),
    "thinking": _face("thinking", "#B58CFF"),
    "angry": _face("angry", "#FF4057", [[-66, -52], [66, -66], [66, 52], [-66, 66]], radius=18, mouth_width=54),
    "dance": _face("dance", "#FFD84A", mouth_width=62, mouth_style="smile"),
    "explore": _face("explore", "#F4FAFF"),
    "sleep": _face("sleep", "#B58CFF", [[-66, -5], [66, -5], [66, 5], [-66, 5]], radius=5),
}


def _step(kind, ms, left=0, right=0, pan=0, tilt=0, speed=75):
    return dict(kind=kind, duration_ms=ms, left=left, right=right,
                pan=pan, tilt=tilt, speed=speed)


DANCE_STEPS = ([_step("drive", 300, -80 if i % 2 == 0 else 80,
                      -80 if i % 2 == 0 else 80) for i in range(4)] +
               [_step("drive", 360, *pair) for pair in
                [(80, -80), (-80, -80), (-80, 80), (80, 80)] * 2 +
                [(80, -80), (-80, -80)]] +
               [_step("drive", 200, -80, 80)])
MOTION_DEFAULTS = {
    "dance": {"name": "dance", "face": "dance", "steps": DANCE_STEPS},
    "360": {"name": "360", "face": "dance", "steps": [_step("drive", 2660, 100, 100), _step("pause", 0)]},
    "explore": {"name": "explore", "face": "explore", "steps": [
        _step("pan", 1000, pan=0), _step("pause", 1000),
        _step("tilt", 1000, tilt=0), _step("pause", 1000),
        _step("drive", 3000, 100, -100), _step("pause", 1000),
        _step("pan", 1000, pan=-100), _step("pause", 1000),
        _step("pan", 1000, pan=0), _step("pause", 1000),
        _step("tilt", 1000, tilt=100), _step("pause", 1000),
        _step("tilt", 1000, tilt=0), _step("pause", 1000),
        _step("drive", 3000, 100, 100), _step("pause", 1000),
        _step("drive", 3000, -100, -100), _step("pause", 1000),
        _step("servo", 1000, pan=0, tilt=0), _step("pause", 0)]},
}


def _read(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def _write(path: Path, data: dict):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def _number(value, low, high, label):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise HTTPException(400, f"{label} doit être entre {low} et {high}")
    return value


def _name(value):
    if not isinstance(value, str) or not PROFILE_NAME.fullmatch(value):
        raise HTTPException(400, "Nom : lettres minuscules, chiffres, _ ou -, 32 caractères max")
    return value


def validate_face(body):
    if not isinstance(body, dict):
        raise HTTPException(400, "Profil de visage invalide")
    name = _name(body.get("name"))
    color = body.get("color")
    if not isinstance(color, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", color):
        raise HTTPException(400, "Couleur hexadécimale invalide")
    corners = body.get("corners")
    if not isinstance(corners, list) or len(corners) != 4:
        raise HTTPException(400, "Quatre coins requis")
    points = []
    for point in corners:
        if not isinstance(point, list) or len(point) != 2:
            raise HTTPException(400, "Coin invalide")
        points.append([_number(point[0], -90, 90, "X"), _number(point[1], -90, 90, "Y")])
    mouth_style = body.get("mouth_style", "line")
    if mouth_style not in {"line", "smile", "smile_inverted"}:
        raise HTTPException(400, "Style de bouche invalide")
    return {"name": name, "color": color.upper(), "corners": points,
            "x": _number(body.get("x"), -70, 70, "Position X"),
            "y": _number(body.get("y"), -70, 70, "Position Y"),
            "radius": _number(body.get("radius"), 0, 60, "Arrondi"),
            "mouth_width": _number(body.get("mouth_width"), 0, 120, "Bouche"),
            "mouth_height": _number(body.get("mouth_height"), 0, 50, "Bouche"),
            "mouth_y": _number(body.get("mouth_y"), 220, 330, "Bouche Y"),
            "mouth_style": mouth_style}


def validate_motion(body):
    if not isinstance(body, dict):
        raise HTTPException(400, "Mouvement invalide")
    name = _name(body.get("name"))
    face = _name(body.get("face"))
    steps = body.get("steps")
    if not isinstance(steps, list) or not 1 <= len(steps) <= 40:
        raise HTTPException(400, "Il faut 1 à 40 étapes")
    checked = []
    for step in steps:
        if not isinstance(step, dict) or step.get("kind") not in {"drive", "servo", "pan", "tilt", "parallel", "pause"}:
            raise HTTPException(400, "Type d’étape invalide")
        checked.append({"kind": step["kind"],
                        "duration_ms": _number(step.get("duration_ms"), 0, 3000, "Durée"),
                        "left": _number(step.get("left"), -100, 100, "Chenille gauche"),
                        "right": _number(step.get("right"), -100, 100, "Chenille droite"),
                        "pan": _number(step.get("pan"), -100, 100, "Pan"),
                        "tilt": _number(step.get("tilt"), 0, 100, "Tilt"),
                        "speed": _number(step.get("speed"), 10, 100, "Vitesse servo")})
    if sum(s["duration_ms"] for s in checked) > 30000:
        raise HTTPException(400, "Séquence limitée à 30 secondes")
    return {"name": name, "face": face, "steps": checked}


class Studio:
    def __init__(self, set_command, current_state):
        self.set_command = set_command
        self.current_state = current_state
        self.lock = threading.RLock()
        self.faces = {**FACE_DEFAULTS, **_read(FACE_PATH)}
        self.motions = {**MOTION_DEFAULTS, **_read(MOTION_PATH)}
        self.active_face = "idle"
        self.manual_face = False
        self.face_revision = 0
        self.run_id = 0
        self.running = ""

    def face_status(self):
        with self.lock:
            if not self.running and not self.manual_face:
                assistant = self.current_state().get("assistant_state", "idle")
                target = assistant if assistant in {"speaking", "listening", "thinking"} else "idle"
                if target != self.active_face and target in self.faces:
                    self.active_face = target
                    self.face_revision += 1
            p = self.faces[self.active_face]
            flat = [int(p["color"][1:], 16), p["x"], p["y"], p["radius"],
                    p["mouth_width"], p["mouth_height"], p["mouth_y"]]
            flat.extend(v for point in p["corners"] for v in point)
            flat.append({"line": 0, "smile": 1, "smile_inverted": 2}.get(p.get("mouth_style"), 0))
            return {"face_rev": self.face_revision, "face": flat}

    def run(self, name):
        with self.lock:
            profile = json.loads(json.dumps(self.motions[name]))
            self.run_id += 1
            run_id = self.run_id
            self.running = name
            self.active_face = profile["face"] if profile["face"] in self.faces else "idle"
            self.manual_face = False
            self.face_revision += 1
        encoded = bytearray()
        for step in profile["steps"] + [_step("servo", 800, pan=0, tilt=0)]:
            kind = step["kind"]
            left = step["left"] if kind in {"drive", "parallel"} else 0
            right = step["right"] if kind in {"drive", "parallel"} else 0
            pan = step["pan"] if kind in {"servo", "pan", "parallel"} else (127 if kind == "tilt" else 0)
            tilt = step["tilt"] if kind in {"servo", "tilt", "parallel"} else (127 if kind == "pan" else 0)
            duration = step["duration_ms"]
            encoded.extend((left & 255, right & 255, pan & 255, tilt,
                            step["speed"], duration & 255, duration >> 8))
        command = "studioseq:" + encoded.hex()
        total_seconds = sum(s["duration_ms"] for s in profile["steps"]) / 1000 + 0.8
        first_issued = threading.Event()
        def worker():
            try:
                with self.lock:
                    if run_id != self.run_id:
                        return
                    self.set_command(command)
                    first_issued.set()
                # The board may use the remote Tailscale route (2.5 s poll).
                # It executes every stage locally after receiving this one command.
                deadline = time.monotonic() + total_seconds + 3.5
                while time.monotonic() < deadline:
                    time.sleep(0.1)
                    with self.lock:
                        if run_id != self.run_id:
                            return
                        if self.current_state()["command"] != command:
                            return
            finally:
                first_issued.set()
                with self.lock:
                    if run_id == self.run_id:
                        if self.current_state()["command"] == command:
                            self.set_command("motion_stop")
                        self.active_face = "idle"
                        self.face_revision += 1
                        self.running = ""
        threading.Thread(target=worker, name=f"kage-studio-{name}", daemon=True).start()
        first_issued.wait(timeout=1)

    def stop(self):
        with self.lock:
            self.run_id += 1
            self.running = ""
            self.active_face = "idle"
            self.manual_face = False
            self.face_revision += 1
            self.set_command("motion_stop")

    def note_command(self, command):
        face = {"angry": "angry", "sleep": "sleep", "idle": "idle"}.get(command)
        if face and face in self.faces:
            with self.lock:
                self.active_face = face
                self.manual_face = command != "idle"
                self.face_revision += 1


def register(app, require_key, set_command, current_state):
    studio = Studio(set_command, current_state)

    @app.get("/control/studio", response_class=HTMLResponse)
    def studio_page(request: Request):
        return HTMLResponse(STUDIO_HTML.read_text(encoding="utf-8"),
                            headers={"Cache-Control": "no-store, max-age=0"})

    @app.get("/control/studio/data")
    def studio_data(request: Request):
        require_key(request)
        with studio.lock:
            return {"faces": studio.faces, "motions": studio.motions,
                    "defaults": {"faces": FACE_DEFAULTS, "motions": MOTION_DEFAULTS},
                    "active_face": studio.active_face, "running": studio.running}

    @app.post("/control/studio/face")
    async def save_face(request: Request):
        require_key(request)
        face = validate_face(await request.json())
        with studio.lock:
            studio.faces[face["name"]] = face
            if face["name"] == studio.active_face:
                studio.face_revision += 1
            _write(FACE_PATH, {k: v for k, v in studio.faces.items() if k not in FACE_DEFAULTS or v != FACE_DEFAULTS[k]})
        return {"ok": True, "face": face}

    @app.post("/control/studio/face/{name}/apply")
    def apply_face(name: str, request: Request):
        require_key(request)
        with studio.lock:
            if name not in studio.faces:
                raise HTTPException(404, "Profil introuvable")
            studio.active_face = name
            studio.manual_face = True
            studio.face_revision += 1
        return {"ok": True, "name": name}

    @app.delete("/control/studio/face/{name}")
    def delete_face(name: str, request: Request):
        require_key(request)
        with studio.lock:
            if name in FACE_DEFAULTS:
                raise HTTPException(400, "Les profils Kage de base ne peuvent pas être supprimés")
            if name not in studio.faces:
                raise HTTPException(404, "Profil introuvable")
            del studio.faces[name]
            if studio.active_face == name:
                studio.active_face = "idle"
                studio.manual_face = False
                studio.face_revision += 1
            _write(FACE_PATH, {k: v for k, v in studio.faces.items() if k not in FACE_DEFAULTS or v != FACE_DEFAULTS[k]})
        return {"ok": True, "name": name}

    @app.post("/control/studio/motion")
    async def save_motion(request: Request):
        require_key(request)
        motion = validate_motion(await request.json())
        with studio.lock:
            if motion["face"] not in studio.faces:
                raise HTTPException(400, "Visage associé introuvable")
            studio.motions[motion["name"]] = motion
            _write(MOTION_PATH, {k: v for k, v in studio.motions.items() if k not in MOTION_DEFAULTS or v != MOTION_DEFAULTS[k]})
        return {"ok": True, "motion": motion}

    @app.post("/control/studio/motion/{name}/run")
    def run_motion(name: str, request: Request):
        require_key(request)
        with studio.lock:
            if name not in studio.motions:
                raise HTTPException(404, "Mouvement introuvable")
        studio.run(name)
        return {"ok": True, "name": name}

    @app.post("/control/studio/stop")
    def stop_motion(request: Request):
        require_key(request)
        studio.stop()
        return {"ok": True}

    return studio
