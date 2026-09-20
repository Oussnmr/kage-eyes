import os
from collections.abc import AsyncIterator

import numpy as np

try:
    from kokoro_onnx import Kokoro
except ImportError:
    Kokoro = None


MODEL_PATH = os.getenv("KAGE_KOKORO_MODEL", r"C:\Kage\models\kokoro\kokoro-v1.0.onnx")
VOICES_PATH = os.getenv("KAGE_KOKORO_VOICES", r"C:\Kage\models\kokoro\voices-v1.0.bin")
TARGET_RATE = 16000
DEFAULT_VOICE = os.getenv("KAGE_TTS_VOICE", "am_puck").strip() or "am_puck"


class KokoroService:
    def __init__(self):
        self.engine = None
        if Kokoro is not None and os.path.isfile(MODEL_PATH) and os.path.isfile(VOICES_PATH):
            self.engine = Kokoro(MODEL_PATH, VOICES_PATH)

    @property
    def available(self) -> bool:
        return self.engine is not None

    async def stream_pcm(self, text: str, voice: str = DEFAULT_VOICE) -> AsyncIterator[bytes]:
        if self.engine is None:
            raise RuntimeError("Kokoro TTS indisponible")
        async for samples, sample_rate in self.engine.create_stream(
            text, voice=voice, lang="en-us"
        ):
            audio = np.asarray(samples, dtype=np.float32).reshape(-1)
            if sample_rate != TARGET_RATE:
                target_count = max(1, round(len(audio) * TARGET_RATE / sample_rate))
                positions = np.linspace(0, len(audio) - 1, target_count)
                audio = np.interp(positions, np.arange(len(audio)), audio).astype(np.float32)
            pcm = np.clip(audio * 32767.0, -32768, 32767).astype("<i2")
            if pcm.size:
                yield pcm.tobytes()


kokoro_service = KokoroService()
