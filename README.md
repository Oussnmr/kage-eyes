# Kage Eyes

Kage Eyes is a focused firmware for the Waveshare ESP32-S3 Touch AMOLED 1.8.
It uses Waveshare's ESP-IDF board support for the AMOLED, touch controller and
QMI8658 motion sensor, while keeping installation as simple as a web page.

## What it does

- two cyan, softly glowing eyes on a true-black AMOLED background;
- a 448 × 368 landscape interface;
- automatic left/right landscape rotation when the device is tilted;
- filtered orientation changes to avoid rapid flipping near the threshold;
- normal, happy, sad, angry, sleepy, surprised, thinking and alert states;
- touch controls for expressions, Wi-Fi status and display brightness;
- smooth, non-blocking blinks and interpolated expression transitions.

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

`firmware/main/eyes/robot_eyes.cpp` contains the expression renderer,
`firmware/main/services/orientation_service.cpp` handles the QMI8658 and
`firmware/main/ui/app_ui.cpp` contains the landscape touch interface.

Pocket Tank is MIT licensed; Kage Eyes retains the required upstream license.
