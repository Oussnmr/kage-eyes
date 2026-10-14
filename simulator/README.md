# Kage Typing — LVGL simulator (isolated preview)

This branch adds a **real LVGL 9.6** framebuffer preview, not a second
HTML/SVG/CSS implementation of the face. It is deliberately **not linked into
the running ESP32 firmware** until the user approves the graphics.

## Shared sources

- `typing_scene.hpp/.cpp`: LVGL objects for the keyboard, eyes, mouth and
  three-finger arm. This is the intended reusable component for ESP-IDF.
- `typing_timeline.hpp`: independent C++ animation timing and key queue;
  no key text is stored. Every press raises the arm immediately, even if
  another press just happened. Pending presses animate in order.
- `wasm_bridge.cpp`: LVGL 448x368 RGB565 display with an in-memory framebuffer.
  This is an Emscripten-specific adapter, not a substitute renderer.

The browser shows the returned RGB565 pixels on a canvas. CSS only sizes
the viewport and builds the surrounding buttons.

### States

1. Frame 0: raised hand, behind keyboard.
2. Frame 1: left-hand strike, black negative-space border.
3. Frame 2: centre strike.
4. Frame 3: extended strike toward right-hand keys.

The eyes and mouth keep the float formula from `robot_eyes.cpp`:
`sin(t*2.20)*11 + sin(t*0.65)*2` (scaled by `hover_px/11`).
Eye height also alternates between full and a mild squint, plus natural
blinks.

The main `robot_eyes.cpp` has **not been changed**. Matching its other
behaviours (angry, dance, sleep, custom face overrides, TTS) requires a later
integration pass on hardware.

## Build

Requires Emscripten (emcc), CMake and LVGL **v9.6.0**.

```sh
git clone --depth 1 --branch v9.6.0 https://github.com/lvgl/lvgl.git /tmp/lvgl-9.6
emcmake cmake -S simulator -B build/typing-wasm -DLVGL_SOURCE=/tmp/lvgl-9.6 -DCMAKE_BUILD_TYPE=Release
cmake --build build/typing-wasm --parallel 4
```

This generates `backend/typing_simulator_dist/kage_lvgl.js` and
`backend/typing_simulator_dist/kage_lvgl.wasm`.

For non-ESP native timing tests:

```sh
g++ -std=c++17 -Wall -Wextra simulator/tests/timeline.cpp -o /tmp/kage-timeline
/tmp/kage-timeline
```

The branch CI builds/validates the WASM artifact. The simulator page shows a
clear error and **does not draw a fake HTML alternative** if WASM is missing.

## Web app

- GET `/control/simulator`: simulator + editable pixel coordinates
- GET `/control/simulator/assets/kage_lvgl.{js,wasm}`: generated engine
- GET/PUT/DELETE `/control/simulator/profiles[/<name>]`: protected, server-side JSON

The built-in `default` profile comes directly from the compiled C++ defaults.
Saved profiles live in `typing_profiles.json` next to the deployed
`typing_simulator_routes.py` (or `KAGE_TYPING_PROFILES_PATH`).
A user can undo/redo geometry changes, inspect/copy the C++ initializer,
delete a custom profile, and restore the author's base C++ geometry.

This is a simulation-only frontend. It has **no command endpoint to Kagé**.

## Deploy to M920q

The real server loads `C:\\Kage\\app.py`, not necessarily the Git working copy.
**Do not overwrite that file** from this branch: the handover lists local
uncommitted changes. Use `tools/deploy_typing_simulator.ps1` instead. It
copies only the new isolated files, backs up before modifying `app.py`,
injects only a route-registration line, and leaves the firmware untouched.

Check that the generated artifacts exist, review the script, then run:

```powershell
& "C:\\Kage\\kage-eyes\\tools\\deploy_typing_simulator.ps1" -Install
```

Restart the backend using the existing Kage shortcut *after* checking that
`app.py` still compiles. No restart is triggered by the deployment script.
Rollback: restore the timestamped backup of `C:\\Kage\\app.py` and restart.
