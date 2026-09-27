# Kagé web pipeline comparison

Version under test: `web-native-v2`  
Runtime and model: Windows, Python 3.12, Codex App Server, `gpt-5.6-luna`, low effort.  
Measurements cover the Codex request path; they exclude microphone capture, STT, endpoint silence, and TTS.

| Request | Pipeline | First final text | Request complete | Search behavior |
|---|---|---:|---:|---|
| Quelle est la capitale de la Belgique ? | legacy | 3.10 s | 3.29 s | no search |
| Quelle est la capitale de la Belgique ? | web-native-v2 | 3.04 s | 3.39 s | no search |
| Tu peux regarder en ligne, c'est quoi les dernières nouvelles pour Volvo Cars ? | legacy DDGS | 3.75 s | 5.90 s | search only because explicitly requested; five result snippets, two page fetches |
| Same Volvo request | web-native-v2 | 14.08 s | 20.28 s | two native live-search calls; sources and publication dates in answer |

The native path is about 0.1 s slower for the stable knowledge question, within run-to-run variation. For this explicit news request, its first final answer arrived about 10.3 s later and completion about 14.4 s later than the legacy path. The native answer contained more dated developments and direct source links. These timings are one-run observations, not a statistical benchmark; source factuality still needs human review.

The legacy router does not search automatically for every current-information question. A separate test of “Qui dirige actuellement Volvo Cars ?” caused the native model to search automatically; legacy routing would not detect that wording.

The native version uses the App Server's live web search in `low` context mode. It leaves stable questions on the normal model path and keeps local DDGS available for the Ollama fallback. Roll back with `tools/rollback_web_pipeline.ps1` and the deployment snapshot path printed by the deploy script.
