"""Small, free web-search adapter used only for current-information requests."""
from __future__ import annotations

import re
from typing import Any

try:
    from ddgs import DDGS
except ImportError:  # pragma: no cover - optional runtime dependency
    DDGS = None


WEB_TRIGGER_RE = re.compile(
    r"\b(?:search the web|search online|web search|look it up online|"
    r"look this up online|check online|use the internet|search the internet|"
    r"find this online)\b",
    re.IGNORECASE,
)


def needs_web_search(message: str) -> bool:
    return bool(WEB_TRIGGER_RE.search(message))


def search_web(message: str) -> dict[str, Any]:
    """Return compact search results; never raises into the voice pipeline."""
    if DDGS is None:
        return {"query": message, "results": [], "error": "search dependency unavailable"}
    try:
        rows = DDGS(timeout=8).text(message, max_results=5)
        results = []
        for row in rows or []:
            title = str(row.get("title", "")).strip()
            url = str(row.get("href", row.get("url", ""))).strip()
            body = str(row.get("body", row.get("snippet", ""))).strip()
            if title and url:
                results.append({"title": title[:180], "url": url[:500], "snippet": body[:500]})
        return {"query": message, "results": results}
    except Exception as exc:
        return {"query": message, "results": [], "error": type(exc).__name__}


def format_web_context(data: dict[str, Any]) -> str:
    results = data.get("results", [])
    if not results:
        return "No web results were available. Say that you could not verify current information."
    lines = ["Web results retrieved just now. Use them cautiously and mention the source URL when useful:"]
    for index, result in enumerate(results, 1):
        lines.append(f"{index}. {result['title']} | {result['url']} | {result['snippet']}")
    return "\n".join(lines)
