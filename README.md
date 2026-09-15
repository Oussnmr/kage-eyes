# Kage Eyes

Kage Eyes is a lightweight touch launcher and robot face for the Waveshare
ESP32-S3 Touch AMOLED 1.8 V2. It uses Waveshare's ESP-IDF board support while
keeping installation as simple as a web page.

## What it does

- icon-only magnetic bubble launcher with seven distinct colours;
- Robot, Microphone, Motion, Display, System, Wi-Fi and Storage apps;
- two solid-cyan eyes and a small mouth on a true-black AMOLED background;
- a 448 × 368 landscape interface;
- automatic left/right landscape rotation when the device is tilted;
- filtered orientation changes to avoid rapid flipping near the threshold;
- smooth startup, stronger random gaze, breathing and natural blinks;
- curious and happy idle poses;
- longer automatic sleep, rising Z marks, automatic wake-up and tap-to-wake;
- five quick taps trigger a red angry face; tap or shake restores normal;
- a green pulse when charging starts;
- a short dizzy reaction after a filtered physical shake;
- one tap blinks; a filtered shake triggers the dizzy reaction;
- swipe up from any app to return home and swipe sideways to browse bubbles;
- a live organic grain orb driven by the ES8311 microphone level;
- resting-pose motion calibration and read-only AXP2101 battery percentage;
- native LVGL shapes and dirty-region redraws instead of a full-screen canvas.

## Build

Install ESP-IDF 5.5.5, export its environment, then run:

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
cd ..
python tools/make_installer.py --build-dir firmware/build --version 0.1.0
```

The installer is generated in `installer/dist/`. Serve it locally with
`python -m http.server -d installer/dist 8765`, or push to GitHub and enable
GitHub Pages with **GitHub Actions** as its source. The included workflow
builds and publishes it automatically after each push to `main`.

Open the hosted page with Chrome or Edge, press **Install Kage Eyes**, select
the Waveshare USB serial device, and choose erase when replacing Pocket Tank.

## Structure

`firmware/main/app_shell.cpp` contains the launcher and hardware apps,
`firmware/main/eyes/robot_eyes.cpp` contains the face, and
`firmware/main/audio/mic_meter.cpp` drives the live microphone meter. The
battery monitor only reads the PMIC; it does not alter charging parameters.

Pocket Tank is MIT licensed; Kage Eyes retains the required upstream license.
