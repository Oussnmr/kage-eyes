# Kage M920q backend

This folder mirrors the FastAPI service used on the Windows M920q.

The ESP32 voice firmware sends raw **16 kHz / mono / signed 16-bit little-endian PCM**
to `POST /audio`. The PC microphone client in `voice.py` uses the same backend
and sends its authenticated text requests to `POST /ask`. The backend:

1. transcribes with faster-whisper `small` in English;
2. recognizes the name Kage and direct English commands without an LLM;
3. sends the text to the local Ollama Qwen model;
4. updates the existing robot command state;
5. streams English Kokoro speech (`am_adam`) when requested.

The live copy is expected at `C:\Kage\app.py`.
The PC voice client is `C:\Kage\voice.py`; it reads `KAGE_API_KEY` from the
Windows user environment and never prints or stores the key.


## Remote access security

Sensitive endpoints require the `X-Kage-Key` header. The backend reads the
expected 64-character secret from the `KAGE_API_KEY` environment variable.
The secret must never be committed to this repository.

Kage stores the same secret locally in NVS after USB Serial/JTAG provisioning.
Use `tools/provision_kage_key.ps1` from Windows after flashing the firmware.

The firmware prefers the local M920q endpoint on Wi-Fi profile 1 and uses the
HTTPS Funnel endpoint on profile 2. The HTTPS client validates the server
certificate with the ESP-IDF certificate bundle; SNTP is started after Wi-Fi
gets an IP so certificate dates can be checked.
