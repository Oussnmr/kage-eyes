# Kagé web-progressive-v3 benchmark

Date: 2026-09-27  
Model: `gpt-5.6-luna`, effort `low`  
Web context: `low`

## Baseline observed in the live voice log

Prompt: `Combien coûte un iPhone 13, reconditionné en ligne ?`

- First model delta: 7,853 ms
- First complete sentence: 8,299 ms
- First useful audio: 15,752 ms
- Generation complete: 11,023 ms
- Playback complete: 83,862 ms
- First sentence length: 22 words before its first full stop

## v3 isolated comparison

The same prompt was tested both with a separate DuckDuckGo snapshot on the
critical path and with one focused native Web search.

| Foreground strategy | Extra search before Codex | Codex first delta | Codex complete | Native searches |
|---|---:|---:|---:|---:|
| DDGS snapshot + Codex | 2,870 ms | 7,840 ms | 9,166 ms | 0 |
| Focused native search | 0 ms | 8,508 ms | 10,370 ms | 1 |

The native strategy was retained because its effective first-text time was
about 2.2 seconds lower and its answer contained stronger direct citations.

## TTS measurement on the live warm Kokoro endpoint

| English Puck input | Synthesis time | PCM bytes |
|---|---:|---:|
| Five words | 2,394 ms | 57,344 |
| Previous long first-sentence shape | 19,673 ms | 398,678 |

The v3 prompt requires a five-word first sentence and the voice client also
enforces a five-word first chunk. Spoken output is capped at three sentences.
The expected first useful Web audio is therefore around 11 seconds under the
measured load, versus 15.7 seconds in the supplied baseline log. Actual live
microphone timing must still be confirmed after deployment.

## Background enrichment

At the first model text delta, v3 starts an independent DDGS search for five
results and fetches up to two pages. It does not speak this research. The most
recent result is cached for 15 minutes and reused when the user says a short
continuation such as `continue`, `donne-moi plus d'informations`, or
`tell me more`.
