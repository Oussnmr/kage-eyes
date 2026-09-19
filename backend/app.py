from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from faster_whisper import WhisperModel

import json
import os
import re
import tempfile
import threading
import urllib.request
import wave

try:
    import pyttsx3
except ImportError:
    pyttsx3 = None

try:
    import pythoncom
except ImportError:
    pythoncom = None


app = FastAPI(title="Kage M920q Backend")

VALID_COMMANDS = {"idle", "blink", "sleep", "angry", "dizzy"}
OLLAMA_URL = "http://127.0.0.1:11434/api/generate"
OLLAMA_MODEL = "qwen3:4b-instruct-2507-q4_K_M"

AUDIO_SAMPLE_RATE = 16000
AUDIO_CHANNELS = 1
AUDIO_SAMPLE_WIDTH = 2
MAX_AUDIO_BYTES = AUDIO_SAMPLE_RATE * AUDIO_SAMPLE_WIDTH * 12
TTS_ENABLED = True

state_lock = threading.Lock()
state = {
    "command": "idle",
    "sequence": 0,
}

print("Chargement de Whisper small...")
whisper_model = WhisperModel(
    "small",
    device="cpu",
    compute_type="int8",
)
print("Whisper small prêt.")


class AskRequest(BaseModel):
    message: str


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


def process_message(message: str) -> dict:
    message = normalize_kage_name(message.strip())
    if not message:
        raise HTTPException(status_code=400, detail="Message vide")

    prompt = f"""
Tu es l'assistant d'un petit robot de bureau nommé Kage.
Tu réponds en français, de façon courte et naturelle.

Tu peux demander UNE réaction physique parmi:
idle, blink, sleep, angry, dizzy, none.

Choisis une réaction seulement si elle est pertinente à la demande.
Ne prétends jamais avoir exécuté une action qui n'existe pas.

Phrase de l'utilisateur:
{message}

Réponds uniquement avec un objet JSON exactement sous cette forme:
{{"command":"none","reply":"ta réponse en français"}}
""".strip()

    payload = json.dumps(
        {
            "model": OLLAMA_MODEL,
            "prompt": prompt,
            "stream": False,
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
    }


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
                "Conversation en français. "
                "Le robot s'appelle Kage, prononcé cagée. "
                "L'utilisateur parle en français."
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
    text = transcribe_pcm(pcm)

    if not text:
        return {
            "ok": False,
            "heard": "",
            "reply": "Je n'ai rien compris.",
            "command": "none",
            "sequence": current_state()["sequence"],
        }

    print(f"Entendu: {text}")
    result = process_message(text)
    print(f"Kage: {result['reply']} | commande={result['command']}")

    # This is deliberately synchronous. The ESP32 waits for /audio to finish,
    # so it is not listening while the nearby M920q speaker speaks the reply.
    speak_reply(result["reply"])
    return result


@app.get("/status")
def get_status():
    current = current_state()
    return {
        "server": "Kage",
        "online": True,
        "command": current["command"],
        "sequence": current["sequence"],
    }


@app.post("/command/{command}")
def send_command(command: str):
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
def latest_command():
    return current_state()


@app.post("/ask")
def ask(body: AskRequest):
    return process_message(body.message)


@app.post("/audio")
async def audio(request: Request):
    if request.headers.get("content-type", "").split(";")[0].strip().lower() != "application/octet-stream":
        raise HTTPException(
            status_code=415,
            detail="Utilise Content-Type: application/octet-stream",
        )

    pcm = await request.body()
    return await run_in_threadpool(process_audio, pcm)
