from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, StreamingResponse
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from tts_service import kokoro_service
from codex_bridge import CodexBridgeError, codex_bridge
from web_search import (
    format_web_context,
    is_web_continuation,
    needs_web_search,
    search_web,
)
from kage_sounds import play_sound
from command_intents import direct_intent, home_intent, looks_like_home_command

from faster_whisper import WhisperModel

import hmac
import json
import os
import queue
import re
import secrets
import subprocess
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
import urllib.request
import urllib.error
import wave
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

KAGE_TTS_VOICE = os.getenv("KAGE_TTS_VOICE", "ff_siwis").strip() or "ff_siwis"

try:
    import pyttsx3
except ImportError:
    pyttsx3 = None

try:
    import pythoncom
except ImportError:
    pythoncom = None


app = FastAPI(title="Kage M920q Backend", docs_url=None, redoc_url=None, openapi_url=None)

VALID_COMMANDS = {
    "idle", "blink", "sleep", "angry", "dizzy",
    "move_h", "move_v", "move_f", "move_b",
    "drive_f", "drive_b", "drive_l", "drive_r", "motion_stop",
    "pan_l", "pan_r", "tilt_u", "tilt_d",
}
VALID_ASSISTANT_STATES = {"idle", "listening", "thinking", "speaking", "error", "offline"}
OLLAMA_URL = "http://127.0.0.1:11434/api/generate"
OLLAMA_MODEL = "qwen3:4b-instruct-2507-q4_K_M"
DEFAULT_CONVERSATION_BACKEND = os.getenv("KAGE_CONVERSATION_BACKEND", "ollama").strip().lower()
SETTINGS_PATH = Path(os.getenv("KAGE_SETTINGS_PATH", r"C:\Kage\kage_settings.json"))
KAGE_API_KEY = os.getenv("KAGE_API_KEY", "").strip()
WEB_PIPELINE_VERSION = "web-progressive-v4.0"
RESEARCH_MEMORY_TTL_SECONDS = 15 * 60
CONTROL_PAGE_PATH = Path(__file__).with_name("control.html")
CONTROL_SESSION_TTL_SECONDS = 30 * 24 * 60 * 60
control_session_lock = threading.Lock()
control_sessions = {}

AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_SAMPLE_WIDTH = 2
MAX_AUDIO_BYTES = AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_WIDTH * 12
TTS_ENABLED = os.getenv("KAGE_PC_TTS", "0").strip().lower() in {"1", "true", "yes", "on"}
WHISPER_MODEL_NAME = os.getenv("KAGE_WHISPER_MODEL", "small").strip() or "small"
DOCKER_CLI = Path(os.getenv(
    "KAGE_DOCKER_CLI",
    r"C:\Users\Oussama\AppData\Local\Programs\DockerDesktop\resources\bin\docker.exe",
))
HOME_BRIDGE_CONTAINER = os.getenv("KAGE_HOME_BRIDGE_CONTAINER", "home-assistant-homeassistant-1")
HOME_BRIDGE_URL = os.getenv("KAGE_HOME_BRIDGE_URL", "http://127.0.0.1:8787").rstrip("/")
HOME_DEVICES = {
    "light": ("40400515e868e76da5da", "switch_led", ("light", "lights", "the light", "main light", "ceiling light", "ceiling", "plafonier", "plafonnier")),
    "lamp": ("bf9a6b44c155f86cc6zyyl", "switch_1", ("lamp", "the lamp", "desk lamp", "bedside lamp", "lampe")),
    "projector": ("bfe6d607527039a8e4wlik", "switch_1", ("projector", "the projector", "beamer", "projecteur")),
    "leds": ("bf1db5280dc2ea5d5buhoy", "switch_1", ("led", "leds", "the leds", "led light", "led lights", "lumiere led", "lumières led")),
    "desk": ("bfb8713df16202297exsnh", "switch_1", ("desk", "the desk", "desk power", "desk plug", "desk plugs", "power strip", "power strips", "multiprise", "multiprises")),
}
HOME_PROFILES = {
    "nightshift": {
        "aliases": ("nightshift", "night shift", "night mode"),
        "actions": (("leds", True), ("desk", True), ("light", False), ("lamp", True)),
    },
    "rest": {
        "aliases": ("rest mode", "restmode", "rest time"),
        "actions": (("lamp", False), ("leds", False), ("light", False), ("desk", False)),
    },
}

if not KAGE_API_KEY:
    print("ATTENTION: KAGE_API_KEY absent. Les endpoints protégés refuseront les requêtes.")

state_lock = threading.Lock()
state = {
    "command": "idle",
    "sequence": 0,
    "assistant_state": "idle",
    "assistant_sequence": 0,
    "voice_active": False,
    "voice_last_seen": 0.0,
}
VOICE_HEARTBEAT_TIMEOUT_SECONDS = 6.0
backend_lock = threading.Lock()
home_control_lock = threading.Lock()
VOICE_INTERRUPT_FLAG = Path(r"C:\Kage\voice_interrupt.flag")
VOICE_WAKE_FLAG = Path(r"C:\Kage\voice_wake.flag")
VOICE_SLEEP_FLAG = Path(r"C:\Kage\voice_sleep.flag")
VOICE_HOLD_ACTIVE_FLAG = Path(r"C:\Kage\voice_hold_active.flag")
VOICE_TEXT_MODE_FLAG = Path(r"C:\Kage\voice_text_mode.flag")
voice_control_lock = threading.Lock()
research_memory_lock = threading.Lock()
research_memory = {
    "generation": 0,
    "query": None,
    "data": None,
    "status": "empty",
    "updated_at": 0.0,
    "ready": None,
    "continuations": 0,
}


def start_background_research(message: str) -> None:
    """Enrich the latest Web query without blocking the spoken response."""
    ready = threading.Event()
    with research_memory_lock:
        generation = int(research_memory["generation"]) + 1
        research_memory.update({
            "generation": generation,
            "query": message,
            "data": None,
            "status": "running",
            "updated_at": time.monotonic(),
            "ready": ready,
            "continuations": 0,
        })

    def research() -> None:
        started = time.perf_counter()
        data = search_web(message)
        with research_memory_lock:
            if generation != research_memory["generation"]:
                ready.set()
                return
            research_memory.update({
                "data": data,
                "status": "ready" if data.get("results") else "unavailable",
                "updated_at": time.monotonic(),
            })
            ready.set()
        print(json.dumps({
            "event": "background_research_completed",
            "duration_ms": round((time.perf_counter() - started) * 1000, 1),
            "results": len(data.get("results", [])),
            "available": bool(data.get("results")),
        }, ensure_ascii=False))

    threading.Thread(target=research, name="kage-background-research", daemon=True).start()


def clear_research_memory() -> None:
    """Detach stale research whenever the conversation moves to a new topic."""
    with research_memory_lock:
        previous_ready = research_memory.get("ready")
        research_memory.update({
            "generation": int(research_memory["generation"]) + 1,
            "query": None,
            "data": None,
            "status": "empty",
            "updated_at": time.monotonic(),
            "ready": None,
            "continuations": 0,
        })
        if previous_ready is not None:
            previous_ready.set()


def latest_research(wait_seconds: float = 0.0) -> dict | None:
    """Return fresh completed research, optionally waiting briefly for it."""
    with research_memory_lock:
        if (not research_memory["query"] or
                time.monotonic() - float(research_memory["updated_at"]) > RESEARCH_MEMORY_TTL_SECONDS):
            return None
        ready = research_memory["ready"]
    if ready is not None and wait_seconds > 0:
        ready.wait(timeout=wait_seconds)
    with research_memory_lock:
        if research_memory["status"] != "ready" or not research_memory["data"]:
            return None
        research_memory["continuations"] = int(research_memory["continuations"]) + 1
        return {
            "query": research_memory["query"],
            "data": research_memory["data"],
            "continuations": research_memory["continuations"],
        }


def research_status() -> str:
    with research_memory_lock:
        return str(research_memory["status"])


def prepare_codex_web_request(message: str) -> tuple[str, str | None, bool, bool]:
    """Choose a new foreground query or the immediately preceding research."""
    if is_web_continuation(message):
        cached = latest_research(wait_seconds=1.5)
        if cached:
            follow_up = (
                f"L'utilisateur demande de continuer la recherche précédente sur : {cached['query']}. "
                "Donne uniquement des faits complémentaires que tu n'as pas encore mentionnés."
            )
            print(json.dumps({
                "event": "background_research_reused",
                "continuation": cached["continuations"],
                "results": len(cached["data"].get("results", [])),
            }, ensure_ascii=False))
            return follow_up, format_web_context(cached["data"]), False, False

    # A bare continuation always refers to the immediately preceding answer.
    # Once another topic begins, older research must never leak into it.
    clear_research_memory()
    explicit_web_request = needs_web_search(message)
    if not explicit_web_request:
        return message, None, False, False

    # The foreground turn gets one focused native search. A broader DDGS pass
    # starts on its first text delta and is retained only for a later
    # "continue" request. This avoids putting two searches on the critical path.
    return message, None, explicit_web_request, True


def home_bridge_authorization() -> str:
    """Read the existing bridge-only token without logging or persisting it."""
    explicit = os.getenv("KAGE_HOME_BRIDGE_TOKEN", "").strip()
    if explicit:
        return explicit if explicit.lower().startswith("bearer ") else f"Bearer {explicit}"
    if not DOCKER_CLI.is_file():
        return ""
    try:
        result = subprocess.run(
            [str(DOCKER_CLI), "exec", HOME_BRIDGE_CONTAINER, "sh", "-c",
             "grep '^kage_bridge_authorization:' /config/secrets.yaml"],
            capture_output=True, text=True, timeout=4,
        )
        match = re.search(r"kage_bridge_authorization:\s*[\"']?(Bearer\s+[^\"'\s]+)", result.stdout)
        return match.group(1) if match else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def execute_home_action(name: str, value: bool, authorization: str) -> tuple[str, bool]:
    """Send one device action and confirm it with the bridge polling contract."""
    device_id, code, _ = HOME_DEVICES[name]
    # TinyTuya persistent connections can retain stale receive data on plugs.
    # Use the bridge's short-lived mode for plugs only; the ceiling light keeps
    # its established persistent path.
    transient_headers = {"X-Tuya-Transient": "1"} if name in {"lamp", "projector", "leds", "desk"} else {}
    payload = json.dumps({"commands": [{"code": code, "value": value}]}).encode("utf-8")
    command_request = urllib.request.Request(
        f"{HOME_BRIDGE_URL}/device/{device_id}/commands", data=payload,
        headers={"Authorization": authorization, "Content-Type": "application/json", **transient_headers}, method="POST",
    )
    status_request = urllib.request.Request(
        f"{HOME_BRIDGE_URL}/device/{device_id}/status",
        headers={"Authorization": authorization, **transient_headers}, method="GET",
    )
    for command_attempt in range(3):
        try:
            with urllib.request.urlopen(command_request, timeout=18) as response:
                body = json.loads(response.read().decode("utf-8"))
            accepted = 200 <= response.status < 300 and body.get("success") is True
        except (urllib.error.URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError):
            accepted = False
        if accepted:
            for _ in range(6):
                time.sleep(0.4)
                try:
                    with urllib.request.urlopen(status_request, timeout=12) as response:
                        status = json.loads(response.read().decode("utf-8"))
                    returned = status.get("result", []) if isinstance(status, dict) else []
                    if (200 <= response.status < 300 and status.get("success") is True
                            and any(item.get("code") == code and item.get("value") == value
                                    for item in returned if isinstance(item, dict))):
                        return name, True
                except (urllib.error.URLError, TimeoutError, OSError, ValueError, json.JSONDecodeError):
                    pass
        time.sleep(0.7 * (command_attempt + 1))
    return name, False


def apply_home_actions(message: str, actions, profile: str | None = None):
    """Execute local home actions; profiles dispatch every device simultaneously."""
    authorization = home_bridge_authorization()
    if not authorization:
        return {"ok": False, "reply": "Home control is not available right now.", "command": "none"}
    # Start this cue before the action. It runs asynchronously and never gates
    # the home bridge request.
    play_sound("home_command")
    with home_control_lock:
        if profile:
            # Each device has its own bridge lock, so the commands can leave at
            # the same time while their confirmations remain independent.
            with ThreadPoolExecutor(max_workers=len(actions)) as executor:
                results = list(executor.map(lambda item: execute_home_action(*item, authorization), actions))
        else:
            results = [execute_home_action(name, value, authorization) for name, value in actions]
    changed = [name for name, confirmed in results if confirmed]
    failed = [name for name, confirmed in results if not confirmed]
    if profile and changed:
        reply = f"{profile.title()} is on."
    elif changed:
        reply = "I updated " + ", ".join(f"{name} {'on' if value else 'off'}" for name, value in actions if name in changed) + "."
    else:
        reply = "I could not reach the home devices."
    if failed and changed:
        reply += f" I could not change {', '.join(failed)}."
    print(json.dumps({"event": "home_control", "profile": profile,
                      "actions": list(actions), "devices": changed, "failed": failed}, ensure_ascii=False))
    current = current_state()
    # Home controls are intentionally silent. The voice client will return to
    # its normal listening window after the local action completes.
    return {"ok": bool(changed), "heard": normalize_kage_name(message), "reply": "",
            "command": "none", "sequence": current["sequence"], "route": "home_control", "speak": False}


def route_home_control(message: str):
    """Route every recognized home-device request directly to the local Python bridge."""
    intent = home_intent(message)
    if intent is None:
        if looks_like_home_command(message):
            current = current_state()
            return {
                "ok": False,
                "heard": normalize_kage_name(message),
                "reply": "Je n'ai pas compris la commande. Répète seulement l'action et l'appareil.",
                "command": "none",
                "sequence": current["sequence"],
                "route": "home_clarification",
                "speak": True,
            }
        return None
    if intent[0] == "profile":
        profile_name = intent[1]
        return apply_home_actions(message, HOME_PROFILES[profile_name]["actions"], profile_name)
    _, targets, turn_on = intent
    return apply_home_actions(message, tuple((name, turn_on) for name in targets))


def load_conversation_backend() -> str:
    try:
        saved = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        backend = str(saved.get("conversation_backend", "")).strip().lower()
        if backend in {"ollama", "codex"}:
            return backend
    except (OSError, json.JSONDecodeError):
        pass
    return DEFAULT_CONVERSATION_BACKEND if DEFAULT_CONVERSATION_BACKEND in {"ollama", "codex"} else "ollama"


conversation_backend = load_conversation_backend()
response_language = "en"
try:
    _saved_settings = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
    if _saved_settings.get("response_language") in {"fr", "en"}:
        response_language = _saved_settings["response_language"]
except (OSError, json.JSONDecodeError):
    pass


def _save_settings(**updates) -> None:
    try:
        saved = json.loads(SETTINGS_PATH.read_text(encoding="utf-8"))
        if not isinstance(saved, dict):
            saved = {}
    except (OSError, json.JSONDecodeError):
        saved = {}
    saved.update(updates)
    SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = SETTINGS_PATH.with_suffix(".tmp")
    temporary_path.write_text(json.dumps(saved, indent=2) + "\n", encoding="utf-8")
    temporary_path.replace(SETTINGS_PATH)


def get_conversation_backend() -> str:
    with backend_lock:
        return conversation_backend


def set_conversation_backend(backend: str) -> str:
    if backend not in {"ollama", "codex"}:
        raise ValueError(f"Unsupported conversation backend: {backend}")
    with backend_lock:
        global conversation_backend
        _save_settings(conversation_backend=backend)
        conversation_backend = backend
        return conversation_backend


def get_response_language() -> str:
    with backend_lock:
        return response_language


def set_response_language(language: str) -> str:
    if language not in {"fr", "en"}:
        raise ValueError(f"Unsupported response language: {language}")
    with backend_lock:
        global response_language
        _save_settings(
            response_language=language,
            tts_voice="am_puck" if language == "en" else "ff_siwis",
        )
        response_language = language
        return response_language

print(f"Chargement de Whisper {WHISPER_MODEL_NAME}...")
whisper_model = WhisperModel(
    WHISPER_MODEL_NAME,
    device="cpu",
    compute_type="int8",
)
print(f"Whisper {WHISPER_MODEL_NAME} prêt.")


class AskRequest(BaseModel):
    message: str


class SpeechRequest(BaseModel):
    text: str


class DriveControlRequest(BaseModel):
    left: int
    right: int


class ServoControlRequest(BaseModel):
    pan: int
    tilt: int
    voice: str = KAGE_TTS_VOICE


def require_kage_key(request: Request) -> None:
    if not KAGE_API_KEY:
        raise HTTPException(
            status_code=503,
            detail="KAGE_API_KEY n'est pas configurée sur le M920q",
        )

    provided = request.headers.get("x-kage-key", "")
    header_valid = bool(provided) and hmac.compare_digest(provided, KAGE_API_KEY)
    session = request.cookies.get("kage-control", "")
    now = time.monotonic()
    with control_session_lock:
        expires = control_sessions.get(session, 0.0)
        session_valid = bool(session) and expires > now
        if session and not session_valid:
            control_sessions.pop(session, None)
    if not header_valid and not session_valid:
        raise HTTPException(status_code=401, detail="Clé Kage invalide")


def normalize_kage_name(text: str) -> str:
    if not text:
        return text

    aliases = r"\b(?:cagée|cagé|cage|cager|kagé|kage)\b"
    return re.sub(aliases, "Kage", text, flags=re.IGNORECASE)


def set_command(command: str) -> int:
    with state_lock:
        state["command"] = command
        state["sequence"] += 1
        return state["sequence"]


def set_assistant_state(assistant_state: str) -> int:
    with state_lock:
        if state["assistant_state"] != assistant_state:
            state["assistant_state"] = assistant_state
            state["assistant_sequence"] += 1
        return state["assistant_sequence"]


def set_voice_active(active: bool) -> bool:
    with state_lock:
        state["voice_active"] = bool(active)
        state["voice_last_seen"] = time.monotonic() if active else 0.0
        return state["voice_active"]


def current_state() -> dict:
    with state_lock:
        # Closing the PowerShell host can terminate voice.py before atexit has
        # time to publish its shutdown. A stale heartbeat is authoritative.
        if (state["voice_active"] and state["voice_last_seen"]
                and time.monotonic() - state["voice_last_seen"] > VOICE_HEARTBEAT_TIMEOUT_SECONDS):
            state["voice_active"] = False
            state["voice_last_seen"] = 0.0
            if state["assistant_state"] != "idle":
                state["assistant_state"] = "idle"
                state["assistant_sequence"] += 1
        return dict(state)


def route_direct_command(message: str):
    """Return a local command result for unambiguous, supported intents."""
    intent = direct_intent(message)
    if intent is None:
        return None
    kind, value = intent
    if kind == "language":
        language = set_response_language(value)
        current = current_state()
        reply = "I will answer in English." if language == "en" else "Je répondrai en français."
        print(json.dumps({
            "event": "response_language_switched",
            "language": language,
            "voice": "am_puck" if language == "en" else "ff_siwis",
        }, ensure_ascii=False))
        return {
            "ok": True, "heard": normalize_kage_name(message), "reply": reply,
            "command": "none", "sequence": current["sequence"], "route": "direct",
            "speak": True, "response_language": language,
            "tts_voice": "am_puck" if language == "en" else "ff_siwis",
        }
    if kind == "backend":
        backend = set_conversation_backend(value)
        print(json.dumps({"event": "backend_switched", "backend": backend}, ensure_ascii=False))
        current = current_state()
        reply = "D'accord, je passe sur ChatGPT." if backend == "codex" else "D'accord, je passe en mode local."
        return {
            "ok": True, "heard": normalize_kage_name(message), "reply": reply,
            "command": "none", "sequence": current["sequence"], "route": "direct",
            "speak": True, "conversation_backend": backend,
        }
    sequence = set_command(value)
    print(json.dumps({"event": "direct_route", "route": value, "latency_ms": 0}, ensure_ascii=False))
    return {
        "ok": True, "heard": normalize_kage_name(message), "reply": "",
        "command": value, "sequence": sequence, "route": "direct", "speak": False,
    }

def process_message(message: str) -> dict:
    llm_started = time.perf_counter()
    message = normalize_kage_name(message.strip())
    if not message:
        raise HTTPException(status_code=400, detail="Message vide")

    direct = route_direct_command(message)
    if direct is not None:
        return direct

    home_control = route_home_control(message)
    if home_control is not None:
        return home_control

    backend = get_conversation_backend()
    explicit_web_request = needs_web_search(message)
    web_context = None
    # Codex has its own hosted search tool and decides when current information
    # is needed. Keep the local DDGS adapter only for the Ollama fallback.
    if backend != "codex" and explicit_web_request:
        web_data = search_web(message)
        web_context = format_web_context(web_data)
        print(json.dumps({
            "event": "web_search",
            "results": len(web_data.get("results", [])),
            "available": not bool(web_data.get("error")),
        }, ensure_ascii=False))

    language = get_response_language()
    if backend == "codex":
        try:
            codex_message, web_context, explicit_web_request, enrich_after = prepare_codex_web_request(message)
            codex = codex_bridge.ask(
                codex_message,
                web_context=web_context,
                response_language=language,
                web_search_requested=explicit_web_request,
                timeout=75,
            )
            if enrich_after or codex.get("web_search_calls", 0) > 0:
                start_background_research(message)
            reply = codex["reply"] or "Je n'ai pas réussi à formuler une réponse."
            current = current_state()
            print(json.dumps({
                "event": "codex_completed",
                "first_delta_ms": codex["first_delta_ms"],
                "total_ms": codex["total_ms"],
                "model": codex["model"],
                "web_search_calls": codex.get("web_search_calls", 0),
                "web_search_mode": codex.get("web_search_mode"),
                "web_pipeline": WEB_PIPELINE_VERSION,
            }, ensure_ascii=False))
            return {
                "ok": True,
                "heard": message,
                "reply": reply,
                "command": "none",
                "sequence": current["sequence"],
                "route": "codex",
                "conversation_backend": backend,
                "speak": True,
            }
        except CodexBridgeError as exc:
            print(json.dumps({"event": "codex_fallback", "reason": str(exc)}, ensure_ascii=False))

    language_instruction = (
        "Answer only in English, briefly and naturally, even when the user speaks French."
        if language == "en" else
        "Réponds toujours en français, brièvement et naturellement, même si une source ou un terme est en anglais."
    )
    reply_example = "your short answer in English" if language == "en" else "ta réponse courte en français"
    prompt = f"""
    Tu es l'assistant d'un petit robot de bureau nommé Kagé.
    {language_instruction}
    Pour les prix, coûts ou montants, utilise les euros (EUR/€) par défaut.
    N'utilise une autre monnaie que si l'utilisateur le demande explicitement.

Tu peux demander UNE réaction physique parmi:
idle, blink, sleep, angry, dizzy, none.

    Choisis une réaction seulement lorsqu'elle est pertinente.
    Ne prétends jamais avoir exécuté une action qui n'existe pas.

    Demande de l'utilisateur :
{message}

    {web_context or "Réponds utilement à partir de tes connaissances générales."}

    Réponds uniquement avec un objet JSON exactement sous cette forme :
    {{"command":"none","reply":"{reply_example}"}}
""".strip()

    payload = json.dumps(
        {
            "model": OLLAMA_MODEL,
            "prompt": prompt,
            "stream": False,
            "think": False,
            "format": "json",
            "keep_alive": "30m",
            "options": {
                "temperature": 0.2,
                "num_predict": 80,
            },
        },
        ensure_ascii=False,
    ).encode("utf-8")

    request = urllib.request.Request(
        OLLAMA_URL,
        data=payload,
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            ollama = json.loads(response.read().decode("utf-8"))
    except Exception as exc:
        raise HTTPException(
            status_code=502,
            detail=f"Ollama indisponible: {exc}",
        ) from exc

    print(json.dumps({"event": "llm_completed",
                      "llm_ms": round((time.perf_counter() - llm_started) * 1000, 1),
                      "model": OLLAMA_MODEL}, ensure_ascii=False))

    try:
        result = json.loads(ollama.get("response", "{}"))
    except json.JSONDecodeError:
        result = {}

    command = str(result.get("command", "none")).strip().lower()
    reply = str(result.get("reply", "")).strip()

    if command not in VALID_COMMANDS and command != "none":
        command = "none"

    if not reply:
        reply = "Je n'ai pas compris."

    if command in VALID_COMMANDS:
        sequence = set_command(command)
    else:
        current = current_state()
        sequence = current["sequence"]

    return {
        "ok": True,
        "heard": message,
        "reply": reply,
        "command": command,
        "sequence": sequence,
        "route": "ollama",
        "conversation_backend": backend,
        "speak": True,
    }


def stream_message_events(message: str):
    """Yield newline-delimited local events while a Codex reply is generated.

    Direct commands remain deterministic and instantaneous. Ollama keeps its
    existing non-streaming fallback until its own streaming JSON protocol is
    implemented.
    """
    message = normalize_kage_name(message.strip())
    if not message:
        raise HTTPException(status_code=400, detail="Message vide")

    direct = route_direct_command(message)
    if direct is not None:
        yield {"type": "done", "result": direct}
        return

    home_control = route_home_control(message)
    if home_control is not None:
        if home_control.get("reply"):
            yield {"type": "delta", "text": home_control["reply"]}
        yield {"type": "done", "result": home_control}
        return

    backend = get_conversation_backend()
    explicit_web_request = needs_web_search(message)
    web_context = None
    if backend != "codex" and explicit_web_request:
        web_data = search_web(message)
        web_context = format_web_context(web_data)
        print(json.dumps({
            "event": "web_search",
            "results": len(web_data.get("results", [])),
            "available": not bool(web_data.get("error")),
        }, ensure_ascii=False))

    language = get_response_language()
    if backend != "codex":
        result = process_message(message)
        if result.get("reply"):
            yield {"type": "delta", "text": result["reply"]}
        yield {"type": "done", "result": result}
        return

    codex_message, web_context, explicit_web_request, enrich_after = prepare_codex_web_request(message)
    events: queue.Queue[dict] = queue.Queue()

    def generate() -> None:
        enrichment_started = False

        def stream_delta(text: str) -> None:
            nonlocal enrichment_started
            if enrich_after and not enrichment_started:
                enrichment_started = True
                start_background_research(message)
            events.put({"type": "delta", "text": text})

        try:
            codex = codex_bridge.ask(
                codex_message,
                web_context=web_context,
                response_language=language,
                web_search_requested=explicit_web_request,
                on_delta=stream_delta,
                timeout=75,
            )
            if ((enrich_after or codex.get("web_search_calls", 0) > 0)
                    and not enrichment_started):
                start_background_research(message)
            reply = codex["reply"] or "I could not form a response."
            current = current_state()
            result = {
                "ok": True,
                "heard": message,
                "reply": reply,
                "command": "none",
                "sequence": current["sequence"],
                "route": "codex",
                "conversation_backend": backend,
                "speak": True,
            }
            print(json.dumps({
                "event": "codex_stream_completed",
                "first_delta_ms": codex["first_delta_ms"],
                "total_ms": codex["total_ms"],
                "model": codex["model"],
                "web_search_calls": codex.get("web_search_calls", 0),
                "web_search_mode": codex.get("web_search_mode"),
                "web_pipeline": WEB_PIPELINE_VERSION,
            }, ensure_ascii=False))
            events.put({"type": "done", "result": result})
        except CodexBridgeError as exc:
            events.put({"type": "error", "detail": str(exc)})
        except Exception as exc:
            events.put({"type": "error", "detail": f"Streaming failed: {exc}"})

    threading.Thread(target=generate, name="kage-codex-stream", daemon=True).start()
    yield {"type": "meta", "route": "codex", "conversation_backend": backend}
    while True:
        event = events.get()
        yield event
        if event["type"] in {"done", "error"}:
            return


def transcribe_pcm(pcm: bytes) -> str:
    if not pcm:
        return ""

    if len(pcm) > MAX_AUDIO_BYTES:
        raise HTTPException(status_code=413, detail="Audio trop long")

    if len(pcm) % AUDIO_SAMPLE_WIDTH:
        raise HTTPException(status_code=400, detail="PCM 16-bit invalide")

    fd, wav_path = tempfile.mkstemp(prefix="kage_", suffix=".wav")
    os.close(fd)

    try:
        with wave.open(wav_path, "wb") as wav:
            wav.setnchannels(AUDIO_CHANNELS)
            wav.setsampwidth(AUDIO_SAMPLE_WIDTH)
            wav.setframerate(AUDIO_SAMPLE_RATE)
            wav.writeframes(pcm)

        segments, _ = whisper_model.transcribe(
            wav_path,
            language="fr",
            task="transcribe",
            vad_filter=True,
            beam_size=5,
            condition_on_previous_text=False,
            initial_prompt=(
                "Ceci est une conversation en français. "
                "Le robot s'appelle Kagé ; conserve correctement son nom. "
                "L'utilisateur parle français."
            ),
        )

        text = " ".join(
            segment.text.strip()
            for segment in segments
            if segment.text.strip()
        ).strip()

        return normalize_kage_name(text)
    finally:
        try:
            os.remove(wav_path)
        except OSError:
            pass


def choose_french_voice(engine) -> None:
    for voice in engine.getProperty("voices"):
        info = " ".join(
            [
                str(getattr(voice, "name", "")),
                str(getattr(voice, "id", "")),
                str(getattr(voice, "languages", "")),
            ]
        ).lower()

        if (
            "french" in info
            or "fr-fr" in info
            or "fr_fr" in info
            or "fr-be" in info
            or "hortense" in info
            or "denise" in info
        ):
            engine.setProperty("voice", voice.id)
            return


def speak_reply(text: str) -> None:
    if not TTS_ENABLED or not text or pyttsx3 is None:
        return

    com_initialized = False
    try:
        if pythoncom is not None:
            pythoncom.CoInitialize()
            com_initialized = True

        engine = pyttsx3.init()
        choose_french_voice(engine)
        engine.setProperty("rate", 185)
        engine.setProperty("volume", 1.0)
        engine.say(text)
        engine.runAndWait()
        engine.stop()
    except Exception as exc:
        print(f"TTS warning: {exc}")
    finally:
        if com_initialized:
            pythoncom.CoUninitialize()


def process_audio(pcm: bytes) -> dict:
    request_id = uuid.uuid4().hex[:12]
    started = time.perf_counter()
    print(json.dumps({"event": "request_started", "request_id": request_id,
                      "ts": datetime.now(timezone.utc).isoformat()}, ensure_ascii=False))
    stt_started = time.perf_counter()
    text = transcribe_pcm(pcm)
    stt_ms = round((time.perf_counter() - stt_started) * 1000, 1)

    if not text:
        result = {
            "ok": False,
            "heard": "",
            "reply": "Je n'ai rien compris.",
            "command": "none",
            "sequence": current_state()["sequence"],
        }
        print(json.dumps({"event": "request_completed", "request_id": request_id,
                          "stt_ms": stt_ms,
                          "total_ms": round((time.perf_counter() - started) * 1000, 1),
                          "empty": True,
                          "pcm_bytes": len(pcm),
                          "audio_seconds": round(len(pcm) / (AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_WIDTH), 3)}, ensure_ascii=False))
        return result

    print(f"Entendu: {text}")
    route_started = time.perf_counter()
    result = process_message(text)
    route_ms = round((time.perf_counter() - route_started) * 1000, 1)
    print(f"Kage: {result['reply']} | commande={result['command']}")

    # This is deliberately synchronous. The ESP32 waits for /audio to finish,
    # so it is not listening while the nearby M920q speaker speaks the reply.
    if result.get("speak", True):
        speak_reply(result["reply"])
    print(json.dumps({"event": "request_completed", "request_id": request_id,
                      "stt_ms": stt_ms, "route_ms": route_ms,
                      "total_ms": round((time.perf_counter() - started) * 1000, 1),
                      "pcm_bytes": len(pcm),
                      "audio_seconds": round(len(pcm) / (AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_WIDTH), 3),
                      "heard": text, "command": result["command"]}, ensure_ascii=False))
    return result


@app.get("/control", response_class=HTMLResponse)
def control_page():
    return HTMLResponse(CONTROL_PAGE_PATH.read_text(encoding="utf-8"))


@app.get("/control/manifest.webmanifest")
def control_manifest():
    return JSONResponse({
        "name": "Kage Control",
        "short_name": "Kage",
        "start_url": "/control",
        "scope": "/",
        "display": "standalone",
        "background_color": "#101216",
        "theme_color": "#101216",
        "orientation": "portrait",
    }, media_type="application/manifest+json")


@app.post("/control/login")
async def control_login(request: Request):
    provided = (await request.body()).decode("utf-8", errors="ignore").strip()
    if not KAGE_API_KEY or not hmac.compare_digest(provided, KAGE_API_KEY):
        raise HTTPException(status_code=401, detail="Clé Kage invalide")
    token = secrets.token_urlsafe(32)
    with control_session_lock:
        control_sessions[token] = time.monotonic() + CONTROL_SESSION_TTL_SECONDS
    response = JSONResponse({"ok": True})
    response.set_cookie(
        "kage-control", token, max_age=CONTROL_SESSION_TTL_SECONDS,
        httponly=True, samesite="strict", path="/",
    )
    return response


@app.get("/status")
def get_status(request: Request):
    require_kage_key(request)
    current = current_state()
    return {
        "server": "Kage",
        "online": True,
        "command": current["command"],
        "sequence": current["sequence"],
        "assistant_state": current["assistant_state"],
        "assistant_sequence": current["assistant_sequence"],
        "conversation_backend": get_conversation_backend(),
        "voice_active": current["voice_active"],
        "codex_model": os.getenv("KAGE_CODEX_MODEL", "gpt-5.6-luna"),
        "web_pipeline": WEB_PIPELINE_VERSION,
        "codex_web_search": os.getenv("KAGE_CODEX_WEB_SEARCH", "live"),
        "codex_web_context": os.getenv("KAGE_CODEX_WEB_CONTEXT", "low"),
        "background_research": research_status(),
    }


@app.post("/command/{command}")
def send_command(command: str, request: Request):
    require_kage_key(request)
    command = command.lower().strip()
    if command not in VALID_COMMANDS:
        raise HTTPException(
            status_code=400,
            detail=f"Commande invalide: {command}",
        )

    sequence = set_command(command)
    return {
        "ok": True,
        "command": command,
        "sequence": sequence,
    }


@app.post("/control/drive")
def control_drive(body: DriveControlRequest, request: Request):
    """Persistent differential drive input from the landscape phone UI."""
    require_kage_key(request)
    if not -100 <= body.left <= 100 or not -100 <= body.right <= 100:
        raise HTTPException(status_code=400, detail="Vitesse hors limites")
    command = f"drive:{body.left}:{body.right}"
    return {"ok": True, "command": command, "sequence": set_command(command)}


@app.post("/control/servos")
def control_servos(body: ServoControlRequest, request: Request):
    """Absolute, cable-safe virtual positions. The ESP32 smooths the motion."""
    require_kage_key(request)
    if not 0 <= body.pan <= 100 or not 0 <= body.tilt <= 100:
        raise HTTPException(status_code=400, detail="Position hors limites")
    command = f"servo:{body.pan}:{body.tilt}"
    return {"ok": True, "command": command, "sequence": set_command(command)}


@app.post("/assistant-state/{assistant_state}")
def update_assistant_state(assistant_state: str, request: Request):
    """Publish a transient PC voice state for the Waveshare animation."""
    require_kage_key(request)
    assistant_state = assistant_state.lower().strip()
    if assistant_state not in VALID_ASSISTANT_STATES:
        raise HTTPException(status_code=400, detail="Assistant state invalid")
    sequence = set_assistant_state(assistant_state)
    print(json.dumps({"event": "assistant_state", "state": assistant_state,
                      "sequence": sequence}, ensure_ascii=False))
    return {"ok": True, "assistant_state": assistant_state, "assistant_sequence": sequence}


@app.post("/voice/toggle")
def toggle_voice_from_waveshare(request: Request):
    """Three taps on Kage's face start direct listening, or stop Voice."""
    require_kage_key(request)
    if _voice_process_count() or current_state()["voice_active"]:
        closed = _close_voice_processes()
        play_sound("triple_close")
        action = "stopped"
    else:
        _launch_voice_direct()
        play_sound("triple_open")
        action = "started"
    sequence = set_assistant_state("idle")
    print(json.dumps({"event": "voice_toggle", "action": action,
                      "assistant_sequence": sequence}, ensure_ascii=False))
    return {"ok": True, "action": action, "assistant_sequence": sequence}


def _launch_voice_direct() -> bool:
    """Open a visible console and return promptly, even while models load."""
    with voice_control_lock:
        if current_state()["voice_active"]:
            return False
        stale_count = _voice_process_count()
        if stale_count:
            # A previous console may have survived a crash or a duplicate
            # launch. Clean every old voice process before starting one fresh.
            cleanup = ("$voice = @(Get-CimInstance Win32_Process | Where-Object { "
                       "$_.Name -eq 'python.exe' -and $_.CommandLine -and "
                       "$_.CommandLine -like '*Kage*voice.py*' "
                       "}); $voice | ForEach-Object { Stop-Process -Id $_.ProcessId -Force "
                       "-ErrorAction SilentlyContinue }")
            subprocess.run(["powershell.exe", "-NoProfile", "-Command", cleanup],
                           capture_output=True, text=True, timeout=8, check=True)
        script = ("$host.UI.RawUI.WindowTitle='Kage Voice'; "
                  "$env:KAGE_START_LISTENING='1'; "
                  "$env:KAGE_SILENCE_AFTER_SPEECH='2.0'; "
                  "$site='C:\\Kage\\venv\\Lib\\site-packages'; "
                  "$env:PYTHONPATH=\"$site;$site\\win32;$site\\win32\\lib;$site\\pywin32_system32;C:\\Kage\"; "
                  "$env:PATH=\"$site\\pywin32_system32;$env:PATH\"; "
                  "& 'C:\\Users\\Oussama\\AppData\\Local\\Programs\\Python\\Python312\\python.exe' 'C:\\Kage\\voice.py'")
        subprocess.Popen([
            "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-Command", script,
        ], creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
        return True


def _voice_process_count() -> int:
    script = ("@(Get-CimInstance Win32_Process | Where-Object { "
              "$_.Name -eq 'python.exe' -and $_.CommandLine -and "
              "$_.CommandLine -like '*Kage*voice.py*' "
              "}).Count")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-Command", script],
                            capture_output=True, text=True, timeout=5, check=True)
    return int(result.stdout.strip().splitlines()[-1])


def _close_voice_processes() -> int:
    with voice_control_lock:
        VOICE_HOLD_ACTIVE_FLAG.unlink(missing_ok=True)
        script = ("$voice = @(Get-CimInstance Win32_Process | Where-Object { "
                  "$_.Name -eq 'python.exe' -and $_.CommandLine -and "
                  "$_.CommandLine -like '*Kage*voice.py*' "
                  "}); $voice | ForEach-Object { Stop-Process -Id $_.ProcessId -Force "
                  "-ErrorAction SilentlyContinue }; $voice.Count")
        result = subprocess.run(["powershell.exe", "-NoProfile", "-Command", script],
                                capture_output=True, text=True, timeout=8, check=True)
        closed = int(result.stdout.strip().splitlines()[-1])
        set_voice_active(False)
        set_assistant_state("idle")
        return closed


@app.post("/voice/start-direct")
def start_voice_direct(request: Request):
    require_kage_key(request)
    current = current_state()
    if not current["voice_active"]:
        started = _launch_voice_direct()
        if started:
            play_sound("triple_open")
        return {"ok": True, "action": "started" if started else "starting"}
    if current["assistant_state"] == "idle":
        wake_voice_from_waveshare(request)
        action = "wake"
    elif current["assistant_state"] == "listening":
        sleep_voice_from_waveshare(request)
        action = "sleep"
    else:
        interrupt_voice_from_waveshare(request)
        action = "interrupt"
    return {"ok": True, "action": action}


@app.post("/voice/hold/start")
def start_voice_hold(request: Request):
    require_kage_key(request)
    VOICE_HOLD_ACTIVE_FLAG.write_text(str(time.time()), encoding="ascii")
    current = current_state()
    if not current["voice_active"]:
        started = _launch_voice_direct()
        if started:
            play_sound("triple_open")
    elif current["assistant_state"] == "idle":
        play_sound("wake_listening")
        started = False
    elif current["assistant_state"] in {"thinking", "speaking"}:
        interrupt_voice_from_waveshare(request)
        started = False
    else:
        started = False
    return {"ok": True, "hold_active": True, "started": started}


@app.post("/voice/hold/stop")
def stop_voice_hold(request: Request):
    require_kage_key(request)
    VOICE_HOLD_ACTIVE_FLAG.unlink(missing_ok=True)
    return {"ok": True, "hold_active": False}


@app.post("/voice/text-mode")
def enable_voice_text_mode(request: Request):
    """Show the desktop text box; submitted text follows the normal voice path."""
    require_kage_key(request)
    VOICE_TEXT_MODE_FLAG.write_text(str(time.time()), encoding="ascii")
    return {"ok": True, "text_mode": True}


@app.post("/voice/text-mode/exit")
def disable_voice_text_mode(request: Request):
    require_kage_key(request)
    VOICE_TEXT_MODE_FLAG.unlink(missing_ok=True)
    return {"ok": True, "text_mode": False}


@app.post("/voice/close-all")
def close_all_voice(request: Request):
    require_kage_key(request)
    try:
        closed = _close_voice_processes()
    except Exception as exc:
        raise HTTPException(status_code=500, detail="Voice close failed") from exc
    if closed:
        play_sound("triple_close")
    sequence = set_assistant_state("idle")
    return {"ok": True, "closed": closed, "voice_active": False, "assistant_sequence": sequence}


@app.post("/voice/interrupt")
def interrupt_voice_from_waveshare(request: Request):
    require_kage_key(request)
    assistant_state = current_state()["assistant_state"]
    play_sound("cancel_thinking" if assistant_state == "thinking" else "stop_speaking")
    try:
        VOICE_INTERRUPT_FLAG.write_text(str(time.time()), encoding="ascii")
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Voice interrupt failed") from exc
    # An interrupted reply exits the conversation and waits for the wake word.
    sequence = set_assistant_state("idle")
    return {"ok": True, "assistant_sequence": sequence}


@app.post("/voice/wake")
def wake_voice_from_waveshare(request: Request):
    require_kage_key(request)
    play_sound("wake_listening")
    try:
        VOICE_WAKE_FLAG.write_text(str(time.time()), encoding="ascii")
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Voice wake failed") from exc
    return {"ok": True}


@app.post("/voice/sleep")
def sleep_voice_from_waveshare(request: Request):
    require_kage_key(request)
    play_sound("sleep_listening")
    try:
        VOICE_SLEEP_FLAG.write_text(str(time.time()), encoding="ascii")
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Voice sleep failed") from exc
    sequence = set_assistant_state("idle")
    set_command("sleep")
    return {"ok": True, "assistant_sequence": sequence}


@app.post("/voice-session/{active}")
def update_voice_session(active: bool, request: Request):
    """Voice.py publishes this once after its model is ready, then clears it on exit."""
    require_kage_key(request)
    value = set_voice_active(active)
    print(json.dumps({"event": "voice_session", "active": value}, ensure_ascii=False))
    return {"ok": True, "voice_active": value}


@app.get("/command/latest")
def latest_command(request: Request):
    require_kage_key(request)
    return current_state()


@app.post("/ask")
def ask(body: AskRequest, request: Request):
    require_kage_key(request)
    return process_message(body.message)


@app.post("/ask/stream")
def ask_stream(body: AskRequest, request: Request):
    """Stream local JSON-line events for progressive PC speech playback."""
    require_kage_key(request)

    def event_stream():
        for event in stream_message_events(body.message):
            yield json.dumps(event, ensure_ascii=False) + "\n"

    return StreamingResponse(
        event_stream(),
        media_type="application/x-ndjson",
        headers={"Cache-Control": "no-cache", "X-Kage-Stream": "text-delta-v1"},
    )


@app.post("/speech")
async def speech(body: SpeechRequest, request: Request):
    require_kage_key(request)
    text = body.text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="Texte vide")
    if not kokoro_service.available:
        raise HTTPException(status_code=503, detail="Kokoro TTS indisponible")

    async def audio_stream():
        try:
            async for chunk in kokoro_service.stream_pcm(text, body.voice):
                yield chunk
        except Exception as exc:
            print(f"TTS streaming warning: {exc}")

    return StreamingResponse(
        audio_stream(),
        media_type="application/octet-stream",
        headers={
            "X-Kage-Audio-Format": "s16le-mono",
            "X-Kage-Sample-Rate": "16000",
            "X-Kage-Voice": body.voice,
        },
    )


@app.post("/audio")
async def audio(request: Request):
    require_kage_key(request)
    if request.headers.get("content-type", "").split(";")[0].strip().lower() != "application/octet-stream":
        raise HTTPException(
            status_code=415,
            detail="Utilise Content-Type: application/octet-stream",
        )

    pcm = await request.body()
    return await run_in_threadpool(process_audio, pcm)
