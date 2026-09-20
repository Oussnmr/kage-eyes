from faster_whisper import WhisperModel
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
from pocketsphinx import LiveSpeech

# A detached PowerShell window can default to a legacy Windows code page.
# Keep status messages from terminating the assistant when they contain accents
# or visual state icons.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, OSError):
    pass

SAMPLE_RATE = 16000
MIC_DEVICE = None  # garde le numéro qui fonctionne actuellement chez toi
WAV_FILE = r"C:\Kage\voice_temp.wav"
KAGE_API_KEY = os.getenv("KAGE_API_KEY", "").strip()

# Détection de voix
BLOCK_MS = 50
SILENCE_AFTER_SPEECH = 0.7   # traite après 0,7 s de silence
MAX_RECORD_SECONDS = 12
PRE_ROLL_SECONDS = 0.30
WAKE_WORD_ENABLED = os.getenv("KAGE_WAKE_WORD", "1").strip().lower() in {"1", "true", "yes", "on"}
WAKE_KEYPHRASE = "wake up"
INTERRUPT_KEYPHRASE = "cage cancel"  # Distinct two-word barge-in phrase.
WAKE_THRESHOLD = float(os.getenv("KAGE_WAKE_THRESHOLD", "1e-18"))
FOLLOW_UP_TIMEOUT_SECONDS = float(os.getenv("KAGE_FOLLOW_UP_TIMEOUT", "25"))
WAITING_REPLIES_ENABLED = os.getenv("KAGE_WAITING_REPLIES", "1").strip().lower() in {
    "1", "true", "yes", "on",
}

print("Chargement de Whisper...")

model = WhisperModel(
    "small",
    device="cpu",
    compute_type="int8",
)

print("Whisper prêt.")


# ---------- VOIX DE KAGE ----------

tts = pyttsx3.init()
tts.setProperty("rate", 185)
tts.setProperty("volume", 1.0)


def choose_english_male_voice():
    voices = tts.getProperty("voices")

    for voice in voices:
        info = (
            str(voice.name) + " "
            + str(voice.id) + " "
            + str(getattr(voice, "languages", ""))
        ).lower()

        if (("english" in info or "en-us" in info or "en_us" in info or
              "en-gb" in info or "en_gb" in info) and
                not any(marker in info for marker in ("zira", "hazel", "female", "woman"))):
            tts.setProperty("voice", voice.id)
            print(f"Voix Kage : {voice.name}")
            return

    print("Aucune voix masculine anglaise spécifique trouvée, voix Windows par défaut utilisée.")


choose_english_male_voice()

request_executor = ThreadPoolExecutor(max_workers=1)
WAITING_REPLIES = (
    "Okay, one second.",
    "I'm thinking.",
    "Let me think.",
    "Hmm, let me see.",
    "Alright, one moment.",
    "Give me a second.",
    "Just a second.",
    "I'm working on that.",
    "Hmm...",
    "Okay, I see.",
    "Sure, let me check.",
    "Right, I’m looking into it.",
    "One moment while I work that out.",
    "Let me find the best answer.",
    "I’m checking that now.",
    "Alright, I’m on it.",
    "Just a moment while I look into this.",
    "I’m putting that together now.",
    "Let me work through that.",
    "I’ll check that for you.",
)
SESSION_END_REPLIES = (
    "Kagé is here if you need me.",
    "I’m here whenever you need me.",
    "Feel free to ask me anything.",
    "I’ll be here if you have another question.",
    "No problem. Just say Kagé when you need me.",
    "Alright. I’m listening whenever you’re ready.",
    "I’ll be here when you need me.",
    "All right, just call me if you need anything.",
    "I’m standing by if another question comes up.",
    "Whenever you’re ready, I’m here.",
    "No worries. I’ll wait here.",
    "Just say wake up when you want me again.",
    "I’m ready whenever you are.",
    "I’ll stay quiet until you call me.",
)


def is_direct_command(text):
    normalized = re.sub(r"[^a-z0-9 ]", " ", text.lower())
    normalized = re.sub(r"\s+", " ", normalized).strip()
    phrases = (
        "stop", "be normal", "return to normal", "go back to normal",
        "calm down", "relax", "reset yourself", "return to idle", "go idle",
        "normal mode", "back to idle", "blink", "blink your eyes", "close your eyes",
        "make your eyes blink", "blink twice", "go to sleep", "sleep", "sleep now",
        "enter sleep mode", "take a nap", "rest", "be angry", "get angry",
        "act angry", "look angry", "show me angry", "angry mode", "be dizzy",
        "get dizzy", "spin", "spin around", "act dizzy", "look dizzy", "dizzy mode",
    )
    return any(normalized == phrase or normalized.endswith(" " + phrase)
               for phrase in phrases)


def is_end_session(text):
    normalized = re.sub(r"[^a-z0-9 ]", " ", text.lower())
    normalized = re.sub(r"\s+", " ", normalized).strip()
    phrases = (
        "stop", "you can stop here", "you can stop now", "stop listening", "stop talking",
        "please stop", "stop for now", "let us stop here", "let's stop here",
        "we can stop here", "that's enough for now", "you can be quiet",
        "go quiet", "end the conversation",
        "end the conversation", "end chat", "end this chat", "cancel chat",
        "be quiet", "quiet", "enough", "that's enough", "that is enough",
        "no more", "stop now",
        "that's all", "that is all", "we are done", "we're done",
        "goodbye", "bye for now", "go back to sleep", "go idle", "wait for wake up",
        "wait until I call you", "stop the chat", "finish the conversation",
    )
    return any(normalized == phrase or normalized.endswith(" " + phrase)
               for phrase in phrases)


def choose_waiting_reply(text):
    """Return None for short/simple requests where silence feels better."""
    if not WAITING_REPLIES_ENABLED:
        return None
    normalized = re.sub(r"\s+", " ", text.strip())
    word_count = len(normalized.split())
    if len(normalized) <= 14 or word_count <= 3:
        if random.random() < 0.70:
            return None
    return random.choice(WAITING_REPLIES)


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
        with listener.ad:
            while stop_event is None or not stop_event.is_set():
                audio, _ = listener.ad.read(listener.buffer_size // 2)
                speech = listener.ep.process(audio)
                if speech is None:
                    continue
                if not listener.in_speech:
                    listener.start_utt()
                listener.process_raw(speech)
                if listener.hyp():
                    listener.end_utt()
                    print("🟣 Kage detected")
                    return True
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
    payload = json.dumps({"text": text, "voice": "am_adam"}, ensure_ascii=False).encode("utf-8")
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
        sd.play(samples, samplerate=16000, blocking=True)
        return
    tts.say(text)
    tts.runAndWait()


def warm_up_tts():
    """Initialize the local Kokoro request path before the first user turn."""
    started = time.perf_counter()
    try:
        samples, _ = synthesize_speech("Okay.")
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


class InterruptiblePlayback:
    def __init__(self):
        self._lock = threading.Lock()
        self._generation = 0
        self.active = threading.Event()

    def start(self, text):
        self.stop()
        with self._lock:
            self._generation += 1
            generation = self._generation
            self.active.set()

        def play():
            try:
                samples, clean_text = synthesize_speech(text)
                with self._lock:
                    if generation != self._generation:
                        return
                if samples is not None:
                    sd.play(samples, samplerate=16000, blocking=True)
                elif clean_text:
                    tts.say(clean_text)
                    tts.runAndWait()
            finally:
                with self._lock:
                    if generation == self._generation:
                        self.active.clear()

        threading.Thread(target=play, name="kage-playback", daemon=True).start()

    def stop(self):
        with self._lock:
            self._generation += 1
            self.active.clear()
        sd.stop()
        tts.stop()


speech_playback = InterruptiblePlayback()


class SentencePlayback:
    """Pre-synthesize queued sentences while the previous audio is playing."""

    def __init__(self):
        self._lock = threading.Lock()
        self._generation = 0
        self._sentences = queue.Queue()
        self._audio = queue.Queue()
        self._release_useful = threading.Event()
        self.active = threading.Event()

    def start(self, started_at=None):
        self.stop()
        with self._lock:
            self._generation += 1
            generation = self._generation
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
            try:
                while True:
                    prepared = audio.get()
                    if prepared is None:
                        return
                    samples, clean_text, useful = prepared
                    with self._lock:
                        if generation != self._generation:
                            return
                    if useful and not release_useful.wait(timeout=None):
                        return
                    if useful and not first_useful_audio_logged:
                        print(json.dumps({
                            "event": "tts_first_useful_audio",
                            "since_stream_start_ms": round((time.perf_counter() - stream_started_at) * 1000, 1)
                            if stream_started_at else None,
                        }, ensure_ascii=False))
                        first_useful_audio_logged = True
                    if samples is not None:
                        sd.play(samples, samplerate=16000, blocking=True)
                    elif clean_text:
                        tts.say(clean_text)
                        tts.runAndWait()
            finally:
                with self._lock:
                    if generation == self._generation:
                        self.active.clear()

        threading.Thread(target=synthesize_sentences, name="kage-sentence-synthesis", daemon=True).start()
        threading.Thread(target=play_prepared_audio, name="kage-sentence-playback", daemon=True).start()

    def enqueue(self, text, useful=False):
        if text and text.strip():
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
        # Wake both workers when a reply is interrupted.
        release_useful.set()
        sentences.put(None)
        audio.put(None)
        sd.stop()
        tts.stop()


def split_complete_sentences(buffer):
    """Return sentence-sized chunks and the incomplete tail of an LLM stream."""
    complete = []
    while True:
        match = re.search(r"[.!?](?=\s|$)", buffer)
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


def speak_streaming_reply_with_barge_in(text, waiting_reply=None, transcription_ms=None):
    """Speak sentence chunks while Codex is still generating the remaining reply."""
    stream_started_at = time.perf_counter()
    events = queue.Queue()
    interrupted = threading.Event()
    listener_stop = threading.Event()
    playback = SentencePlayback()
    playback.start(stream_started_at)
    if waiting_reply:
        playback.enqueue(waiting_reply)
    print(json.dumps({
        "event": "waiting_reply",
        "used": bool(waiting_reply),
    }, ensure_ascii=False))

    def listen_for_interrupt():
        if wait_for_wake_word(
            listener_stop,
            announce=False,
            keyphrase=INTERRUPT_KEYPHRASE,
        ):
            interrupted.set()

    threading.Thread(
        target=stream_to_kage,
        args=(text, events),
        name="kage-response-stream",
        daemon=True,
    ).start()
    listener = threading.Thread(target=listen_for_interrupt, name="kage-stream-barge-in", daemon=True)
    listener.start()

    pending = ""
    complete_sentence_count = 0
    streamed_text_received = False
    streamed_audio_queued = False
    stream_started = False
    first_delta_ms = None
    two_sentences_ms = None
    completed_ms = None
    result = None
    completed = False
    while not completed:
        if interrupted.is_set():
            playback.stop()
            listener_stop.set()
            listener.join(timeout=1)
            print("🛑 Streaming reply interrupted by wake word")
            speak("Kagé is listening.")
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
            sentences, pending = split_complete_sentences(pending)
            for sentence in sentences:
                playback.enqueue(sentence, useful=True)
                streamed_audio_queued = True
                complete_sentence_count += 1
            # Wait for two complete sentences before beginning the substantive
            # reply. The first sentence is synthesized immediately but held in
            # the playback worker until the second one is ready.
            if not stream_started and complete_sentence_count >= 2:
                stream_started = True
                two_sentences_ms = round((time.perf_counter() - stream_started_at) * 1000, 1)
                playback.release_useful()
        elif event_type == "done":
            completed_ms = round((time.perf_counter() - stream_started_at) * 1000, 1)
            result = event.get("result", {})
            if pending.strip():
                playback.enqueue(pending, useful=True)
                streamed_audio_queued = True
                complete_sentence_count += 1
            if complete_sentence_count and not stream_started:
                # Short replies with fewer than two sentences should not stay
                # held forever once the model has completed.
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
            listener_stop.set()
            listener.join(timeout=1)
            raise RuntimeError(f"Streaming response failed: {event.get('detail', 'unknown error')}")

    while playback.active.is_set():
        if interrupted.is_set():
            playback.stop()
            listener_stop.set()
            listener.join(timeout=1)
            print("🛑 Streaming reply interrupted by wake word")
            speak("Kagé is listening.")
            return None, True
        time.sleep(0.03)

    listener_stop.set()
    listener.join(timeout=1)
    print(json.dumps({
        "event": "streaming_timing",
        "transcription_ms": transcription_ms,
        "first_delta_ms": first_delta_ms,
        "two_sentences_ms": two_sentences_ms,
        "generation_complete_ms": completed_ms,
        "total_until_playback_done_ms": round((time.perf_counter() - stream_started_at) * 1000, 1),
    }, ensure_ascii=False))
    return result or {}, False


def speak_reply_with_barge_in(text):
    """Speak a conversational reply while listening only for a local wake word."""
    listener_stop = threading.Event()
    interrupted = threading.Event()

    def listen_for_interrupt():
        if wait_for_wake_word(
            listener_stop,
            announce=False,
            keyphrase=INTERRUPT_KEYPHRASE,
        ):
            interrupted.set()

    speech_playback.start(text)
    listener = threading.Thread(target=listen_for_interrupt, name="kage-barge-in", daemon=True)
    listener.start()

    while speech_playback.active.is_set():
        if interrupted.is_set():
            speech_playback.stop()
            listener_stop.set()
            listener.join(timeout=1)
            print("🛑 Reply interrupted by wake word")
            speak("Kagé is listening.")
            return True
        time.sleep(0.03)

    listener_stop.set()
    listener.join(timeout=1)
    return False


# ---------- MICRO ----------

def rms(block):
    return float(np.sqrt(np.mean(np.square(block))))


def record_until_silence(wait_for_speech_seconds=MAX_RECORD_SECONDS):
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

        for _ in range(10):
            block, _ = stream.read(block_size)
            noise_values.append(rms(block))

        noise_floor = max(sum(noise_values) / len(noise_values), 0.002)
        speech_threshold = max(noise_floor * 3.0, 0.012)

        frames = []
        speech_started = False
        silent_time = 0.0
        waited_for_speech = 0.0
        utterance_time = 0.0

        while True:
            block, overflowed = stream.read(block_size)

            level = rms(block)

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

    if not speech_started:
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
    segments, _ = model.transcribe(
        WAV_FILE,
            language="en",
        vad_filter=True,
        beam_size=3,
        condition_on_previous_text=False,
        initial_prompt=(
            "The robot is named Kage. Keep the name pronounced Kage. "
            "The user speaks English."
        ),
    )

    text = " ".join(
        segment.text.strip()
        for segment in segments
    ).strip()

    return normalize_kage_name(text)


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

def handle_utterance(wait_for_speech_seconds):
    try:
        recorded = record_until_silence(wait_for_speech_seconds)

        if not recorded:
            return False

        transcription_started = time.perf_counter()
        text = transcribe()
        transcription_ms = round((time.perf_counter() - transcription_started) * 1000, 1)

        if not text:
            print("❌ Je n'ai pas compris.")
            return True

        print(f"📝 Entendu : {text}")

        if is_end_session(text):
            request_executor.submit(send_to_kage, text)
            speak(random.choice(SESSION_END_REPLIES))
            return "end"

        # Codex sends text chunks as it generates them. Completed sentences
        # are queued for Kokoro immediately, rather than waiting for the full
        # response. Direct commands remain locally routed by the backend.
        waiting_reply = None if is_direct_command(text) else choose_waiting_reply(text)
        result, was_interrupted = speak_streaming_reply_with_barge_in(
            text,
            waiting_reply,
            transcription_ms=transcription_ms,
        )
        if was_interrupted:
            return "interrupted"

        print(f"🤖 Kage : {result['reply']}")
        print(f"🎭 Commande : {result['command']}")
        print(f"🔢 Séquence : {result['sequence']}")

        return True

    except Exception as e:
        print(f"❌ Erreur : {e}")
        return True


def main():
    """Run one interactive voice loop in the primary Python process only."""
    warm_up_tts()
    print("\nKage Voice prêt.")

    while True:
        if WAKE_WORD_ENABLED:
            try:
                wait_for_wake_word()
            except KeyboardInterrupt:
                break
            speak("Kagé is listening.")
            wait_time = FOLLOW_UP_TIMEOUT_SECONDS
        else:
            choice = input("\nEntrée = parler | q = quitter : ")
            if choice.lower() == "q":
                break
            wait_time = MAX_RECORD_SECONDS

        while True:
            outcome = handle_utterance(wait_time)
            if not outcome or outcome == "end":
                break
            if not WAKE_WORD_ENABLED:
                break
            print(f"🟣 Conversation active — listening for {int(FOLLOW_UP_TIMEOUT_SECONDS)} more seconds.")

        if WAKE_WORD_ENABLED:
            print("⚪ Conversation ended — returning to wake word.")

    if os.path.exists(WAV_FILE):
        os.remove(WAV_FILE)


if __name__ == "__main__":
    main()
