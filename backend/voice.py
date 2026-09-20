from faster_whisper import WhisperModel
import sounddevice as sd
import numpy as np
import urllib.request
import json
import wave
import os
import collections
import pyttsx3
from concurrent.futures import ThreadPoolExecutor

SAMPLE_RATE = 16000
MIC_DEVICE = None  # garde le numéro qui fonctionne actuellement chez toi
WAV_FILE = r"C:\Kage\voice_temp.wav"
KAGE_API_KEY = os.getenv("KAGE_API_KEY", "").strip()

# Détection de voix
BLOCK_MS = 50
SILENCE_AFTER_SPEECH = 0.7   # traite après 0,7 s de silence
MAX_RECORD_SECONDS = 12
PRE_ROLL_SECONDS = 0.30

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
WAITING_REPLY = "Let me think for a second."


def speak(text):
    if not text:
        return
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
            sd.play(samples, samplerate=16000, blocking=True)
            return
    except Exception as exc:
        print(f"Kokoro unavailable, Windows voice fallback: {exc}")
    tts.say(text)
    tts.runAndWait()


# ---------- MICRO ----------

def rms(block):
    return float(np.sqrt(np.mean(np.square(block))))


def record_until_silence():
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
        total_time = 0.0

        while total_time < MAX_RECORD_SECONDS:
            block, overflowed = stream.read(block_size)

            level = rms(block)
            total_time += BLOCK_MS / 1000

            if not speech_started:
                pre_roll.append(block.copy())

                if level >= speech_threshold:
                    speech_started = True
                    print("🟢 Parole détectée")

                    frames.extend(list(pre_roll))
                    frames.append(block.copy())

            else:
                frames.append(block.copy())

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

print("\nKage Voice prêt.")

while True:
    choice = input(
        "\nEntrée = parler | q = quitter : "
    )

    if choice.lower() == "q":
        break

    try:
        recorded = record_until_silence()

        if not recorded:
            print("❌ Aucune parole détectée.")
            continue

        text = transcribe()

        if not text:
            print("❌ Je n'ai pas compris.")
            continue

        print(f"📝 Entendu : {text}")

        # Keep listening for the backend response while allowing a short,
        # natural acknowledgement if a conversational answer takes longer.
        request_future = request_executor.submit(send_to_kage, text)
        try:
            result = request_future.result(timeout=3.0)
        except TimeoutError:
            speak(WAITING_REPLY)
            result = request_future.result(timeout=60.0)

        print(f"🤖 Kage : {result['reply']}")
        print(f"🎭 Commande : {result['command']}")
        print(f"🔢 Séquence : {result['sequence']}")

        # Kage répond à voix haute
        speak(result["reply"])

    except Exception as e:
        print(f"❌ Erreur : {e}")


if os.path.exists(WAV_FILE):
    os.remove(WAV_FILE)
