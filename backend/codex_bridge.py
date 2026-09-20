"""Persistent, local-only bridge from Kage to Codex App Server.

The bridge talks to `codex app-server --stdio`, using the existing ChatGPT/Codex
login. It does not use or expose an OpenAI API key and never opens a network port.
"""
from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import threading
import time
from pathlib import Path
from typing import Any, Callable


def _codex_executable() -> str:
    configured = os.getenv("KAGE_CODEX_EXE", "").strip()
    if configured:
        return configured
    from_path = shutil.which("codex")
    if from_path:
        return from_path
    bundled_root = Path(os.getenv("LOCALAPPDATA", "")) / "OpenAI" / "Codex" / "bin"
    candidates = list(bundled_root.glob("*/codex.exe"))
    if candidates:
        return str(max(candidates, key=lambda path: path.stat().st_mtime))
    return ""


SANDBOX_DIR = os.getenv("KAGE_CODEX_SANDBOX", r"C:\\Kage\\kage_codex_sandbox")
MODEL = os.getenv("KAGE_CODEX_MODEL", "gpt-5.6-luna")
EFFORT = os.getenv("KAGE_CODEX_EFFORT", "low")


class CodexBridgeError(RuntimeError):
    pass


class CodexBridge:
    """One persistent App Server process, serialized because it shares a thread."""

    def __init__(self) -> None:
        self._process: subprocess.Popen[str] | None = None
        self._messages: queue.Queue[dict[str, Any]] = queue.Queue()
        self._request_id = 0
        self._thread_id: str | None = None
        self._disabled_plugin_ids: list[str] = []
        self._lock = threading.RLock()
        self._stderr_tail: list[str] = []
        self._started_at: float | None = None

    def _reader(self, stream: Any) -> None:
        for line in iter(stream.readline, ""):
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            self._messages.put(message)

    def _stderr_reader(self, stream: Any) -> None:
        for line in iter(stream.readline, ""):
            line = line.strip()
            if line:
                self._stderr_tail.append(line)
                del self._stderr_tail[:-20]

    def _ensure_process(self) -> None:
        if self._process is not None and self._process.poll() is None:
            return
        executable = Path(_codex_executable())
        if not executable.is_file():
            raise CodexBridgeError("Codex App Server executable is unavailable")
        Path(SANDBOX_DIR).mkdir(parents=True, exist_ok=True)
        self._messages = queue.Queue()
        self._thread_id = None
        self._disabled_plugin_ids = []
        self._stderr_tail = []
        self._started_at = time.perf_counter()
        self._process = subprocess.Popen(
            [str(executable), "app-server", "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
            cwd=SANDBOX_DIR,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        assert self._process.stdout and self._process.stderr
        threading.Thread(target=self._reader, args=(self._process.stdout,), daemon=True).start()
        threading.Thread(target=self._stderr_reader, args=(self._process.stderr,), daemon=True).start()
        self._request("initialize", {
            "clientInfo": {"name": "kage", "version": "1.0"},
            "capabilities": {},
        }, timeout=30)
        self._notify("initialized", {})

    def _send(self, message: dict[str, Any]) -> None:
        if self._process is None or self._process.poll() is not None or self._process.stdin is None:
            raise CodexBridgeError("Codex App Server stopped")
        self._process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
        self._process.stdin.flush()

    def _notify(self, method: str, params: dict[str, Any]) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def _request(self, method: str, params: dict[str, Any], timeout: float) -> dict[str, Any]:
        self._request_id += 1
        request_id = self._request_id
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        deadline = time.monotonic() + timeout
        held: list[dict[str, Any]] = []
        try:
            while time.monotonic() < deadline:
                try:
                    message = self._messages.get(timeout=min(0.25, deadline - time.monotonic()))
                except queue.Empty:
                    continue
                if message.get("id") == request_id:
                    if "error" in message:
                        detail = message["error"].get("message", "request failed")
                        raise CodexBridgeError(f"Codex App Server: {detail}")
                    return message.get("result", {})
                held.append(message)
        finally:
            for message in held:
                self._messages.put(message)
        raise CodexBridgeError(f"Codex App Server timed out during {method}")

    @staticmethod
    def _enabled_plugins(result: dict[str, Any]) -> list[str]:
        ids: list[str] = []
        for marketplace in result.get("marketplaces", []):
            for plugin in marketplace.get("plugins", []):
                plugin_id = plugin.get("id")
                if plugin.get("enabled") and isinstance(plugin_id, str):
                    ids.append(plugin_id)
        return sorted(set(ids))

    def _ensure_thread(self) -> None:
        self._ensure_process()
        if self._thread_id:
            return
        plugins = self._request("plugin/list", {
            "cwds": [SANDBOX_DIR],
            "forceRefetch": False,
        }, timeout=30)
        self._disabled_plugin_ids = self._enabled_plugins(plugins)
        result = self._request("thread/start", {
            "cwd": SANDBOX_DIR,
            "model": MODEL,
            "approvalPolicy": "never",
            "sandboxPolicy": {"type": "readOnly"},
            "serviceName": "kage_voice",
            "disabledPluginIds": self._disabled_plugin_ids,
        }, timeout=30)
        thread = result.get("thread", result)
        self._thread_id = thread.get("id")
        if not self._thread_id:
            raise CodexBridgeError("Codex App Server did not return a thread ID")
        startup_ms = round((time.perf_counter() - (self._started_at or time.perf_counter())) * 1000, 1)
        print(json.dumps({"event": "codex_ready", "startup_ms": startup_ms,
                          "model": MODEL, "disabled_plugins": len(self._disabled_plugin_ids)}))

    def ask(self, message: str, web_context: str | None = None,
            on_delta: Callable[[str], None] | None = None, timeout: float = 60) -> dict[str, Any]:
        """Return a concise reply and first-token/total timings. Never gives Codex tools."""
        with self._lock:
            self._ensure_thread()
            assert self._thread_id
            self._request_id += 1
            request_id = self._request_id
            prompt = (
                "You are Kage, a desktop voice assistant. Reply in concise, natural English. "
                "For a factual question, lead with the direct answer. Start with one useful "
                "short sentence, then use two to four compact sentences when more detail helps. "
                "End each idea with a period; avoid long, multi-clause sentences. "
                "Do not inspect files, use tools, browse the web, or change anything. "
                "If web results are provided below, use them as the only source for current facts, "
                "and mention the source title or URL when useful. Otherwise do not invent current facts.\n\n"
                f"User: {message}\n\n"
                f"{web_context or 'No web results were requested.'}"
            )
            params = {
                "threadId": self._thread_id,
                "input": [{"type": "text", "text": prompt}],
                "model": MODEL,
                "effort": EFFORT,
                "approvalPolicy": "never",
                "sandboxPolicy": {"type": "readOnly"},
                "disabledPluginIds": self._disabled_plugin_ids,
            }
            self._send({"jsonrpc": "2.0", "id": request_id, "method": "turn/start", "params": params})
            started = time.perf_counter()
            first_delta_ms: float | None = None
            chunks: list[str] = []
            token_usage: dict[str, Any] | None = None
            deadline = time.monotonic() + timeout
            held: list[dict[str, Any]] = []
            try:
                while time.monotonic() < deadline:
                    try:
                        event = self._messages.get(timeout=min(0.25, deadline - time.monotonic()))
                    except queue.Empty:
                        continue
                    if event.get("id") == request_id:
                        if "error" in event:
                            raise CodexBridgeError(event["error"].get("message", "turn failed"))
                        continue
                    if event.get("method") == "item/agentMessage/delta":
                        text = event.get("params", {}).get("delta", "")
                        if text:
                            if first_delta_ms is None:
                                first_delta_ms = round((time.perf_counter() - started) * 1000, 1)
                            chunks.append(text)
                            if on_delta:
                                on_delta(text)
                        continue
                    if event.get("method") == "thread/tokenUsage/updated":
                        token_usage = event.get("params")
                        continue
                    if event.get("method") == "turn/completed":
                        params = event.get("params", {})
                        if params.get("threadId") == self._thread_id:
                            return {
                                "reply": "".join(chunks).strip(),
                                "first_delta_ms": first_delta_ms,
                                "total_ms": round((time.perf_counter() - started) * 1000, 1),
                                "token_usage": token_usage,
                                "model": MODEL,
                            }
                    held.append(event)
            finally:
                for event in held:
                    self._messages.put(event)
            raise CodexBridgeError("Codex response timed out")

    def close(self) -> None:
        if self._process and self._process.poll() is None:
            self._process.terminate()
        self._process = None
        self._thread_id = None


codex_bridge = CodexBridge()
