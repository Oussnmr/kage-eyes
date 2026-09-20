# Current status

## Objective

Reduce perceived latency while preserving the functioning Kage Eyes firmware and keeping local operation free of recurring cost.

## Current architecture

- ESP32-S3 Waveshare Kage Eyes firmware in `C:\Kage\kage-eyes`.
- Backend: `C:\Kage\app.py`, FastAPI/Uvicorn on port 8000.
- Audio path: PCM upload to `/audio`, then Whisper transcription, synchronous Ollama generation, synchronous `pyttsx3` playback.
- STT: `faster-whisper` model `small`, CPU INT8, French hint.
- LLM: Ollama `qwen3:4b-instruct-2507-q4_K_M`, 2.5 GB, currently used with `stream:false`.
- ESP32 firmware repository is clean on branch `main`, commit `88241e0`.

## Completed

- Read the project context and technical brief.
- Authorized PC inventory completed.
- Confirmed backend and Ollama listeners.
- Confirmed installed Ollama model and capabilities.
- Created backend rollback copy: `app_before_telemetry_20260919_165644.py`.
- Added non-invasive request telemetry to `app.py` for audio requests.

## Current measurements

- Ollama direct short French generation: about 5.64 s total in the warm direct test.
- Authenticated `/ask` after restart: 6.28 s total, with instrumented LLM time 6.24 s.
- After explicitly adding `think:false`, the same short question measured 6.45 s total / 6.38 s instrumented LLM time; this did not produce a material improvement, so the main cost is CPU token generation rather than visible reasoning.
- Authenticated simple `Kage, stop.` request: 4.73 s total, with instrumented LLM time 4.73 s; it was routed through Ollama and returned `idle`, confirming the deterministic router is currently absent and the stop semantics are incomplete.
- Deterministic router added for supported direct commands. `Kage, stop.` now returns in about 61 ms and bypasses Ollama; a concurrent conversational request still uses Ollama.
- `stop` now returns in about 34 ms and is marked `speak:false`, so it does not start a new TTS response.
- Ollama inference portion in the direct test: about 5.46 s for 39 output tokens.
- Existing endpointing in firmware context is not yet measured end-to-end.
- Full request baseline is pending backend restart and authenticated audio samples.

## Decisions made

- Deterministic router is the highest-priority latency change for simple commands.
- Do not replace the local model before measuring warm inference and disabling unnecessary thinking.
- Do not use ChatGPT/Codex subscription as an assumed server API backend.
- Preserve the Kage Eyes display/QSPI/DMA implementation.

## Files modified

- `C:\Kage\app.py`: request timing logs added.
- `C:\Kage\app.py`: first deterministic direct-command router added.
- `C:\Kage\app.py`: stop intent made silent for TTS.
- `C:\Kage\app.py`: explicit `think:false` added to normal Ollama generation.
- `C:\Kage\KAGE_WORK_STATUS.md`: this checkpoint.

## Services installed/configured

- Existing Ollama installation with `qwen3:4b-instruct-2507-q4_K_M`.
- Existing FastAPI/Uvicorn process on port 8000.

## Tests passed

- Ollama `/api/tags` responded successfully.
- Direct Ollama generation responded in French.

## Tests failed

- Python executable validation from the restricted shell was blocked by the environment.
- Authenticated end-to-end baseline is pending service restart.

## Known problems

- Current backend generates non-streaming LLM output and speaks synchronously with `pyttsx3`.
- Known commands are currently embedded in the LLM prompt rather than bypassing the model.
- ESP32 firmware repository contains the microphone meter UI but the current backend audio integration is not yet traced end-to-end.

## Current blockers

- A real `/audio` baseline still needs a representative microphone recording; no recording was present in `C:\Kage`.
- Live Kage audio baseline received remotely from `188.189.16.126`: `Kage, stop.` request `cc6755f59295` had STT 3517.2 ms, direct route 0.1 ms, total 3517.5 ms.
- Live Kage audio baseline received remotely: `Kage, mets-toi en colère.` request `c4beb3a07a74` had STT 3367.1 ms, direct route 0.1 ms, total 3876.0 ms; TTS/action tail was about 509 ms.

## Current model recommendation

- Luna, low reasoning: sufficient for inventory, telemetry, benchmarks, and mechanical implementation.

## Exact next step

Restart the existing backend without changing its behavior, verify telemetry output, then run representative authenticated `/audio` samples and record stage timings before implementing the deterministic router.

## Latest update

- Whisper `base` reduced live STT to about 1.07 s on the recent remote commands.
- The phrase for returning to normal was initially missed and fell through to Ollama, causing about 9.15 s backend time.
- Angry and return-to-normal phrase variants are now matched locally.
- Current direct physical commands are silent (`speak:false`) so TTS no longer delays ESP32 command polling.
- Backend telemetry now records PCM byte count and clip duration for each `/audio` request, allowing endpointing/capture delay to be separated from STT compute time.
- Latest live base measurements: angry clip 2.224 s / STT 1072.5 ms / total 1072.9 ms; return-normal clip 2.688 s / STT 1104.8 ms / total 1105.1 ms. Neither used Ollama.
- Remaining perceived ~10 s delay is before backend request arrival; current checkout lacks the deployed voice bridge, so firmware endpointing/upload timing is not yet editable from this repository.
- Git references were refreshed and `origin/main` contains the deployed voice bridge; local checkout is now fast-forwarded to `d7bc010`.
- Firmware change prepared but not flashed: parse the `/audio` response and dispatch its command immediately, avoiding the remote `/command/latest` polling wait of up to 2.5 s.
- ESP-IDF 5.5.5 is now installed locally under `C:\Users\Oussama\Documents\Codex\esp-idf-v5.5.5`; required submodules were initialized.
- Firmware build passed for ESP32-S3. Generated image: `C:\Kage\kage-eyes\firmware\build\kage_eyes.bin` (0x18d7d0 bytes, 61% free in the app partition).
- One link issue in the first build was fixed by exposing the bridge command dispatcher outside its private namespace; the incremental rebuild passed.
- No firmware flash has been performed. OTA/USB flash path still needs verification and explicit approval before use.
- Exact next step: inspect the firmware OTA/update path and, if reachable, prepare a reversible OTA deployment plan; otherwise wait for USB access.

### Controlled latency measurement 2026-09-19 18:54 UTC

- Command: `Kage met toi en colère.`
- Request ID: `ae55d58c36ea`
- Captured audio: 1.952 s / 62,464 PCM bytes.
- STT: 1,055.7 ms.
- Deterministic route: 0.1 ms; Ollama was not used.
- Backend total after `/audio` request arrival: 1,055.9 ms.
- The remaining end-of-speech-to-action delay is before backend request arrival (ESP32 endpointing/upload and transport); this backend log cannot measure that segment yet.

### Firmware timing sample after instrumentation

- User command: `Kage met toi en colère.`
- Firmware event log: capture/endpoint segment `1815 ms`.
- Firmware event log: upload + response segment `4097 ms`.
- Backend telemetry for the same request: STT `1065.9 ms`, deterministic route `0.1 ms`, backend total `1066.0 ms`, audio clip `2.176 s`.
- Therefore the dominant remaining delay is approximately `3031 ms` inside ESP32 network/upload coordination or transport around the backend call, not STT or LLM routing.

### Codex subscription backend feasibility probe

- `codex login status` confirms the PC is authenticated through ChatGPT, without an API key.
- One controlled `codex exec --model gpt-5.6-luna` request returned `bonjour` successfully.
- End-to-end process time was 5,450 ms and Codex reported 2,444 tokens for the trivial request.
- Conclusion: ChatGPT/Codex entitlement is technically reachable, but spawning `codex exec` per conversation is unsuitable for Kagé due to agent startup overhead and quota consumption. A persistent Codex app-server benchmark remains the only viable Codex-based path to evaluate.

### Persistent Codex App Server probe

- A single persistent `codex app-server` process was exercised through its supported local protocol with `gpt-5.6-luna`, no reasoning, and the existing ChatGPT/Codex login. No separately billed API was used.
- First turn: first usable text at `1,616.0 ms`; complete turn at `3,365.9 ms`.
- Second turn in the same session: first usable text at `334.7 ms`; complete turn at `2,036.4 ms`.
- Decision: a persistent Codex adapter is technically promising for conversational fallback, but it is not ready to become the default backend until it is tested with realistic short French questions, timeouts, reconnects, and a real TTS output path.

### Waveshare speaker and microphone reliability review

- The current backend only invokes `pyttsx3` on the M920q. No audio generated by the backend is transferred to the Waveshare speaker yet.
- The Waveshare BSP supports ES8311 speaker output, so no additional hardware is assumed necessary. A backend-to-ESP audio protocol and an ESP playback task must be added before Kagé can speak through the device.
- Voice capture runs in a single microphone task. Once an utterance ends, its synchronous HTTP upload blocks microphone reads until the backend returns. This explains temporary frozen level display while a request is processing.
- A lasting `STARTING`/`LISTENING 0%` state is not recovered by the current implementation if `esp_codec_dev_read` stalls: there is no read watchdog or codec recovery path. The timestamp instrumentation did not alter microphone capture or gain.

## Exact next step

Implement a small microphone health watchdog with explicit `PROCESSING` state and recoverable codec reinitialization; validate it with the device logs. Then add a minimal speaker hardware playback test before choosing and integrating the French TTS engine.

### Microphone recovery and Waveshare speaker hardware test (firmware `4e4fefe`)

- `MicMeterState::Processing` now makes the microphone screen distinguish a normal backend wait from listening. It is entered only after endpointing and before the synchronous upload.
- The ESP-IDF codec driver uses a finite 1 s I2S read timeout. A failed read now closes and deletes the failed microphone handle, waits 250 ms, and reopens it in the same task. This provides recovery for the observed `LISTENING/STARTING 0%` failure mode when the driver reports a timeout/error.
- System now has a `TEST SPEAKER` button. It locally plays a short 660 Hz, 220 ms confirmation tone through the ES8311 speaker. It does not use Wi-Fi or alter saved Wi-Fi profiles.
- Firmware build passed: `kage_eyes.bin` is 0x192430 bytes, with 60% app-partition space free.
- Commit `4e4fefe Recover microphone reads and add speaker test` was pushed to `main`. GitHub Actions deployment `35466242093` completed successfully. The public OTA image now reports `1,647,664` bytes (matching the local build) and was last modified at `2026-09-19 20:08:48 UTC`.

## Exact next step

Update through Waveshare System → UPDATE FIRMWARE. Then press TEST SPEAKER once and report whether the short tone is audible, and reproduce a microphone request once to confirm `PROCESSING` / recovery logs.

### Hardware feedback

- User confirms the microphone test now works after firmware `4e4fefe`.
- Speaker DMA-close correction `7e13b0e` is published successfully. OTA image size is `1,647,696` bytes and was updated at `2026-09-19 20:22:52 UTC`.
- User confirms the Waveshare speaker test is audible. Microphone capture/recovery and speaker hardware output are both validated.

### Next phase

Replace the local confirmation tone with a real backend-generated audio stream, starting with a bounded WAV/PCM response and then adding chunked playback. Preserve `STOP` interruption and keep the existing microphone/network state reporting.

### TTS architecture decision (2026-09-19)

- Primary TTS candidate selected: local Kokoro‑82M with French voice `ff_siwis`. It is Apache‑2.0, has a dedicated French voice, and supports incremental generation. It is not installed on the M920q yet.
- Piper remains the fallback benchmark: smaller and simpler ONNX deployment, but expected to be less natural in French.
- Wire protocol: conversational `/audio` responses will use `application/octet-stream` PCM (`16 kHz`, `mono`, signed 16-bit) with command/sequence metadata in HTTP headers. ESP32 receives chunks in the HTTP callback and feeds a dedicated speaker queue; playback starts after a short prebuffer rather than after the complete response.
- The backend will no longer run `pyttsx3` for Waveshare replies. Direct physical commands remain silent and immediate. Conversational replies produce PCM for the Waveshare speaker.
- `STOP` will clear the speaker queue, abort the active HTTP audio request, and return microphone state to listening. This is the first interruption implementation; full acoustic echo cancellation is deferred until streaming playback is stable.

## Current model recommendation

- Luna, low reasoning: sufficient from here for dependency installation, Kokoro/Piper latency benchmarks, endpoint implementation, firmware queue plumbing, builds, and device tests. Escalate only if a real I2S/HTTP concurrency fault appears.

### Kokoro installation and backend endpoint

- Installed `kokoro-onnx 0.6.1`, `soundfile`, and required local phonemizer packages into `C:\Kage\venv`. No paid service or API was added.
- Downloaded Kokoro ONNX model and voice pack into `C:\Kage\models\kokoro`.
- Warm local French benchmark with `ff_siwis`: first audio `800–1466 ms` for tested phrases, real-time factor `0.51–0.58`; cold first phrase was `3486 ms` after a `2922 ms` model load.
- Added `C:\Kage\tts_service.py` and a protected `POST /speech` endpoint in `C:\Kage\app.py`. It streams signed 16-bit mono PCM at 16 kHz and keeps Kokoro loaded in memory.
- Restarted the live backend on port 8000; Uvicorn reports application startup complete. Firmware speaker transport is not connected yet.

### Codex App Server bridge — 2026-09-20

- Added `C:\Kage\codex_bridge.py`: a persistent, local stdio bridge to the installed Codex App Server.
- Authentication remains the existing ChatGPT/Codex login. No OpenAI API key, network listener, secret copy, or paid API configuration was added.
- The bridge discovers enabled plugins at startup and disables all 22 for the Kagé thread, preventing their global skills/MCP context from being injected.
- Kagé uses `gpt-5.6-luna` with low reasoning and the default service tier for Codex conversations. No Fast service tier is used.
- The selected backend is persisted safely in `C:\Kage\kage_settings.json`; `KAGE_CONVERSATION_BACKEND` is used only as the first-run default. No secrets are stored in that file.
- Spoken commands `Kage, switch to local` and `Kage, switch to ChatGPT` update the setting immediately and confirm it aloud. Direct physical commands always run before either backend.
- Validation: Codex bridge startup 1.57 s; first text 2.26 s; full short answer 2.55 s. Direct `can you be angry` route remained local at 293 ms end-to-end.
- Backend was securely restarted with the existing `KAGE_API_KEY` user environment variable and Codex selected.

## Exact next step

Test the PC-local wake word `Kage` with the user. It uses PocketSphinx keyword spotting and no audio leaves the PC until wake detection. Then tune sensitivity from real results before adding interruption while Kagé is speaking.

### PC wake word prototype — 2026-09-20

- Installed the free local `pocketsphinx 5.0.4` package. It has an included English acoustic model and does not require an account, access key, cloud request, or payment.
- `C:\Kage\voice.py` now starts in wake-word mode by default. It waits locally for English-pronounced `Kage` (detected as phonetic `cage`), says “I’m listening”, then records and processes the following command.
- `KAGE_WAKE_WORD=0` restores the previous Enter-to-speak mode if required for debugging.
- Microphone initialization passed. Real false-positive/false-negative testing is pending.

### Follow-up conversation window — 2026-09-20

- After wake detection, Kagé now keeps the PC microphone session open for 25 seconds after every answer. A follow-up does not require saying `Kage` again.
- If no speech is detected during that window, Kagé returns silently to wake-word mode. The timeout is configurable with `KAGE_FOLLOW_UP_TIMEOUT`; its default is 25 seconds.

### Conversation polish — 2026-09-20

- Waiting acknowledgements now rotate through ten short English phrases, including a soft “Hmm...”, and may remain silent for short inputs. This avoids filling every small interaction with speech.
- Added local end-session phrases such as `stop listening`, `that's all`, `we're done`, `goodbye`, and `go back to sleep`.
- Expanded English direct command variants for idle, blink, sleep, angry, and dizzy so these continue to bypass Codex and Ollama.
- TTS remains local Kokoro `am_adam`; Codex supplies text only.
- Added many session-stop variants, including `stop`, `stop listening`, `stop talking`, `that's enough`, `quiet`, `goodbye`, and `go back to sleep`.
- Session closure now uses a randomized helpful phrase containing the name `Kagé` where appropriate. The wake acknowledgement also says `Kagé is listening`.
- The detector still uses the English phonetic form `cage` internally because PocketSphinx matches the sound; user-facing TTS text uses `Kagé`.
