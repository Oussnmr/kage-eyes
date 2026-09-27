"""Smoke-test Kage's Codex bridge without starting the live backend."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


DEFAULT_PROMPTS = (
    "Quelle est la capitale de la Belgique ?",
    "Quelles sont les dernières nouvelles concernant Volvo Cars aujourd'hui ?",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--legacy-root",
        help="Run the previous adapter + Codex bridge from this backup directory.",
    )
    parser.add_argument("prompts", nargs="*", default=list(DEFAULT_PROMPTS))
    args = parser.parse_args()
    if args.legacy_root:
        sys.path.insert(0, str(Path(args.legacy_root).resolve()))
        from web_search import format_web_context, needs_web_search, search_web
        from codex_bridge import CodexBridge

        bridge = CodexBridge()
    else:
        sys.path.insert(0, str((Path(__file__).resolve().parents[1] / "backend")))
        from codex_bridge import codex_bridge
        from web_search import needs_web_search

        bridge = codex_bridge
    try:
        for prompt in args.prompts:
            if args.legacy_root:
                web_context = None
                if needs_web_search(prompt):
                    web_context = format_web_context(search_web(prompt))
                result = bridge.ask(prompt, web_context=web_context, timeout=90)
                result["pipeline"] = "legacy-ddgs"
            else:
                explicit_web = needs_web_search(prompt)
                result = bridge.ask(
                    prompt,
                    response_language="fr",
                    web_search_requested=explicit_web,
                    timeout=90,
                )
                result["pipeline"] = "web-progressive-v3"
            print(json.dumps({"prompt": prompt, **result}, ensure_ascii=False))
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
