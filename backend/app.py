from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from tts_service import kokoro_service
from codex_bridge import CodexBridgeError, codex_bridge
from web_search import format_web_context, needs_web_search, search_web

from faster_whisper import WhisperModel

import hmac
import json
import os
import queue
import re
import tempfile
import threading
import urllib.request
import wave
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

try:
    import pyttsx3
except ImportError:
    pyttsx3 = None

try:
    import pythoncom
except ImportError:
    pythoncom = None


app = FastAPI(title="Kage M920q Backend", docs_url=None, redoc_url=None, openapi_url=None)

VALID_COMMANDS = {"idle", "blink", "sleep", "angry", "dizzy"}
OLLAMA_URL = "http://127.0.0.1:11434/api/generate"
OLLAMA_MODEL = "qwen3:4b-instruct-2507-q4_K_M"
DEFAULT_CONVERSATION_BACKEND = os.getenv("KAGE_CONVERSATION_BACKEND", "ollama").strip().lower()
SETTINGS_PATH = Path(os.getenv("KAGE_SETTINGS_PATH", r"C:\Kage\kage_settings.json"))
KAGE_API_KEY = os.getenv("KAGE_API_KEY", "").strip()

AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_SAMPLE_WIDTH = 2
MAX_AUDIO_BYTES = AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_WIDTH * 12
TTS_ENABLED = os.getenv("KAGE_PC_TTS", "0").strip().lower() in {"1", "true", "yes", "on"}
WHISPER_MODEL_NAME = os.getenv("KAGE_WHISPER_MODEL", "small").strip() or "small"

if not KAGE_API_KEY:
    print("ATTENTION: KAGE_API_KEY absent. Les endpoints protégés refuseront les requêtes.")

state_lock = threading.Lock()
state = {
    "command": "idle",
    "sequence": 0,
}
backend_lock = threading.Lock()


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


def get_conversation_backend() -> str:
    with backend_lock:
        return conversation_backend


def set_conversation_backend(backend: str) -> str:
    if backend not in {"ollama", "codex"}:
        raise ValueError(f"Unsupported conversation backend: {backend}")
    with backend_lock:
        global conversation_backend
        SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
        temporary_path = SETTINGS_PATH.with_suffix(".tmp")
        temporary_path.write_text(
            json.dumps({"conversation_backend": backend}, indent=2) + "\n",
            encoding="utf-8",
        )
        temporary_path.replace(SETTINGS_PATH)
        conversation_backend = backend
        return conversation_backend

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
    voice: str = "am_adam"


def require_kage_key(request: Request) -> None:
    if not KAGE_API_KEY:
        raise HTTPException(
            status_code=503,
            detail="KAGE_API_KEY n'est pas configurée sur le M920q",
        )

    provided = request.headers.get("x-kage-key", "")
    if not provided or not hmac.compare_digest(provided, KAGE_API_KEY):
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


def current_state() -> dict:
    with state_lock:
        return dict(state)


def route_direct_command(message: str):
    """Return a local command result for unambiguous, supported intents."""
    normalized = re.sub(r"[^a-zA-ZÀ-ÿ0-9 ]", " ", message.lower())
    normalized = re.sub(r"\s+", " ", normalized).strip()

    local_backend_phrases = (
        "switch to local", "use local", "go local", "local mode", "run locally",
        "use ollama", "switch to ollama", "go back to local", "stay local",
        "pass in local mode", "passe en local",
    )
    codex_backend_phrases = (
        "switch to chatgpt", "switch to codex", "use chatgpt", "use codex",
        "go back to chatgpt", "back to chatgpt", "use the cloud", "cloud mode",
        "use the chatgpt backend", "repasse sur chatgpt",
    )
    if any(normalized == phrase or normalized.endswith(" " + phrase)
           for phrase in local_backend_phrases):
        backend = set_conversation_backend("ollama")
        print(json.dumps({"event": "backend_switched", "backend": backend}, ensure_ascii=False))
        current = current_state()
        return {
            "ok": True, "heard": normalize_kage_name(message),
            "reply": "Okay. I will use local mode.", "command": "none",
            "sequence": current["sequence"], "route": "direct", "speak": True,
            "conversation_backend": backend,
        }
    if any(normalized == phrase or normalized.endswith(" " + phrase)
           for phrase in codex_backend_phrases):
        backend = set_conversation_backend("codex")
        print(json.dumps({"event": "backend_switched", "backend": backend}, ensure_ascii=False))
        current = current_state()
        return {
            "ok": True, "heard": normalize_kage_name(message),
            "reply": "Okay. I will use ChatGPT.", "command": "none",
            "sequence": current["sequence"], "route": "direct", "speak": True,
            "conversation_backend": backend,
        }

    english_rules = (
        ("idle", ("stop", "be normal", "return to normal", "go back to normal", "calm down",
                   "relax", "reset", "reset yourself", "return to idle", "go idle", "normal mode",
                   "back to idle", "come back to normal", "return back to normal", "stop here",
                   "stop listening", "stop talking", "stop the conversation", "end the conversation",
                   "end chat", "end this chat", "cancel chat", "be quiet", "quiet", "go quiet",
                   "enough", "that's enough", "that is enough", "that's enough for now",
                   "no more", "stop now", "you can stop here", "you can stop now", "please stop",
                   "let's stop here", "let us stop here", "we can stop here"),
         "Okay, I am back to normal."),
        ("blink", ("blink", "blink your eyes", "make your eyes blink", "close and open your eyes",
                    "blink twice", "blink two times", "give me a blink"), "Sure."),
        ("sleep", ("go to sleep", "sleep", "enter sleep mode", "take a nap", "rest", "sleep now",
                    "go into sleep mode", "take a rest", "you can sleep", "sleep for now"), "I am going to sleep."),
        ("angry", ("be angry", "get angry", "act angry", "look angry", "show me angry", "angry mode",
                    "make yourself angry", "become angry", "turn angry", "show an angry face"), "Okay."),
        ("dizzy", ("spin", "spin around", "get dizzy", "act dizzy", "look dizzy", "dizzy mode",
                    "make yourself dizzy", "become dizzy", "turn dizzy", "start spinning"), "Oops."),
    )
    for command, phrases, reply in english_rules:
        if any(normalized == phrase or normalized.endswith(" " + phrase) for phrase in phrases):
            sequence = set_command(command)
            print(json.dumps({"event": "direct_route", "route": command, "latency_ms": 0}, ensure_ascii=False))
            return {"ok": True, "heard": normalize_kage_name(message), "reply": reply,
                    "command": command, "sequence": sequence, "route": "direct", "speak": False}
    rules = (
        ("idle", ("stop", "arrête", "arrete", "tais toi", "au repos",
                   "reviens à la normale", "revient à la normale", "reviens a la normale",
                   "revient a la normale", "revient la normale", "retourne à la normale",
                   "retourne a la normale", "redeviens normal", "redeviens normale"),
         "D'accord, je reviens à la normale."),
        ("blink", ("cligne", "clignote"), "Voilà."),
        ("sleep", ("endors toi", "mets toi en veille", "mise en veille"), "Je passe en veille."),
        ("angry", ("sois en colère", "sois en colere", "en colère", "en colere"), "Très bien."),
        ("dizzy", ("tourne sur toi même", "tourne sur toi meme", "étourdis", "etourdis"), "Oups."),
    )
    for command, phrases, reply in rules:
        if any(normalized == phrase or normalized.endswith(" " + phrase) for phrase in phrases):
            sequence = set_command(command)
            print(json.dumps({"event": "direct_route", "route": command,
                              "latency_ms": 0}, ensure_ascii=False))
            return {
                "ok": True,
                "heard": normalize_kage_name(message),
                "reply": "" if command == "idle" else reply,
                "command": command,
                "sequence": sequence,
                "route": "direct",
                "speak": False,
            }
    if "colere" in normalized or "colère" in normalized:
        if any(word in normalized for word in ("met", "mets", "mettre", "sois")):
            sequence = set_command("angry")
            print(json.dumps({"event": "direct_route", "route": "angry",
                              "latency_ms": 0}, ensure_ascii=False))
            return {
                "ok": True,
                "heard": normalize_kage_name(message),
                "reply": "Très bien.",
                "command": "angry",
                "sequence": sequence,
                "route": "direct",
                "speak": False,
            }
    if "angry" in normalized and any(word in normalized for word in ("can", "please", "make", "become", "be")):
        sequence = set_command("angry")
        print(json.dumps({"event": "direct_route", "route": "angry", "latency_ms": 0}, ensure_ascii=False))
        return {
            "ok": True,
            "heard": normalize_kage_name(message),
            "reply": "Okay.",
            "command": "angry",
            "sequence": sequence,
            "route": "direct",
            "speak": False,
        }
    return None


def process_message(message: str) -> dict:
    llm_started = time.perf_counter()
    message = normalize_kage_name(message.strip())
    if not message:
        raise HTTPException(status_code=400, detail="Message vide")

    direct = route_direct_command(message)
    if direct is not None:
        return direct

    web_context = None
    if needs_web_search(message):
        web_data = search_web(message)
        web_context = format_web_context(web_data)
        print(json.dumps({
            "event": "web_search",
            "results": len(web_data.get("results", [])),
            "available": not bool(web_data.get("error")),
        }, ensure_ascii=False))

    backend = get_conversation_backend()
    if backend == "codex":
        try:
            codex = codex_bridge.ask(message, web_context=web_context, timeout=60)
            reply = codex["reply"] or "I could not form a response."
            current = current_state()
            print(json.dumps({
                "event": "codex_completed",
                "first_delta_ms": codex["first_delta_ms"],
                "total_ms": codex["total_ms"],
                "model": codex["model"],
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

    prompt = f"""
    You are the assistant of a small desktop robot named Kage.
    You answer in English, briefly and naturally.

Tu peux demander UNE réaction physique parmi:
idle, blink, sleep, angry, dizzy, none.

    Choose a reaction only when it is relevant to the request.
    Never claim to have executed an action that does not exist.

    User request:
{message}

    {web_context or "No web search was requested. Do not invent current facts."}

    Reply only with a JSON object in exactly this form:
    {{"command":"none","reply":"your short answer in English"}}
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

    web_context = None
    if needs_web_search(message):
        web_data = search_web(message)
        web_context = format_web_context(web_data)
        print(json.dumps({
            "event": "web_search",
            "results": len(web_data.get("results", [])),
            "available": not bool(web_data.get("error")),
        }, ensure_ascii=False))

    backend = get_conversation_backend()
    if backend != "codex":
        result = process_message(message)
        if result.get("reply"):
            yield {"type": "delta", "text": result["reply"]}
        yield {"type": "done", "result": result}
        return

    events: queue.Queue[dict] = queue.Queue()

    def generate() -> None:
        try:
            codex = codex_bridge.ask(
                message,
                web_context=web_context,
                on_delta=lambda text: events.put({"type": "delta", "text": text}),
                timeout=60,
            )
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
            language="en",
            task="transcribe",
            vad_filter=True,
            beam_size=5,
            condition_on_previous_text=False,
            initial_prompt=(
                "This is an English conversation. "
                "The robot is named Kage; keep the name pronounced Kage. "
                "The user speaks English."
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


@app.get("/status")
def get_status(request: Request):
    require_kage_key(request)
    current = current_state()
    return {
        "server": "Kage",
        "online": True,
        "command": current["command"],
        "sequence": current["sequence"],
        "conversation_backend": get_conversation_backend(),
        "codex_model": os.getenv("KAGE_CODEX_MODEL", "gpt-5.6-luna"),
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
