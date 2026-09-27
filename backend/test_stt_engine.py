"""Run with: python -m unittest backend/test_stt_engine.py"""

import os
import unittest
from pathlib import Path
from unittest.mock import patch

from stt_engine import KageSTT, STTResult


class EmptyNemotronTests(unittest.TestCase):
    def make_engine(self, fallback_on_empty=False):
        engine = KageSTT.__new__(KageSTT)
        engine.engine = "auto"
        engine.allow_fallback = True
        engine.fallback_on_empty = fallback_on_empty
        return engine

    def test_empty_nemotron_does_not_fallback_to_whisper_by_default(self):
        engine = self.make_engine()
        empty = STTResult("", "nemotron", 12.0, language="fr-FR")
        whisper = STTResult("hallucination", "whisper", 20.0, language="fr")
        with patch.object(engine, "_transcribe_nemotron", return_value=empty), \
                patch.object(engine, "_transcribe_whisper", return_value=whisper) as fallback:
            result = engine.transcribe(Path(__file__))
        self.assertEqual(result.engine, "nemotron")
        self.assertEqual(result.text, "")
        fallback.assert_not_called()

    def test_explicit_empty_fallback_opt_in_is_preserved(self):
        engine = self.make_engine(fallback_on_empty=True)
        empty = STTResult("", "nemotron", 12.0, language="fr-FR")
        whisper = STTResult("real speech", "whisper", 20.0, language="fr")
        with patch.object(engine, "_transcribe_nemotron", return_value=empty), \
                patch.object(engine, "_transcribe_whisper", return_value=whisper):
            result = engine.transcribe(Path(__file__))
        self.assertEqual(result.text, "real speech")
        self.assertTrue(result.fallback_used)


if __name__ == "__main__":
    unittest.main()
