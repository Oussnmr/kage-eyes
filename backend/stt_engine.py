import json
import os
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import requests


def _env_bool(name, default):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


@dataclass
class STTResult:
    text: str
    engine: str
    duration_ms: float
    language: str = ""
    confidence: float | None = None
    fallback_used: bool = False
    primary_engine: str = ""
    primary_error: str = ""

    def log_payload(self):
        payload = asdict(self)
        if self.confidence is not None:
            payload["confidence"] = round(self.confidence, 4)
        payload["duration_ms"] = round(self.duration_ms, 1)
        return payload


class KageSTT:
    """Reversible STT adapter for Whisper and local Nemotron."""

    def __init__(self):
        self.engine = os.getenv("KAGE_STT_ENGINE", "auto").strip().lower()
        self.language = os.getenv("KAGE_STT_LANGUAGE", "fr").strip() or "fr"
        self.whisper_model_name = os.getenv("KAGE_WHISPER_MODEL", "small").strip() or "small"
        self.whisper_compute_type = os.getenv("KAGE_WHISPER_COMPUTE", "int8").strip() or "int8"
        self.whisper_beam_size = int(os.getenv("KAGE_WHISPER_BEAM", "3"))
        self.allow_fallback = _env_bool("KAGE_STT_FALLBACK", True)
        self.min_confidence = float(os.getenv("KAGE_STT_MIN_CONFIDENCE", "0.72"))
        self.nemo_url = os.getenv("KAGE_NEMO_URL", "http://127.0.0.1:8080").rstrip("/")
        self.nemo_timeout = float(os.getenv("KAGE_NEMO_TIMEOUT", "30"))
        self._whisper_model = None
        self._session = requests.Session()
        default_terms = "Kagé,Kage,ChatGPT,Qwen,Nextcloud,Angry,Dizzy"
        self.context_terms = [
            item.strip()
            for item in os.getenv("KAGE_STT_CONTEXT", default_terms).split(",")
            if item.strip()
        ]

    def describe(self):
        return {
            "engine": self.engine,
            "language": self.language,
            "whisper_model": self.whisper_model_name,
            "whisper_compute": self.whisper_compute_type,
            "whisper_beam": self.whisper_beam_size,
            "fallback": self.allow_fallback,
            "nemotron_url": self.nemo_url,
            "min_confidence": self.min_confidence,
            "context_terms": self.context_terms,
        }

    def _whisper_language(self):
        value = self.language.lower()
        if value.startswith("fr"):
            return "fr"
        if value.startswith("en"):
            return "en"
        return value.split("-")[0]

    def _nemotron_language(self):
        value = self.language.replace("_", "-")
        if value.lower() == "fr":
            return "fr-FR"
        if value.lower() == "en":
            return "en-US"
        return value

    def _load_whisper(self):
        if self._whisper_model is None:
            from faster_whisper import WhisperModel

            started = time.perf_counter()
            self._whisper_model = WhisperModel(
                self.whisper_model_name,
                device="cpu",
                compute_type=self.whisper_compute_type,
            )
            print(json.dumps({
                "event": "stt_model_loaded",
                "engine": "whisper",
                "model": self.whisper_model_name,
                "duration_ms": round((time.perf_counter() - started) * 1000, 1),
            }, ensure_ascii=False))
        return self._whisper_model

    def nemo_ready(self):
        try:
            response = self._session.get(f"{self.nemo_url}/ready", timeout=0.7)
            if not response.ok:
                return False
            data = response.json()
            return bool(data.get("ready", True))
        except Exception:
            return False

    def _transcribe_whisper(self, wav_path):
        started = time.perf_counter()
        model = self._load_whisper()
        language = self._whisper_language()

        if language == "fr":
            initial_prompt = (
                "Le robot s'appelle Kagé. L'utilisateur parle français. "
                "Transcris fidèlement le français et conserve les noms Kagé, ChatGPT, Qwen et Nextcloud."
            )
        else:
            initial_prompt = (
                "The robot is named Kage. Keep the name pronounced Kage. "
                f"The user speaks {language}."
            )

        segments, _ = model.transcribe(
            str(wav_path),
            language=language,
            vad_filter=True,
            beam_size=self.whisper_beam_size,
            condition_on_previous_text=False,
            initial_prompt=initial_prompt,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()
        return STTResult(
            text=text,
            engine="whisper",
            duration_ms=(time.perf_counter() - started) * 1000,
            language=language,
        )

    def _transcribe_nemotron(self, wav_path):
        started = time.perf_counter()
        speech_contexts = []
        if self.context_terms:
            speech_contexts.append({"phrases": self.context_terms, "boost": 2.5})

        with open(wav_path, "rb") as audio:
            response = self._session.post(
                f"{self.nemo_url}/v1/audio/transcriptions",
                files={"file": (Path(wav_path).name, audio, "audio/wav")},
                data={
                    "model": "default",
                    "language": self._nemotron_language(),
                    "response_format": "verbose_json",
                    "automatic_punctuation": "true",
                    "speech_contexts": json.dumps(speech_contexts, ensure_ascii=False),
                },
                timeout=self.nemo_timeout,
            )
        response.raise_for_status()
        payload = response.json()
        words = payload.get("words") or []
        confidences = [
            float(word["confidence"])
            for word in words
            if isinstance(word, dict) and word.get("confidence") is not None
        ]
        confidence = sum(confidences) / len(confidences) if confidences else None
        return STTResult(
            text=(payload.get("text") or "").strip(),
            engine="nemotron",
            duration_ms=(time.perf_counter() - started) * 1000,
            language=payload.get("language") or self._nemotron_language(),
            confidence=confidence,
        )

    def transcribe(self, wav_path):
        wav_path = Path(wav_path)
        if not wav_path.exists():
            raise FileNotFoundError(wav_path)

        use_nemotron = self.engine in {"auto", "nemotron"}
        primary_error = ""

        if use_nemotron and self.nemo_ready():
            try:
                result = self._transcribe_nemotron(wav_path)
                low_confidence = (
                    result.confidence is not None and result.confidence < self.min_confidence
                )
                if result.text and not low_confidence:
                    return result
                primary_error = (
                    "empty transcript"
                    if not result.text
                    else f"confidence {result.confidence:.3f} < {self.min_confidence:.3f}"
                )
                if not self.allow_fallback:
                    return result
            except Exception as exc:
                primary_error = f"{type(exc).__name__}: {exc}"
                if self.engine == "nemotron" and not self.allow_fallback:
                    raise
        elif self.engine == "nemotron":
            primary_error = "Nemotron server not ready"
            if not self.allow_fallback:
                raise RuntimeError(primary_error)

        result = self._transcribe_whisper(wav_path)
        if use_nemotron:
            result.fallback_used = True
            result.primary_engine = "nemotron"
            result.primary_error = primary_error or "Nemotron server unavailable"
        return result
