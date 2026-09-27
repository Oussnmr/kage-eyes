"""Exercise the live progressive Web flow without printing its API key."""

from __future__ import annotations

import json
import os
import time
import urllib.request
import argparse


BASE_URL = "http://127.0.0.1:8000"


def request(path: str, *, payload: dict | None = None):
    key = os.environ.get("KAGE_API_KEY", "")
    if not key:
        raise RuntimeError("KAGE_API_KEY is unavailable")
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    return urllib.request.Request(
        BASE_URL + path,
        data=data,
        headers={"X-Kage-Key": key, "Content-Type": "application/json; charset=utf-8"},
        method="POST" if payload is not None else "GET",
    )


def stream(message: str) -> dict:
    started = time.perf_counter()
    first_delta_ms = None
    reply = ""
    with urllib.request.urlopen(request("/ask/stream", payload={"message": message}), timeout=90) as response:
        for raw in response:
            event = json.loads(raw.decode("utf-8"))
            if event.get("type") == "delta":
                if first_delta_ms is None:
                    first_delta_ms = round((time.perf_counter() - started) * 1000, 1)
                reply += event.get("text", "")
            elif event.get("type") == "done":
                reply = event.get("result", {}).get("reply", reply)
    return {
        "message": message,
        "first_delta_ms": first_delta_ms,
        "total_ms": round((time.perf_counter() - started) * 1000, 1),
        "reply": reply,
    }


def status() -> dict:
    with urllib.request.urlopen(request("/status"), timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "prompt", nargs="?",
        default="Combien coûte un iPhone 13, reconditionné en ligne ?",
    )
    parser.add_argument("continuation", nargs="?", default="Continue")
    args = parser.parse_args()
    first = stream(args.prompt)
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline and status().get("background_research") == "running":
        time.sleep(0.25)
    second = stream(args.continuation)
    print(json.dumps({
        "pipeline": status().get("web_pipeline"),
        "background_research": status().get("background_research"),
        "first": first,
        "continuation": second,
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
