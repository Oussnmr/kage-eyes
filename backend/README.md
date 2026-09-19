# Kage M920q backend

This folder mirrors the FastAPI service used on the Windows M920q.

The ESP32 voice firmware sends raw **16 kHz / mono / signed 16-bit little-endian PCM**
to `POST /audio`. The backend:

1. transcribes with faster-whisper `small` in French;
2. normalizes common transcriptions of the name Kage ("cagée", "cagé", etc.);
3. sends the text to the local Ollama Qwen model;
4. updates the existing robot command state;
5. optionally speaks the reply through the M920q Windows audio output.

The live copy is expected at `C:\Kage\app.py`.
