# Kage Eyes

Kage Eyes is a small, first firmware for the Waveshare ESP32-S3 Touch AMOLED
1.8 (V1 and V2). It keeps Pocket Tank's tested power sequencing, display
driver, board-revision detection, touch driver, ESP-IDF configuration and web
flashing infrastructure. It contains no aquarium, LLM, model partition,
network, audio or motor code.

## What it does

- two cyan, softly glowing eyes on a true-black AMOLED background;
- normal, happy, angry, surprised, sleepy, look-left and look-right states;
- a screen tap advances to the next state;
- a random, non-blocking blink while normal;
- 220 ms geometry transitions, updated at about 60 frames per second.

## Build

Install ESP-IDF 5.4.1, export its environment, then run:

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

`firmware/main/eyes/eyes.c` contains the complete expression state machine and
renderer. `main.c` owns the non-blocking display loop. Hardware remains in the
small display and touch ports retained from Pocket Tank.

Pocket Tank is MIT licensed; Kage Eyes retains the required upstream license.
