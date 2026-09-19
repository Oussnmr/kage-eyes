#pragma once

/* Plays a short, quiet confirmation tone through the onboard ES8311 speaker.
   This is a hardware check only; it does not contact the backend. */
void speaker_test_play();
