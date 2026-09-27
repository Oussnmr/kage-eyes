"""Run with: python -m unittest backend/test_command_intents.py"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from command_intents import direct_intent, home_intent, session_intent  # noqa: E402


class CommandIntentTests(unittest.TestCase):
    def test_robot_in_french_and_english(self):
        cases = {
            "Kagé, mets-toi en colère.": ("robot", "angry"),
            "Kagé, retourne à ton état normal.": ("robot", "idle"),
            "Please blink your eyes": ("robot", "blink"),
            "Retourne dormir": ("robot", "sleep"),
        }
        for phrase, expected in cases.items():
            with self.subTest(phrase=phrase):
                self.assertEqual(direct_intent(phrase), expected)

    def test_backend_in_french_and_english(self):
        for phrase in ("Passe sur ChatGPT", "Use ChatGPT"):
            self.assertEqual(direct_intent(phrase), ("backend", "codex"))
        for phrase in ("Passe en local", "Use Ollama"):
            self.assertEqual(direct_intent(phrase), ("backend", "ollama"))

    def test_home_in_french_and_english(self):
        cases = {
            "Allume la lumière du bureau": ("devices", ("light",), True),
            "Turn on the light": ("devices", ("light",), True),
            "Éteins les LED": ("devices", ("leds",), False),
            "Turn off the LED lights": ("devices", ("leds",), False),
            "Kill everything": ("devices", ("light", "lamp", "projector", "leds", "desk"), False),
            "Ferme tout": ("devices", ("light", "lamp", "projector", "leds", "desk"), False),
            "Night shift": ("profile", "nightshift"),
            "Passe en mode nuit": ("profile", "nightshift"),
            "Mode repos": ("profile", "rest"),
        }
        for phrase, expected in cases.items():
            with self.subTest(phrase=phrase):
                self.assertEqual(home_intent(phrase), expected)

    def test_session_ending(self):
        self.assertEqual(session_intent("Close the shop"), "shutdown")
        self.assertEqual(session_intent("Retourne dormir"), "end")
        self.assertIsNone(session_intent("Ferme tout"))

    def test_no_false_commands(self):
        for phrase in (
            "Pourquoi le ciel est bleu ?", "What is night shift?",
            "Je suis en colère aujourd'hui", "Je veux savoir comment allumer la lumière",
        ):
            with self.subTest(phrase=phrase):
                self.assertIsNone(direct_intent(phrase))
                self.assertIsNone(home_intent(phrase))


if __name__ == "__main__":
    unittest.main()
