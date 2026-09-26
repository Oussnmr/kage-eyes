import argparse
import collections
import json
import math
import time
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd


PHRASES = [
    "Kagé, mets-toi en colère.",
    "Kagé, retourne à ton état normal.",
    "Allume la lumière du bureau.",
    "Passe sur ChatGPT.",
    "Quelle est la capitale de la Belgique ?",
    "Explique-moi pourquoi le ciel est bleu.",
    "Est-ce que tu peux baisser un peu la luminosité du plafonnier ?",
    "Demain matin, rappelle-moi de consulter mon calendrier avant de partir au bureau.",
    "J'aimerais que tu m'expliques calmement comment fonctionne un arc-en-ciel, avec un exemple simple.",
]


def resolve_mic_device():
    default_input = sd.default.device[0]
    if isinstance(default_input, int) and default_input >= 0:
        info = sd.query_devices(default_input)
        if info.get("max_input_channels", 0) > 0:
            return default_input
    candidates = [
        (index, info) for index, info in enumerate(sd.query_devices())
        if info.get("max_input_channels", 0) > 0
    ]
    for index, info in candidates:
        name = str(info.get("name", "")).lower()
        if "micro" in name or "mikrofon" in name or "usb" in name:
            return index
    return candidates[0][0] if candidates else None


def rms(block):
    return math.sqrt(float(np.mean(np.square(block, dtype=np.float64))))


def record_phrase(device, silence_seconds, max_seconds):
    sample_rate = 16000
    block_ms = 50
    block_size = int(sample_rate * block_ms / 1000)
    pre_roll = collections.deque(maxlen=int(0.3 * 1000 / block_ms))
    frames = []
    calibration = []
    speech_started = False
    silence = 0.0
    utterance = 0.0

    with sd.InputStream(
        samplerate=sample_rate,
        channels=1,
        dtype="float32",
        blocksize=block_size,
        device=device,
    ) as stream:
        for _ in range(6):
            block, _ = stream.read(block_size)
            calibration.append(rms(block))
        noise_floor = float(np.median(calibration)) if calibration else 0.0
        threshold = max(noise_floor * 3.0, 0.012)
        print(f"  Écoute… (seuil RMS {threshold:.4f})", flush=True)

        deadline = time.monotonic() + 8
        while time.monotonic() < deadline or speech_started:
            block, _ = stream.read(block_size)
            level = rms(block)
            if not speech_started:
                pre_roll.append(block.copy())
                if level >= threshold:
                    speech_started = True
                    frames.extend(pre_roll)
                    frames.append(block.copy())
                    print("  Parole détectée.", flush=True)
            else:
                utterance += block_ms / 1000
                frames.append(block.copy())
                silence = silence + block_ms / 1000 if level < threshold else 0.0
                if silence >= silence_seconds or utterance >= max_seconds:
                    break

    if not speech_started:
        return None
    audio = np.concatenate(frames, axis=0)
    return np.clip(audio * 32767, -32768, 32767).astype(np.int16), sample_rate, silence


def main():
    parser = argparse.ArgumentParser(description="Capture repeatable French STT reference WAV files.")
    parser.add_argument("--output", default=r"C:\Kage\stt_bench_audio")
    parser.add_argument("--silence", type=float, default=0.7)
    parser.add_argument("--max-seconds", type=float, default=15)
    args = parser.parse_args()

    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    device = resolve_mic_device()
    if device is None:
        raise SystemExit("No microphone input device found.")
    device_name = str(sd.query_devices(device)["name"])
    print(f"Microphone: {device} — {device_name}")
    print("Chaque phrase sera conservée en WAV. Entrée démarre la prise; R permet de la refaire.")

    manifest = {
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "microphone_index": device,
        "microphone_name": device_name,
        "endpoint_silence_ms": round(args.silence * 1000),
        "files": [],
    }
    for index, phrase in enumerate(PHRASES, 1):
        while True:
            input(f"\n{index}. {phrase}\nAppuie sur Entrée, puis parle naturellement… ")
            result = record_phrase(device, args.silence, args.max_seconds)
            if result is None:
                print("  Aucune parole détectée; nouvelle tentative.")
                continue
            audio, sample_rate, detected_silence = result
            filename = f"{index:02d}.wav"
            path = output / filename
            with wave.open(str(path), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(sample_rate)
                wav.writeframes(audio.tobytes())
            duration_ms = round(len(audio) * 1000 / sample_rate, 1)
            choice = input(f"  Sauvé {filename} ({duration_ms:.0f} ms). Entrée=valider, R=refaire: ")
            if choice.strip().lower() != "r":
                manifest["files"].append({
                    "file": filename,
                    "text": phrase,
                    "duration_ms": duration_ms,
                    "detected_trailing_silence_ms": round(detected_silence * 1000, 1),
                })
                break

    (output / "references.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"\nCorpus prêt: {output}")


if __name__ == "__main__":
    main()
