import sounddevice as sd
import numpy as np
import urllib.request
import json
import wave
import os
import sys
import collections
import queue
import pyttsx3
from concurrent.futures import ThreadPoolExecutor
import re
import random
import threading
import time
import hashlib
import atexit
import tkinter as tk
import msvcrt
from pocketsphinx import LiveSpeech
from kage_sounds import play_sound, start_loop, stop_loop
from stt_engine import KageSTT
from command_intents import is_direct, session_intent

# A detached PowerShell window can default to a legacy Windows code page.
# Keep status messages from terminating the assistant when they contain accents
# or visual state icons.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, OSError):
    pass

SAMPLE_RATE = 16000


def resolve_mic_device():
    configured = os.getenv("KAGE_MIC_DEVICE", "").strip()
    if configured:
        try:
            return int(configured)
        except ValueError:
            pass
    try:
        default_input = sd.default.device[0]
        if isinstance(default_input, int) and default_input >= 0:
            info = sd.query_devices(default_input)
            if info.get("max_input_channels", 0) > 0:
                return default_input
        candidates = [
            (index, info) for index, info in enumerate(sd.query_devices())
            if info.get("max_input_channels", 0) > 0
        ]
        # Prefer a real microphone over loopback/stereo-mix inputs.
        for index, info in candidates:
            name = str(info.get("name", "")).lower()
            if "micro" in name or "mikrofon" in name or "usb" in name:
                return index
        return candidates[0][0] if candidates else None
    except Exception:
        return None


MIC_DEVICE = resolve_mic_device()
WAV_FILE = r"C:\Kage\voice_temp.wav"
KAGE_API_KEY = os.getenv("KAGE_API_KEY", "").strip()
KAGE_TTS_VOICE = os.getenv("KAGE_TTS_VOICE", "ff_siwis").strip() or "ff_siwis"
SETTINGS_PATH = r"C:\Kage\kage_settings.json"
WAITING_AUDIO_DIR = r"C:\Kage\waiting_audio"
VOICE_INTERRUPT_FLAG = r"C:\Kage\voice_interrupt.flag"
VOICE_WAKE_FLAG = r"C:\Kage\voice_wake.flag"
VOICE_SLEEP_FLAG = r"C:\Kage\voice_sleep.flag"
VOICE_HOLD_ACTIVE_FLAG = r"C:\Kage\voice_hold_active.flag"
VOICE_TEXT_MODE_FLAG = r"C:\Kage\voice_text_mode.flag"

# Détection de voix
BLOCK_MS = 50
SILENCE_AFTER_SPEECH = float(os.getenv("KAGE_SILENCE_AFTER_SPEECH", "2.0"))
MIN_AUDIO_DURATION_MS = float(os.getenv("KAGE_MIN_AUDIO_DURATION_MS", "1200"))
MAX_RECORD_SECONDS = 12
PRE_ROLL_SECONDS = 0.30
WAKE_WORD_ENABLED = os.getenv("KAGE_WAKE_WORD", "1").strip().lower() in {"1", "true", "yes", "on"}
TEXT_MODE_ON_START = os.getenv("KAGE_TEXT_MODE", "0").strip().lower() in {"1", "true", "yes", "on"}
TEXT_MODE_HOTKEY = os.getenv("KAGE_TEXT_HOTKEY", "t").strip().lower()[:1] or "t"
WAKE_KEYPHRASE = "wake up"
START_LISTENING_ON_LAUNCH = os.getenv("KAGE_START_LISTENING", "0").strip().lower() in {"1", "true", "yes"}
WAKE_THRESHOLD = float(os.getenv("KAGE_WAKE_THRESHOLD", "1e-18"))
FOLLOW_UP_TIMEOUT_SECONDS = float(os.getenv("KAGE_FOLLOW_UP_TIMEOUT", "25"))
WAITING_REPLIES_ENABLED = os.getenv("KAGE_WAITING_REPLIES", "1").strip().lower() in {
    "1", "true", "yes", "on",
}
TEXT_INPUT_QUEUE = queue.Queue()
TEXT_WINDOW_STARTED = False


def _text_input_window():
    """Keep a small optional desktop text box available when requested."""
    global TEXT_WINDOW_STARTED
    if TEXT_WINDOW_STARTED:
        return
    TEXT_WINDOW_STARTED = True
    try:
        root = tk.Tk()
        root.title("Kagé — mode écriture")
        root.geometry("520x130")
        root.attributes("-topmost", True)
        tk.Label(root, text="Écris une phrase puis Entrée. Le bouton Audio revient au micro.").pack(pady=(10, 4))
        entry = tk.Entry(root, width=60)
        entry.pack(fill="x", padx=12)
        entry.focus_set()
        buttons = tk.Frame(root)
        buttons.pack(pady=8)
        def submit(_event=None):
            text = entry.get().strip()
            if text:
                TEXT_INPUT_QUEUE.put(text)
                entry.delete(0, tk.END)
        def audio():
            try:
                os.remove(VOICE_TEXT_MODE_FLAG)
            except FileNotFoundError:
                pass
            root.destroy()
        tk.Button(buttons, text="Envoyer", command=submit).pack(side="left", padx=5)
        tk.Button(buttons, text="Retour au micro", command=audio).pack(side="left", padx=5)
        entry.bind("<Return>", submit)
        root.protocol("WM_DELETE_WINDOW", audio)
        root.mainloop()
    except Exception as exc:
        print(json.dumps({"event": "text_window_unavailable", "error": str(exc)}, ensure_ascii=False))
    finally:
        TEXT_WINDOW_STARTED = False


def _start_text_window_if_requested():
    if os.path.exists(VOICE_TEXT_MODE_FLAG) and not TEXT_WINDOW_STARTED:
        threading.Thread(target=_text_input_window, name="kage-text-window", daemon=True).start()

print("Chargement du moteur STT...")
stt = KageSTT()
print(json.dumps({"event": "stt_config", **stt.describe()}, ensure_ascii=False))
print("Moteur STT prêt.")


# ---------- VOIX DE KAGE ----------

tts = pyttsx3.init()
tts.setProperty("rate", 185)
tts.setProperty("volume", 1.0)


def choose_french_voice():
    voices = tts.getProperty("voices")

    for voice in voices:
        info = (
            str(voice.name) + " "
            + str(voice.id) + " "
            + str(getattr(voice, "languages", ""))
        ).lower()

        if any(marker in info for marker in (
            "french", "fr-fr", "fr_fr", "france", "hortense", "denise",
        )):
            tts.setProperty("voice", voice.id)
            print(f"Voix Windows française de secours : {voice.name}")
            return True

    print("Aucune voix Windows française trouvée : secours Windows désactivé.")
    return False


WINDOWS_FRENCH_VOICE_AVAILABLE = choose_french_voice()

request_executor = ThreadPoolExecutor(max_workers=1)
state_executor = ThreadPoolExecutor(max_workers=1)
assistant_state_lock = threading.Lock()
last_assistant_state = None
mouth_publish_lock = threading.Lock()
mouth_request_lock = threading.Lock()
last_mouth_publish_at = 0.0


def publish_assistant_state(state):
    """Tell the local backend about a visual state without delaying audio."""
    global last_assistant_state
    with assistant_state_lock:
        if state == last_assistant_state:
            return
        last_assistant_state = state

    def publish():
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:8000/assistant-state/{state}",
                headers={"X-Kage-Key": KAGE_API_KEY},
                method="POST",
            )
            with urllib.request.urlopen(request, timeout=0.75):
                pass
        except Exception as exc:
            print(json.dumps({"event": "assistant_state_unavailable", "state": state,
                              "error": str(exc)}, ensure_ascii=False))

    state_executor.submit(publish)


def publish_mouth_level(level):
    """Publish a low-rate audio envelope; stale levels expire on the backend."""
    global last_mouth_publish_at
    now = time.monotonic()
    with mouth_publish_lock:
        if level and now - last_mouth_publish_at < 0.10:
            return
        last_mouth_publish_at = now
    if not mouth_request_lock.acquire(blocking=False):
        return

    def publish():
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:8000/voice/mouth/{int(level)}",
                headers={"X-Kage-Key": KAGE_API_KEY}, method="POST",
            )
            with urllib.request.urlopen(request, timeout=0.4):
                pass
        except Exception:
            pass
        finally:
            mouth_request_lock.release()

    state_executor.submit(publish)


def play_audio_with_mouth(samples):
    """Use the synthesized PCM itself for mouth motion, never a speaking timer."""
    samples = np.asarray(samples, dtype=np.int16)
    if samples.size == 0:
        return
    stop = threading.Event()

    def meter():
        started = time.perf_counter()
        for offset in range(0, samples.size, 1600):  # 100 ms at 16 kHz
            if stop.is_set():
                break
            chunk = samples[offset:offset + 1600].astype(np.float32) / 32768.0
            rms = float(np.sqrt(np.mean(chunk * chunk))) if chunk.size else 0.0
            level = int(max(0.0, min(1.0, (rms - 0.008) * 4.0)) * 1000)
            publish_mouth_level(level)
            deadline = started + (offset + chunk.size) / 16000.0
            stop.wait(max(0.0, deadline - time.perf_counter()))

    worker = threading.Thread(target=meter, name="kage-mouth-meter", daemon=True)
    try:
        sd.play(samples, samplerate=16000)
        worker.start()
        sd.wait()
    finally:
        stop.set()
        worker.join(timeout=0.15)
        publish_mouth_level(0)


def classify_behavior_cue(text):
    """Recognize only an unambiguous opening cue from the spoken answer."""
    first = re.split(r"(?<=[.!?])\s+", text.strip(), maxsplit=1)[0].lower()
    first = re.sub(r"^[^\wÀ-ÿ]+", "", first)
    if re.match(r"^(yes|oui|absolutely|exactly|correct|bien sûr|tout à fait)\b", first):
        return "affirm"
    if re.match(r"^(no|non|not quite|pas du tout|ce n'est pas|ce n’est pas)\b", first):
        return "deny"
    if re.match(r"^(let me think|i'm thinking|laisse[- ]moi réfléchir|je réfléchis)\b", first):
        return "thinking"
    if re.match(r"^(interesting|c'est intéressant|c’est intéressant|curieux)\b", first):
        return "curious"
    if re.match(r"^(great|bravo|félicitations|congratulations)\b", first):
        return "celebrate"
    if re.match(r"^(attention|careful|warning|be careful)\b", first):
        return "warning"
    if re.match(r"^(haha|ha ha|c'est drôle|c’est drôle|that's funny|that is funny)\b", first):
        return "amused"
    return None


def publish_behavior_cue(behavior):
    if not behavior:
        return
    def publish():
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:8000/behavior/{behavior}",
                headers={"X-Kage-Key": KAGE_API_KEY}, method="POST",
            )
            with urllib.request.urlopen(request, timeout=0.5):
                pass
        except Exception:
            pass
    state_executor.submit(publish)


def publish_voice_session(active, wait=False):
    """Publish persistent readiness once Whisper and audio are ready; no process polling."""
    def publish():
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:8000/voice-session/{str(bool(active)).lower()}",
                headers={"X-Kage-Key": KAGE_API_KEY}, method="POST",
            )
            with urllib.request.urlopen(request, timeout=0.75):
                pass
        except Exception as exc:
            print(json.dumps({"event": "voice_session_unavailable", "active": active,
                              "error": str(exc)}, ensure_ascii=False))
    if wait:
        publish()
    else:
        state_executor.submit(publish)


def start_voice_session_heartbeat():
    """Keep the UI readiness state accurate even if PowerShell is closed abruptly."""
    def heartbeat():
        while True:
            publish_voice_session(True)
            time.sleep(2.0)

    threading.Thread(target=heartbeat, name="kage-voice-heartbeat", daemon=True).start()


def consume_control_flag(path):
    try:
        if os.path.exists(path):
            os.remove(path)
            return True
    except OSError:
        pass
    return False


def consume_interrupt_request():
    return consume_control_flag(VOICE_INTERRUPT_FLAG)


def hold_is_active():
    return os.path.exists(VOICE_HOLD_ACTIVE_FLAG)


def reset_visual_state_on_exit():
    """Best-effort reset so closing the console cannot leave purple/green eyes."""
    try:
        publish_voice_session(False, wait=True)
        request = urllib.request.Request(
            "http://127.0.0.1:8000/assistant-state/idle",
            headers={"X-Kage-Key": KAGE_API_KEY},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=0.5):
            pass
    except Exception:
        pass


atexit.register(reset_visual_state_on_exit)
WAITING_REPLIES = (
    "Voyons voir...",
    "Donne-moi un instant...",
    "Un petit instant...",
    "Je vérifie...",
    "Laisse-moi réfléchir...",
    "Je regarde cela tout de suite...",
    "Je vérifie ça pour toi...",
    "Je traite ta demande...",
    "Bien sûr.",
    "Une seconde...",
)
WAITING_REPLIES_EN = (
    "Let me check...",
    "Give me a moment...",
    "One moment...",
    "I'm checking...",
    "Let me think...",
)

_waiting_audio = {}


def current_voice_settings():
    try:
        saved = json.loads(open(SETTINGS_PATH, "r", encoding="utf-8").read())
    except (OSError, json.JSONDecodeError):
        saved = {}
    language = saved.get("response_language", "en")
    if language not in {"fr", "en"}:
        language = "fr"
    expected_voice = "am_puck" if language == "en" else "ff_siwis"
    return language, saved.get("tts_voice", expected_voice) or expected_voice


def current_response_language():
    return current_voice_settings()[0]


def listening_announcement():
    return "Kagé is listening." if current_response_language() == "en" else "Kagé t'écoute."

def load_waiting_audio():
    _waiting_audio.clear()
    for phrase in WAITING_REPLIES:
        path = os.path.join(WAITING_AUDIO_DIR, f"{hashlib.sha1(phrase.encode()).hexdigest()}.wav")
        try:
            with wave.open(path, "rb") as wav:
                if wav.getnchannels() == 1 and wav.getsampwidth() == 2 and wav.getframerate() == 16000:
                    _waiting_audio[clean_speech_text(phrase)] = np.frombuffer(
                        wav.readframes(wav.getnframes()), dtype=np.int16
                    ).copy()
        except (OSError, EOFError):
            continue
SESSION_END_REPLIES = (
    "D'accord, je reste disponible.",
    "Je suis là si tu as besoin de moi.",
    "À tout à l'heure.",
    "Je me mets en veille. Rappelle-moi quand tu veux.",
)


def is_direct_command(text):
    return is_direct(text)


def is_end_session(text):
    return session_intent(text) is not None


def choose_waiting_reply(text):
    """Choose an opening only if the conversational reply takes at least 3 s."""
    if not WAITING_REPLIES_ENABLED:
        return None
    return random.choice(
        WAITING_REPLIES_EN if current_response_language() == "en" else WAITING_REPLIES
    )


def wait_for_wake_word(stop_event=None, announce=True, keyphrase=None):
    """Block locally until the selected local wake phrase is heard."""
    keyphrase = keyphrase or WAKE_KEYPHRASE
    if announce:
        print(f"\n🟣 Wake word active — say ‘{keyphrase}’.")
    listener = None
    try:
        listener = LiveSpeech(
            keyphrase=keyphrase,
            kws_threshold=WAKE_THRESHOLD,
            sampling_rate=SAMPLE_RATE,
            audio_device=MIC_DEVICE,
        )
        listener.ad.start()
        while stop_event is None or not stop_event.is_set():
            _start_text_window_if_requested()
            if msvcrt.kbhit():
                key = msvcrt.getwch().lower()
                if key == TEXT_MODE_HOTKEY:
                    try:
                        open(VOICE_TEXT_MODE_FLAG, "w", encoding="ascii").close()
                    except OSError:
                        pass
                    _start_text_window_if_requested()
                    print("⌨️ Fenêtre texte ouverte — Entrée envoie, Retour au micro la ferme.")
                    continue
            try:
                return ("text_input", TEXT_INPUT_QUEUE.get_nowait())
            except queue.Empty:
                pass
            if hold_is_active():
                print("🟣 Kage woke for push-to-talk")
                return "hold"
            if consume_control_flag(VOICE_WAKE_FLAG):
                print("🟣 Kage woke from touch")
                return "touch"
            audio, _ = listener.ad.read(listener.buffer_size // 2)
            speech = listener.ep.process(audio)
            if speech is None:
                continue
            if not listener.in_speech:
                listener.start_utt()
            listener.process_raw(speech)
            if listener.hyp():
                hypothesis = listener.hyp()
                listener.end_utt()
                hypothesis_text = getattr(hypothesis, "hypstr", str(hypothesis))
                best_score = getattr(hypothesis, "best_score", None)
                print(json.dumps({
                    "event": "wake_detection_candidate",
                    "keyphrase": keyphrase,
                    "hypothesis": hypothesis_text,
                    "best_score": best_score,
                }, ensure_ascii=False))
                print("🟣 Kage detected")
                return "wake_word"
        return False
    except sd.PortAudioError as exc:
        # A second sounddevice stream can briefly fail while the PC audio
        # device is switching between capture and playback. The listener is
        # optional during playback; never let its failure kill the voice loop.
        print(json.dumps({
            "event": "wake_listener_unavailable",
            "error": str(exc),
            "keyphrase": keyphrase,
        }, ensure_ascii=False))
        return False
    except Exception as exc:
        # PocketSphinx/PortAudio can surface device failures through a backend
        # exception type that is not exported as sounddevice.PortAudioError.
        # Barge-in is optional, so disable only that listener and keep playback
        # and the main voice loop alive.
        print(json.dumps({
            "event": "wake_listener_unavailable",
            "error_type": type(exc).__name__,
            "error": str(exc),
            "keyphrase": keyphrase,
        }, ensure_ascii=False))
        return False
    finally:
        if listener is not None:
            try:
                listener.ad.close()
            except Exception:
                pass


def clean_speech_text(text):
    if not text:
        return ""
    # Keep Markdown and citation syntax out of spoken audio. The full answer
    # remains visible in the console, but the voice should read natural prose.
    text = re.sub(r"\[([^\]]+)\]\([^\)]+\)", r"\1", text)
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#]", "", text)
    text = text.replace("«", "").replace("»", "")
    text = text.replace("\u201c", "").replace("\u201d", "")

    # In French, spell the name phonetically so Kokoro says "cagué" rather
    # than the English-style "Kage". Keep the visible transcript untouched
    # and preserve the English pronunciation in English mode.
    if current_response_language() == "fr":
        text = re.sub(r"\bKagé\b|\bKage\b", "cagué", text, flags=re.IGNORECASE)

    # Make euro prices sound natural. Do ranges first so `€100-€300` is not
    # read as three separate symbols/numbers by the speech engine.
    # Allow thousands separators plus an optional decimal part, e.g. 1,000.50.
    number = r"\d+(?:[.,]\d+)*"

    def normalize_euro_number(value):
        value = value.strip()
        # Commas/dots repeated every three digits are thousands separators,
        # not decimal points. Remove them so TTS says "one thousand".
        if re.fullmatch(r"\d{1,3}(?:,\d{3})+", value):
            return value.replace(",", "")
        if re.fullmatch(r"\d{1,3}(?:\.\d{3})+", value):
            return value.replace(".", "")
        if "," in value and "." in value:
            return value.replace(",", "")
        return value.replace(",", ".")

    def euro_range(match):
        low = normalize_euro_number(match.group(1))
        high = normalize_euro_number(match.group(2))
        return f"__KAGE_EURO_RANGE_{low}_{high}__"

    text = re.sub(
        rf"€\s*({number})\s*(?:-|–|—|to)\s*€\s*({number})",
        euro_range,
        text,
        flags=re.IGNORECASE,
    )

    def restore_euro_range(match):
        return f"around {match.group(1)} to {match.group(2)} euros"
    text = re.sub(
        rf"(?:€\s*)?({number})\s*(?:€|EUR)?\s*(?:-|–|—|to)\s*"
        rf"(?:€\s*)?({number})\s*(?:€|EUR)",
        euro_range,
        text,
        flags=re.IGNORECASE,
    )

    def euro_amount(match):
        amount = normalize_euro_number(match.group(1) or match.group(2))
        return f"{amount} euros"

    text = re.sub(
        rf"€\s*({number})|({number})\s*(?:€|EUR)",
        euro_amount,
        text,
        flags=re.IGNORECASE,
    )
    text = re.sub(
        rf"__KAGE_EURO_RANGE_({number})_({number})__",
        restore_euro_range,
        text,
        flags=re.IGNORECASE,
    )
    # A web answer may already say "around" before the currency range. Avoid
    # producing "around around ..." after the speech normalization.
    text = re.sub(r"\baround\s+around\b", "around", text, flags=re.IGNORECASE)
    text = re.sub(r"\s+", " ", text).strip()
    return text


def synthesize_speech(text):
    text = clean_speech_text(text)
    if not text:
        return None, ""
    cached = _waiting_audio.get(text)
    if cached is not None:
        return cached, text
    _, selected_voice = current_voice_settings()
    payload = json.dumps({"text": text, "voice": selected_voice}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        "http://127.0.0.1:8000/speech",
        data=payload,
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "X-Kage-Key": KAGE_API_KEY,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            pcm = response.read()
        samples = np.frombuffer(pcm, dtype=np.int16)
        if samples.size:
            return samples, text
    except Exception as exc:
        print(f"Kokoro unavailable, Windows voice fallback: {exc}")
    return None, text


def speak(text):
    samples, text = synthesize_speech(text)
    if not text:
        return
    if samples is not None:
        publish_assistant_state("speaking")
        play_audio_with_mouth(samples)
        play_sound("speech_finished")
        return
    if WINDOWS_FRENCH_VOICE_AVAILABLE:
        tts.say(text)
        tts.runAndWait()
        play_sound("speech_finished")
    else:
        print(json.dumps({"event": "tts_skipped", "reason": "no_french_voice"}, ensure_ascii=False))


def warm_up_tts():
    """Initialize the local Kokoro request path before the first user turn."""
    started = time.perf_counter()
    try:
        samples, _ = synthesize_speech("D'accord.")
        print(json.dumps({
            "event": "tts_warmup",
            "available": samples is not None,
            "duration_ms": round((time.perf_counter() - started) * 1000, 1),
        }, ensure_ascii=False))
    except Exception as exc:
        print(json.dumps({
            "event": "tts_warmup_failed",
            "error": str(exc),
        }, ensure_ascii=False))


def transcript_rejection_reason(text):
    """Reject obvious STT loops without penalizing ordinary short repetitions."""
    words = re.findall(r"[0-9a-zà-öø-ÿ']+", (text or "").lower())
    if len(words) < 12:
        return None
    counts = collections.Counter(words)
    dominant_count = counts.most_common(1)[0][1]
    if dominant_count / len(words) >= 0.60:
        return "dominant_word_loop"
    if len(set(words)) / len(words) <= 0.20:
        return "low_vocabulary_loop"
    return None


class SentencePlayback:
    """Pre-synthesize queued sentences while the previous audio is playing."""

    def __init__(self):
        self._lock = threading.Lock()
        self._generation = 0
        self._behavior_cue_sent = False
        self._sentences = queue.Queue()
        self._audio = queue.Queue()
        self._release_useful = threading.Event()
        self._play_thread = None
        self.active = threading.Event()

    def start(self, started_at=None, waiting_reply=None, thinking_cue=False):
        self.stop()
        with self._lock:
            self._generation += 1
            generation = self._generation
            self._behavior_cue_sent = False
            self._sentences = queue.Queue()
            self._audio = queue.Queue()
            self._release_useful = threading.Event()
            sentences = self._sentences
            audio = self._audio
            release_useful = self._release_useful
            stream_started_at = started_at
            self.active.set()
        first_useful_audio_logged = False

        def synthesize_sentences():
            while True:
                item = sentences.get()
                if item is None:
                    audio.put(None)
                    return
                sentence, useful = item
                with self._lock:
                    if generation != self._generation:
                        return
                samples, clean_text = synthesize_speech(sentence)
                with self._lock:
                    if generation != self._generation:
                        return
                # The playback worker can now read this audio while this
                # worker immediately prepares the following sentence.
                audio.put((samples, clean_text, useful))

        def play_prepared_audio():
            nonlocal first_useful_audio_logged
            waiting_used = False
            recording_loop = None
            normal_completion = False

            def play_waiting_reply():
                nonlocal waiting_used
                if not waiting_reply:
                    return
                samples, clean_text = synthesize_speech(waiting_reply)
                with self._lock:
                    if generation != self._generation:
                        return
                if samples is not None:
                    publish_assistant_state("speaking")
                    play_audio_with_mouth(samples)
                    publish_assistant_state("thinking")
                    waiting_used = True
                elif clean_text and WINDOWS_FRENCH_VOICE_AVAILABLE:
                    tts.say(clean_text)
                    tts.runAndWait()
                    waiting_used = True

            def play_thinking_cue():
                play_sound("thinking")
                # The cue lasts about 300 ms. Keep it before, not over, speech.
                time.sleep(0.32)

            try:
                # The thinking cue is intentionally emitted only after STT
                # validation in handle_utterance. Keep this optional flag for
                # compatibility, but do not delay the opening reply here.
                if thinking_cue:
                    play_thinking_cue()
                if thinking_cue or waiting_reply:
                    recording_loop = start_loop("recording_loop")
                play_waiting_reply()
                while True:
                    prepared = audio.get()
                    if prepared is None:
                        normal_completion = True
                        return
                    samples, clean_text, useful = prepared
                    with self._lock:
                        if generation != self._generation:
                            return
                    if useful and not release_useful.wait(timeout=None):
                        return
                    if useful and not first_useful_audio_logged:
                        stop_loop(recording_loop)
                        recording_loop = None
                        print(json.dumps({
                            "event": "tts_first_useful_audio",
                            "since_stream_start_ms": round((time.perf_counter() - stream_started_at) * 1000, 1)
                            if stream_started_at else None,
                        }, ensure_ascii=False))
                        first_useful_audio_logged = True
                        publish_assistant_state("speaking")
                    if samples is not None:
                        play_audio_with_mouth(samples)
                    elif clean_text and WINDOWS_FRENCH_VOICE_AVAILABLE:
                        tts.say(clean_text)
                        tts.runAndWait()
                    elif clean_text:
                        print(json.dumps({"event": "tts_skipped", "reason": "no_french_voice"}, ensure_ascii=False))
            finally:
                stop_loop(recording_loop)
                if normal_completion:
                    play_sound("speech_finished")
                if thinking_cue or waiting_reply:
                    print(json.dumps({"event": "waiting_reply", "used": waiting_used}, ensure_ascii=False))
                with self._lock:
                    if generation == self._generation:
                        self.active.clear()

        playback_thread = threading.Thread(
            target=play_prepared_audio, name="kage-sentence-playback", daemon=True
        )
        with self._lock:
            self._play_thread = playback_thread
        threading.Thread(target=synthesize_sentences, name="kage-sentence-synthesis", daemon=True).start()
        playback_thread.start()

    def enqueue(self, text, useful=False):
        if text and text.strip():
            if useful and not self._behavior_cue_sent:
                self._behavior_cue_sent = True
                publish_behavior_cue(classify_behavior_cue(text))
            self._sentences.put((text.strip(), useful))

    def finish(self):
        self._sentences.put(None)

    def release_useful(self):
        self._release_useful.set()

    def stop(self):
        with self._lock:
            self._generation += 1
            self.active.clear()
            sentences = self._sentences
            audio = self._audio
            release_useful = self._release_useful
            playback_thread = self._play_thread
            self._play_thread = None
        # Wake both workers when a reply is interrupted.
        release_useful.set()
        sentences.put(None)
        audio.put(None)
        sd.stop()
        tts.stop()
        # The next wake-word listener must not open the microphone while the
        # interrupted output worker is still releasing PortAudio/Windows TTS.
        if playback_thread is not None and playback_thread is not threading.current_thread():
            playback_thread.join(timeout=3)
            if playback_thread.is_alive():
                print(json.dumps({"event": "playback_worker_still_active_after_stop"}))


MAX_SPOKEN_SENTENCES = 3
FIRST_SPOKEN_MIN_WORDS = 5
FIRST_SPOKEN_MAX_WORDS = 8


def split_complete_sentences(buffer, max_words=None):
    """Return sentence-sized chunks and the incomplete tail of an LLM stream."""
    complete = []
    while True:
        match = re.search(r"[.!?](?=\s|$)", buffer)
        clause_match = re.search(r"[;:](?=\s)", buffer)

        # Keep the first spoken unit as a real short sentence. We wait through
        # five words for the model's own full stop, but force a safe boundary at
        # eight words if the model ignores the instruction. A forced boundary
        # gets a period so Kokoro does not read a bare sentence fragment.
        if max_words:
            words = list(re.finditer(r"[0-9A-Za-zÀ-ÖØ-öø-ÿ]+(?:['’][0-9A-Za-zÀ-ÖØ-öø-ÿ]+)?", buffer))
            if len(words) >= max_words:
                cut = words[max_words - 1].end()
                if len(buffer) > cut:
                    while cut < len(buffer) and buffer[cut] in ",;:!?":
                        cut += 1
                    if not match or cut < match.end():
                        phrase = buffer[:cut].strip()
                        buffer = buffer[cut:].lstrip(" ,;:-")
                        if phrase:
                            complete.append(phrase.rstrip(".!?") + ".")
                        max_words = None
                        continue

        # A long clause can be synthesized while the model writes the rest of
        # the sentence.  Keep very short clauses buffered to avoid choppy speech
        # and avoid treating a URL scheme such as "https:" as a boundary.
        if clause_match:
            clause = buffer[:clause_match.end()]
            word_count = len(re.findall(r"[A-Za-z0-9]+(?:'[A-Za-z0-9]+)?", clause))
            if word_count >= 8 and (not match or clause_match.start() < match.start()):
                match = clause_match
        if not match:
            return complete, buffer
        sentence = buffer[:match.end()].strip()
        buffer = buffer[match.end():].lstrip()
        if sentence:
            complete.append(sentence)


def stream_to_kage(text, events):
    """Read the local newline-delimited response stream on a worker thread."""
    payload = json.dumps({"message": text}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        "http://127.0.0.1:8000/ask/stream",
        data=payload,
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "X-Kage-Key": KAGE_API_KEY,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=75) as response:
            for raw_line in response:
                line = raw_line.decode("utf-8").strip()
                if line:
                    events.put(json.loads(line))
    except Exception as exc:
        events.put({"type": "error", "detail": str(exc)})


def speak_streaming_reply_with_barge_in(text, waiting_reply=None, transcription_ms=None,
                                       thinking_cue=True):
    """Speak sentence chunks while Codex is still generating the remaining reply."""
    stream_started_at = time.perf_counter()
    events = queue.Queue()
    playback = SentencePlayback()
    playback.start(stream_started_at, waiting_reply=waiting_reply, thinking_cue=thinking_cue)

    threading.Thread(
        target=stream_to_kage,
        args=(text, events),
        name="kage-response-stream",
        daemon=True,
    ).start()
    pending = ""
    complete_sentence_count = 0
    streamed_text_received = False
    streamed_audio_queued = False
    stream_started = False
    first_delta_ms = None
    first_sentence_ms = None
    completed_ms = None
    result = None
    completed = False
    while not completed:
        if consume_interrupt_request():
            playback.stop()
            return None, True
        try:
            event = events.get(timeout=0.05)
        except queue.Empty:
            continue
        event_type = event.get("type")
        if event_type == "delta":
            delta = event.get("text", "")
            if delta:
                streamed_text_received = True
                if first_delta_ms is None:
                    first_delta_ms = round((time.perf_counter() - stream_started_at) * 1000, 1)
            pending += delta
            sentences, pending = split_complete_sentences(
                pending,
                max_words=FIRST_SPOKEN_MAX_WORDS if complete_sentence_count == 0 else None,
            )
            for sentence in sentences:
                if complete_sentence_count >= MAX_SPOKEN_SENTENCES:
                    continue
                playback.enqueue(sentence, useful=True)
                streamed_audio_queued = True
                complete_sentence_count += 1
            # Release the first complete sentence immediately. Later sentences
            # stay queued and are synthesized while the first one is spoken.
            if not stream_started and complete_sentence_count >= 1:
                stream_started = True
                first_sentence_ms = round((time.perf_counter() - stream_started_at) * 1000, 1)
                playback.release_useful()
        elif event_type == "done":
            completed_ms = round((time.perf_counter() - stream_started_at) * 1000, 1)
            result = event.get("result", {})
            if pending.strip() and complete_sentence_count < MAX_SPOKEN_SENTENCES:
                playback.enqueue(pending, useful=True)
                streamed_audio_queued = True
                complete_sentence_count += 1
            if complete_sentence_count and not stream_started:
                # A reply without a complete sentence should not stay held once
                # the model has completed.
                playback.release_useful()
            # Fall back to the completed answer only when the server did not
            # send any usable stream text (for example the Ollama fallback).
            elif (not streamed_text_received and not streamed_audio_queued
                  and result.get("speak", True) and result.get("reply")):
                playback.enqueue(result["reply"], useful=True)
                playback.release_useful()
            playback.finish()
            completed = True
        elif event_type == "error":
            playback.stop()
            raise RuntimeError(f"Streaming response failed: {event.get('detail', 'unknown error')}")

    while playback.active.is_set():
        if consume_interrupt_request():
            playback.stop()
            return None, True
        time.sleep(0.03)

    print(json.dumps({
        "event": "streaming_timing",
        "transcription_ms": transcription_ms,
        "first_delta_ms": first_delta_ms,
        "first_sentence_ms": first_sentence_ms,
        "generation_complete_ms": completed_ms,
        "total_until_playback_done_ms": round((time.perf_counter() - stream_started_at) * 1000, 1),
    }, ensure_ascii=False))
    return result or {}, False


# ---------- MICRO ----------

def rms(block):
    return float(np.sqrt(np.mean(np.square(block))))


_last_record_metrics = {}


def record_until_silence(wait_for_speech_seconds=MAX_RECORD_SECONDS):
    global _last_record_metrics
    capture_started = time.perf_counter()
    block_size = int(SAMPLE_RATE * BLOCK_MS / 1000)
    pre_roll_blocks = max(1, int(PRE_ROLL_SECONDS * 1000 / BLOCK_MS))
    pre_roll = collections.deque(maxlen=pre_roll_blocks)

    print("\n🎤 J'écoute... parle maintenant.")

    # Petite mesure du bruit ambiant
    noise_values = []

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="float32",
        device=MIC_DEVICE,
        blocksize=block_size,
    ) as stream:

        calibration_blocks = []
        for _ in range(10):
            block, _ = stream.read(block_size)
            noise_values.append(rms(block))
            calibration_blocks.append(block.copy())

        noise_floor = max(sum(noise_values) / len(noise_values), 0.002)
        speech_threshold = max(noise_floor * 3.0, 0.012)

        hold_mode = hold_is_active()
        frames = calibration_blocks.copy() if hold_mode else []
        speech_started = False
        silent_time = 0.0
        waited_for_speech = 0.0
        utterance_time = 0.0

        while True:
            if hold_mode and not hold_is_active():
                print("🔵 Push-to-talk released — transcribing")
                break
            if consume_control_flag(VOICE_SLEEP_FLAG):
                print("⚪ Kage sleeping from touch")
                return "touch_sleep"
            block, overflowed = stream.read(block_size)

            level = rms(block)
            if hold_is_active():
                if not hold_mode:
                    hold_mode = True
                    frames.extend(list(pre_roll))
                frames.append(block.copy())
                continue
            if hold_mode:
                print("🔵 Push-to-talk released — transcribing")
                break

            if not speech_started:
                waited_for_speech += BLOCK_MS / 1000
                if waited_for_speech >= wait_for_speech_seconds:
                    break
                pre_roll.append(block.copy())

                if level >= speech_threshold:
                    speech_started = True
                    print("🟢 Parole détectée")

                    frames.extend(list(pre_roll))
                    frames.append(block.copy())

            else:
                utterance_time += BLOCK_MS / 1000
                frames.append(block.copy())

                if utterance_time >= MAX_RECORD_SECONDS:
                    print("🔵 Phrase maximale atteinte")
                    break

                if level < speech_threshold:
                    silent_time += BLOCK_MS / 1000
                else:
                    silent_time = 0.0

                if silent_time >= SILENCE_AFTER_SPEECH:
                    print("🔵 Fin de phrase détectée")
                    break

    if not speech_started and not hold_mode:
        return False
    if hold_mode and len(frames) < 3:
        return False

    audio = np.concatenate(frames, axis=0)

    audio_int16 = np.clip(
        audio * 32767,
        -32768,
        32767,
    ).astype(np.int16)

    with wave.open(WAV_FILE, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(audio_int16.tobytes())

    _last_record_metrics = {
        "audio_duration_ms": round(len(audio_int16) * 1000 / SAMPLE_RATE, 1),
        "endpoint_silence_ms": round(silent_time * 1000, 1),
        "capture_total_ms": round((time.perf_counter() - capture_started) * 1000, 1),
        "hold_mode": hold_mode,
        "silence_setting_ms": round(SILENCE_AFTER_SPEECH * 1000, 1),
    }

    return True


# ---------- TRANSCRIPTION ----------

def normalize_kage_name(text):
    replacements = [
        "cagée",
        "cagé",
        "cage",
        "cager",
        "kagé",
        "kage",
        "Cagée",
        "Cagé",
        "Cage",
        "Kagé",
    ]

    for alias in replacements:
        text = text.replace(alias, "Kage")

    return text


def transcribe():
    result = stt.transcribe(WAV_FILE)
    result.text = normalize_kage_name(result.text)
    return result


# ---------- QWEN / KAGE ----------

def send_to_kage(text):
    if not KAGE_API_KEY:
        raise RuntimeError("KAGE_API_KEY n'est pas disponible dans cette session.")

    payload = json.dumps(
        {"message": text},
        ensure_ascii=False,
    ).encode("utf-8")

    request = urllib.request.Request(
        "http://127.0.0.1:8000/ask",
        data=payload,
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "X-Kage-Key": KAGE_API_KEY,
        },
        method="POST",
    )

    with urllib.request.urlopen(
        request,
        timeout=60,
    ) as response:
        return json.loads(
            response.read().decode("utf-8")
        )


# ---------- BOUCLE ----------

def handle_utterance(wait_for_speech_seconds, simulated_text=None):
    try:
        publish_assistant_state("listening")
        recorded = True if simulated_text is not None else record_until_silence(wait_for_speech_seconds)

        if recorded == "touch_sleep":
            return recorded
        if not recorded:
            return False

        audio_duration_ms = float(_last_record_metrics.get("audio_duration_ms", 0.0))
        if simulated_text is None and audio_duration_ms < MIN_AUDIO_DURATION_MS:
            print(json.dumps({
                "event": "transcription_rejected",
                "transcription_ms": 0.0,
                "reason": "audio_too_short",
                "audio_duration_ms": round(audio_duration_ms, 1),
                "min_audio_duration_ms": MIN_AUDIO_DURATION_MS,
            }, ensure_ascii=False))
            publish_assistant_state("listening")
            return True

        # RMS only detects sound energy, not intelligible speech. Wait for the
        # transcript gate before playing the end-of-transcription cue, so a
        # noise-only false alert does not produce a misleading thinking sound.
        if simulated_text is not None:
            text = simulated_text.strip()
            transcription_ms = 0.0
            print(json.dumps({"event": "text_mode_transcript", "text": text}, ensure_ascii=False))
        else:
            stt_result = transcribe()
            text = stt_result.text
            transcription_ms = round(stt_result.duration_ms, 1)
            print(json.dumps({
                "event": "speech_to_transcript_timing",
                **_last_record_metrics,
                "stt_ms": transcription_ms,
                "endpoint_plus_stt_ms": round(
                    _last_record_metrics.get("endpoint_silence_ms", 0) + transcription_ms, 1
                ),
            }, ensure_ascii=False))

        rejection_reason = transcript_rejection_reason(text)
        if not text or rejection_reason:
            print("❌ Je n'ai pas compris.")
            print(json.dumps({
                "event": "transcription_rejected",
                "transcription_ms": transcription_ms,
                "reason": rejection_reason or "empty",
            }, ensure_ascii=False))
            publish_assistant_state("listening")
            return True

        print(f"📝 Entendu : {text}")
        if simulated_text is not None:
            print(json.dumps({
                "event": "stt_result",
                "text": text,
                "engine": "text_mode",
                "duration_ms": 0.0,
                "language": current_response_language(),
                "confidence": 1.0,
                "confidence_usable": True,
                "fallback_used": False,
            }, ensure_ascii=False))
        else:
            print(json.dumps({"event": "stt_result", **stt_result.log_payload()}, ensure_ascii=False))

        # The cue now means that Kagé accepted intelligible speech and is
        # starting to process it. Rejected/empty transcripts remain silent.
        play_sound("thinking")

        # Local actions and session closure have their own feedback sounds.
        # Conversational feedback is scheduled against real first-audio time.
        direct_command = is_direct_command(text)
        publish_assistant_state("thinking")

        ending = session_intent(text)
        if ending:
            if ending == "shutdown":
                # Queue the closure cue before notifying the backend and
                # exiting. Playback remains independent from shutdown.
                play_sound("triple_close")
                publish_voice_session(False, wait=True)
                publish_assistant_state("idle")
                return "shutdown"
            request_executor.submit(send_to_kage, text)
            publish_assistant_state("speaking")
            speak(random.choice(SESSION_END_REPLIES))
            publish_assistant_state("idle")
            return "end"

        # Codex sends text chunks as it generates them. Completed sentences
        # are queued for Kokoro immediately, rather than waiting for the full
        # response. Direct commands remain locally routed by the backend.
        waiting_reply = None if direct_command else choose_waiting_reply(text)
        result, was_interrupted = speak_streaming_reply_with_barge_in(
            text,
            waiting_reply,
            transcription_ms=transcription_ms,
            # The cue was already played immediately after capture. This call
            # is responsible only for the cached opening and streamed answer.
            thinking_cue=False,
        )
        if was_interrupted:
            # Do not reopen the microphone after stopping speech: otherwise
            # Kage can transcribe its own last spoken audio/echo.
            publish_assistant_state("idle")
            return "interrupted"

        print(f"🤖 Kage : {result['reply']}")
        print(f"🎭 Commande : {result['command']}")
        print(f"🔢 Séquence : {result['sequence']}")
        # A normal response keeps the existing short follow-up conversation
        # window. Only a touch interruption exits back to the wake word.
        publish_assistant_state("listening")
        return True

    except Exception as e:
        print(f"❌ Erreur : {e}")
        return True


def main():
    """Run one interactive voice loop in the primary Python process only."""
    load_waiting_audio()
    print(json.dumps({"event": "waiting_audio", "loaded": len(_waiting_audio)}, ensure_ascii=False))
    warm_up_tts()
    publish_voice_session(True)
    start_voice_session_heartbeat()
    print("\nKage Voice prêt.")

    # Keep the optional text box visible alongside the normal audio console.
    # Audio remains the default; the box only injects text when submitted.
    if not os.path.exists(VOICE_TEXT_MODE_FLAG):
        try:
            open(VOICE_TEXT_MODE_FLAG, "w", encoding="ascii").close()
        except OSError:
            pass
    _start_text_window_if_requested()

    text_mode = TEXT_MODE_ON_START
    if text_mode:
        print("⌨️ Mode écriture actif — tape une phrase puis Entrée. /micro revient au micro, /q quitte.")
    start_direct = START_LISTENING_ON_LAUNCH or text_mode
    while True:
        if text_mode:
            try:
                choice = input("\n⌨️ Texte (/micro = microphone, /q = quitter) : ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if choice.lower() in {"/q", "q", "quit", "quitter"}:
                break
            if choice.lower() in {"/micro", "micro", "m"}:
                text_mode = False
                print("🎤 Mode microphone actif.")
                continue
            if not choice:
                continue
            outcome = handle_utterance(MAX_RECORD_SECONDS, simulated_text=choice)
            if outcome == "shutdown":
                break
            continue
        _start_text_window_if_requested()
        if hold_is_active():
            start_direct = True
        if WAKE_WORD_ENABLED and not start_direct:
            try:
                wake_source = wait_for_wake_word()
            except KeyboardInterrupt:
                break
            if isinstance(wake_source, tuple) and wake_source[0] == "text_input":
                outcome = handle_utterance(MAX_RECORD_SECONDS, simulated_text=wake_source[1])
                if outcome == "shutdown":
                    break
                continue
            # Touch already emitted its confirmation cue in the backend.
            if wake_source == "wake_word":
                play_sound("wake_listening")
            if wake_source != "hold" and not hold_is_active():
                speak(listening_announcement())
            wait_time = FOLLOW_UP_TIMEOUT_SECONDS
        else:
            start_direct = False
            if WAKE_WORD_ENABLED:
                if not hold_is_active():
                    speak(listening_announcement())
                wait_time = FOLLOW_UP_TIMEOUT_SECONDS
                # Do not fall through to the keyboard-only branch.
            else:
                choice = input("\nEntrée = parler | t = mode écriture | q = quitter : ")
                if choice.lower() == "q":
                    break
                if choice.lower() in {"t", "texte", "/texte"}:
                    text_mode = True
                    print("⌨️ Mode écriture actif — tape une phrase puis Entrée. /micro revient au micro.")
                    continue
                wait_time = MAX_RECORD_SECONDS

        while True:
            outcome = handle_utterance(wait_time)
            if not outcome or outcome in {"end", "shutdown", "interrupted", "touch_sleep"}:
                break
            if not WAKE_WORD_ENABLED:
                break
            print(f"🟣 Conversation active — listening for {int(FOLLOW_UP_TIMEOUT_SECONDS)} more seconds.")

        if WAKE_WORD_ENABLED:
            # The physical double-tap and interruption cues are already more
            # specific than a generic sleep cue, so do not stack sounds.
            if outcome not in {"shutdown", "interrupted", "touch_sleep"}:
                play_sound("sleep_listening")
            publish_assistant_state("idle")
            print("⚪ Conversation ended — returning to wake word.")
        if outcome == "shutdown":
            break

    if os.path.exists(WAV_FILE):
        os.remove(WAV_FILE)


if __name__ == "__main__":
    main()
