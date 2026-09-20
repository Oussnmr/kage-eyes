"""Small, free web-search adapter used only for current-information requests."""
from __future__ import annotations

import re
import html
import urllib.request
from html.parser import HTMLParser
from typing import Any
from urllib.parse import urlparse

try:
    from ddgs import DDGS
except ImportError:  # pragma: no cover - optional runtime dependency
    DDGS = None


WEB_TRIGGER_RE = re.compile(
    r"\b(?:search the web|search online|search that up|search this up|"
    r"look it up online|look this up online|check online|use the internet|"
    r"search the internet|find this online|what(?:'s| is) the weather in|"
    r"weather forecast for|temperature in|how much is|what is the price of|"
    r"what's the price of|price of|cost of|worth of|current value of|"
    r"latest news about|today's news about|today news about)\b",
    re.IGNORECASE,
)

SEARCH_PREFIX_RE = re.compile(
    r"^\s*(?:kage\s*)?(?:please\s*)?(?:search(?: the web| online)?|"
    r"look (?:it|this|that) up(?: online)?|check online|use the internet)"
    r"\s*(?:for|about)?\s*",
    re.IGNORECASE,
)


class _PageTextParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.parts: list[str] = []
        self.skip_depth = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in {"script", "style", "noscript", "svg"}:
            self.skip_depth += 1

    def handle_endtag(self, tag: str) -> None:
        if tag in {"script", "style", "noscript", "svg"} and self.skip_depth:
            self.skip_depth -= 1

    def handle_data(self, data: str) -> None:
        if not self.skip_depth:
            text = html.unescape(re.sub(r"\s+", " ", data)).strip()
            if text:
                self.parts.append(text)


def fetch_page_text(url: str) -> str:
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        return ""
    if parsed.hostname.lower() in {"localhost", "127.0.0.1", "::1"}:
        return ""
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "Kage/1.0 (current-information lookup)"},
    )
    try:
        with urllib.request.urlopen(request, timeout=8) as response:
            content_type = response.headers.get("Content-Type", "").lower()
            if "text/html" not in content_type:
                return ""
            raw = response.read(400_000)
        parser = _PageTextParser()
        parser.feed(raw.decode("utf-8", errors="replace"))
        return " ".join(parser.parts)[:3500]
    except Exception:
        return ""


def needs_web_search(message: str) -> bool:
    return bool(WEB_TRIGGER_RE.search(message))


def search_query(message: str) -> str:
    query = SEARCH_PREFIX_RE.sub("", message).lstrip(":, ").strip(" .?!")
    return query if len(query) >= 5 else message


def search_web(message: str) -> dict[str, Any]:
    """Return compact search results; never raises into the voice pipeline."""
    if DDGS is None:
        return {"query": message, "results": [], "error": "search dependency unavailable"}
    try:
        rows = DDGS(timeout=8).text(search_query(message), max_results=5)
        results = []
        for row in rows or []:
            title = str(row.get("title", "")).strip()
            url = str(row.get("href", row.get("url", ""))).strip()
            body = str(row.get("body", row.get("snippet", ""))).strip()
            if title and url:
                results.append({"title": title[:180], "url": url[:500], "snippet": body[:500]})
        for result in results[:2]:
            result["page_text"] = fetch_page_text(result["url"])
        return {"query": message, "results": results}
    except Exception as exc:
        return {"query": message, "results": [], "error": type(exc).__name__}


def format_web_context(data: dict[str, Any]) -> str:
    results = data.get("results", [])
    if not results:
        return "No web results were available. Say that you could not verify current information."
    lines = ["Web results retrieved just now. Use them cautiously and mention the source URL when useful:"]
    for index, result in enumerate(results, 1):
        page = result.get("page_text", "")
        lines.append(f"{index}. {result['title']} | {result['url']} | {result['snippet']}")
        if page:
            lines.append(f"Page content: {page}")
    return "\n".join(lines)
