import json
import os
import time
import urllib.request
import uuid
from dataclasses import asdict, dataclass
from pathlib import Path

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
    confidence_usable: bool = False
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
        # Match the actual live client unless the operator explicitly opts in
        # to a larger model. The M920q currently runs the cached base model.
        self.whisper_model_name = os.getenv("KAGE_WHISPER_MODEL", "base").strip() or "base"
        self.whisper_compute_type = os.getenv("KAGE_WHISPER_COMPUTE", "int8").strip() or "int8"
        self.whisper_beam_size = int(os.getenv("KAGE_WHISPER_BEAM", "1"))
        self.whisper_cpu_threads = int(os.getenv("KAGE_WHISPER_CPU_THREADS", "0"))
        self.whisper_num_workers = int(os.getenv("KAGE_WHISPER_NUM_WORKERS", "1"))
        self.whisper_prompt = _env_bool("KAGE_WHISPER_PROMPT", self.language.lower().startswith("fr"))
        self.allow_fallback = _env_bool("KAGE_STT_FALLBACK", True)
        # An explicit empty Nemotron transcript means its VAD found no speech.
        # Do not turn noise into a Whisper hallucination unless the operator
        # explicitly opts back into this risky fallback.
        self.fallback_on_empty = _env_bool("KAGE_STT_FALLBACK_ON_EMPTY", False)
        # NeMo-Speech.cpp's greedy RNNT confidence is currently a constant
        # 1.0, so it must not drive fallback decisions by default.
        self.confidence_usable = _env_bool("KAGE_NEMO_CONFIDENCE_USABLE", False)
        self.min_confidence = float(os.getenv("KAGE_STT_MIN_CONFIDENCE", "0.72"))
        # Docker Desktop occupies 8080 on the M920q; NeMo-Speech uses the
        # dedicated local port 18080 instead.
        self.nemo_url = os.getenv("KAGE_NEMO_URL", "http://127.0.0.1:18080").rstrip("/")
        self.nemo_read_timeout = float(os.getenv("KAGE_NEMO_READ_TIMEOUT", "20"))
        self._whisper_model = None
        default_terms = "Kagé,Kage,ChatGPT,Qwen,Nextcloud,Angry,Dizzy,plafonnier,luminosité,calendrier"
        self.context_terms = [
            item.strip()
            for item in os.getenv("KAGE_STT_CONTEXT", default_terms).split(",")
            if item.strip()
        ]

        # Preserve the old production startup behaviour when rollback forces
        # Whisper: load it before the first utterance instead of adding a cold
        # model-load penalty to the first request.
        if self.engine in {"whisper", "auto"}:
            self._load_whisper()

    def describe(self):
        return {
            "engine": self.engine,
            "language": self.language,
            "whisper_model": self.whisper_model_name,
            "whisper_compute": self.whisper_compute_type,
            "whisper_beam": self.whisper_beam_size,
            "whisper_cpu_threads": self.whisper_cpu_threads,
            "whisper_num_workers": self.whisper_num_workers,
            "whisper_prompt": self.whisper_prompt,
            "fallback": self.allow_fallback,
            "fallback_on_empty": self.fallback_on_empty,
            "nemotron_url": self.nemo_url,
            "confidence_fallback_enabled": self.confidence_usable,
            "min_confidence": self.min_confidence if self.confidence_usable else None,
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
                cpu_threads=self.whisper_cpu_threads,
                num_workers=self.whisper_num_workers,
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
            with urllib.request.urlopen(f"{self.nemo_url}/ready", timeout=0.7) as response:
                data = json.loads(response.read().decode("utf-8"))
            return bool(data.get("ready", True))
        except Exception:
            return False

    def _transcribe_whisper(self, wav_path):
        started = time.perf_counter()
        model = self._load_whisper()
        language = self._whisper_language()

        if language == "fr" and self.whisper_prompt:
            initial_prompt = (
                "Le robot s'appelle Kagé. L'utilisateur parle français. "
                "Transcris fidèlement le français et conserve les noms Kagé, ChatGPT, Qwen, "
                "Nextcloud, Angry et Dizzy."
            )
        else:
            initial_prompt = None

        segments, _ = model.transcribe(
            str(wav_path),
            language=language,
            vad_filter=True,
            beam_size=self.whisper_beam_size,
            best_of=1,
            temperature=0,
            without_timestamps=True,
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

        boundary = f"----KageSTT{uuid.uuid4().hex}"
        chunks = []
        fields = {
            "model": "default",
            "language": self._nemotron_language(),
            "response_format": "verbose_json",
            "automatic_punctuation": "true",
            "speech_contexts": json.dumps(speech_contexts, ensure_ascii=False),
        }
        for name, value in fields.items():
            chunks.append((
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n"
                f"{value}\r\n"
            ).encode("utf-8"))
        chunks.append((
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
            f"filename=\"{Path(wav_path).name}\"\r\nContent-Type: audio/wav\r\n\r\n"
        ).encode("utf-8"))
        chunks.append(Path(wav_path).read_bytes())
        chunks.append(f"\r\n--{boundary}--\r\n".encode("ascii"))
        request = urllib.request.Request(
            f"{self.nemo_url}/v1/audio/transcriptions",
            data=b"".join(chunks),
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=self.nemo_read_timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
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
            confidence_usable=self.confidence_usable and confidence is not None,
        )

    def transcribe(self, wav_path):
        wav_path = Path(wav_path)
        if not wav_path.exists():
            raise FileNotFoundError(wav_path)

        use_nemotron = self.engine in {"auto", "nemotron"}
        primary_error = ""

        if use_nemotron:
            try:
                result = self._transcribe_nemotron(wav_path)
                low_confidence = (
                    result.confidence_usable and result.confidence < self.min_confidence
                )
                if result.text and not low_confidence:
                    return result
                primary_error = (
                    "empty transcript"
                    if not result.text
                    else f"confidence {result.confidence:.3f} < {self.min_confidence:.3f}"
                )
                if (not self.allow_fallback or
                        (not result.text and not self.fallback_on_empty)):
                    return result
            except Exception as exc:
                primary_error = f"{type(exc).__name__}: {exc}"
                if self.engine == "nemotron" and not self.allow_fallback:
                    raise
        result = self._transcribe_whisper(wav_path)
        if use_nemotron:
            result.fallback_used = True
            result.primary_engine = "nemotron"
            result.primary_error = primary_error or "Nemotron server unavailable"
        return result
