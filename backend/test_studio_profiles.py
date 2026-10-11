"""Checks for the bounded runtime face and motion studio protocol."""

import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from fastapi import HTTPException

sys.path.insert(0, str(Path(__file__).parent))
import studio_profiles as studio


class StudioTests(unittest.TestCase):
    def test_default_profiles_validate(self):
        for face in studio.FACE_DEFAULTS.values():
            self.assertEqual(studio.validate_face(face)["name"], face["name"])
        for motion in studio.MOTION_DEFAULTS.values():
            self.assertEqual(studio.validate_motion(motion)["name"], motion["name"])

    def test_rejects_out_of_range_values(self):
        with self.assertRaises(HTTPException):
            studio.validate_face({**studio.DEFAULT_FACE, "x": 500})
        motion = {**studio.MOTION_DEFAULTS["dance"], "steps": [
            {**studio.DANCE_STEPS[0], "left": 101}]}
        with self.assertRaises(HTTPException):
            studio.validate_motion(motion)

    def test_one_command_contains_full_sequence_and_interrupts(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(studio, "FACE_PATH", Path(directory) / "faces.json"), \
             patch.object(studio, "MOTION_PATH", Path(directory) / "motions.json"):
            state = {"command": "idle"}
            sent = []

            def send(command):
                sent.append(command)
                state["command"] = command

            profiles = studio.Studio(send, lambda: state)
            profiles.run("explore")
            self.assertEqual(len(sent), 1)
            self.assertTrue(sent[0].startswith("studioseq:"))
            packet = bytes.fromhex(sent[0][10:])
            self.assertEqual(len(packet),
                             (len(studio.MOTION_DEFAULTS["explore"]["steps"]) + 1) * 7)
            self.assertTrue(any(packet[i + 2] == 127 for i in range(0, len(packet), 7)))
            self.assertTrue(any(packet[i + 3] == 127 for i in range(0, len(packet), 7)))
            profiles.stop()
            time.sleep(0.02)
            self.assertEqual(sent[-1], "motion_stop")
            self.assertGreater(profiles.face_status()["face_rev"], 0)


if __name__ == "__main__":
    unittest.main()
