import argparse
import ctypes
import gc
import json
import os
import statistics
import sys
import threading
import time
import unicodedata
import wave
from contextlib import contextmanager
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
BACKEND_DIR = REPO_ROOT / "backend"
sys.path.insert(0, str(BACKEND_DIR))

from stt_engine import KageSTT


DEFAULT_ENDPOINTS_MS = (700, 550, 450)


def collect_audio(path):
    path = Path(path)
    if path.is_file():
        return [path]
    if path.is_dir():
        return sorted(path.glob("*.wav"))
    return []


def wav_duration_ms(path):
    with wave.open(str(path), "rb") as audio:
        return round(audio.getnframes() * 1000 / audio.getframerate(), 1)


def normalize_text(value):
    value = unicodedata.normalize("NFKC", value).casefold()
    return " ".join("".join(char if char.isalnum() else " " for char in value).split())


def edit_distance(left, right):
    previous = list(range(len(right) + 1))
    for row, left_item in enumerate(left, 1):
        current = [row]
        for column, right_item in enumerate(right, 1):
            current.append(min(
                current[-1] + 1,
                previous[column] + 1,
                previous[column - 1] + (left_item != right_item),
            ))
        previous = current
    return previous[-1]


def error_rates(reference, hypothesis):
    ref_normal = normalize_text(reference)
    hyp_normal = normalize_text(hypothesis)
    ref_words = ref_normal.split()
    hyp_words = hyp_normal.split()
    return {
        "reference": reference,
        "wer": round(edit_distance(ref_words, hyp_words) / max(1, len(ref_words)), 4),
        "cer": round(edit_distance(ref_normal, hyp_normal) / max(1, len(ref_normal)), 4),
        "exact_normalized": ref_normal == hyp_normal,
    }


def load_references(audio_dir):
    manifest_path = Path(audio_dir) / "references.json"
    if not manifest_path.exists():
        return {}
    payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    if isinstance(payload, dict) and isinstance(payload.get("files"), list):
        return {item["file"]: item["text"] for item in payload["files"]}
    if isinstance(payload, dict):
        return payload
    raise ValueError(f"Unsupported references format: {manifest_path}")


@contextmanager
def temporary_environment(values):
    previous = {name: os.environ.get(name) for name in values}
    try:
        for name, value in values.items():
            os.environ[name] = str(value)
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


if os.name == "nt":
    from ctypes import wintypes

    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    def _filetime_seconds(value):
        return ((value.dwHighDateTime << 32) | value.dwLowDateTime) / 10_000_000

    def process_cpu_seconds(pid):
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel = wintypes.FILETIME()
        user = wintypes.FILETIME()
        try:
            ok = ctypes.windll.kernel32.GetProcessTimes(
                handle, ctypes.byref(creation), ctypes.byref(exit_time),
                ctypes.byref(kernel), ctypes.byref(user),
            )
            return _filetime_seconds(kernel) + _filetime_seconds(user) if ok else None
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)

    def process_rss_mb(pid):
        handle = ctypes.windll.kernel32.OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid
        )
        if not handle:
            return None
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        try:
            ok = ctypes.windll.psapi.GetProcessMemoryInfo(
                handle, ctypes.byref(counters), counters.cb
            )
            return counters.WorkingSetSize / 1048576 if ok else None
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
else:
    def process_cpu_seconds(pid):
        return time.process_time() if pid == os.getpid() else None

    def process_rss_mb(pid):
        return None


class ResourceSampler:
    def __init__(self, pid):
        self.pid = pid
        self.stop_event = threading.Event()
        self.samples = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop_event.wait(0.02):
            value = process_rss_mb(self.pid)
            if value is not None:
                self.samples.append(value)

    def __enter__(self):
        self._run_once()
        self.thread.start()
        return self

    def _run_once(self):
        value = process_rss_mb(self.pid)
        if value is not None:
            self.samples.append(value)

    def __exit__(self, *_):
        self.stop_event.set()
        self.thread.join(timeout=1)
        self._run_once()

    @property
    def peak_mb(self):
        return round(max(self.samples), 1) if self.samples else None


def nemo_server_pid():
    path = Path(r"C:\Kage\stt_diagnostics\nemotron_server.json")
    if not path.exists():
        return None
    try:
        pid = int(json.loads(path.read_text(encoding="utf-8-sig"))["pid"])
        return pid if process_cpu_seconds(pid) is not None else None
    except (KeyError, TypeError, ValueError, OSError, json.JSONDecodeError):
        return None


CASES = (
    (
        "whisper-en-current",
        {
            "KAGE_STT_ENGINE": "whisper",
            "KAGE_STT_LANGUAGE": "en",
            "KAGE_WHISPER_MODEL": "base",
            "KAGE_WHISPER_COMPUTE": "int8",
            "KAGE_WHISPER_BEAM": "1",
            "KAGE_WHISPER_PROMPT": "0",
            "KAGE_STT_FALLBACK": "0",
        },
    ),
    (
        "whisper-fr-fixed",
        {
            "KAGE_STT_ENGINE": "whisper",
            "KAGE_STT_LANGUAGE": "fr",
            "KAGE_WHISPER_MODEL": "base",
            "KAGE_WHISPER_COMPUTE": "int8",
            "KAGE_WHISPER_BEAM": "1",
            "KAGE_WHISPER_PROMPT": "1",
            "KAGE_STT_FALLBACK": "0",
        },
    ),
    (
        "nemotron-fr",
        {
            "KAGE_STT_ENGINE": "nemotron",
            "KAGE_STT_LANGUAGE": "fr",
            "KAGE_STT_FALLBACK": "0",
            "KAGE_NEMO_CONFIDENCE_USABLE": "0",
        },
    ),
)


def run_case(name, environment, files, references, repeats):
    rows = []
    with temporary_environment(environment):
        init_started = time.perf_counter()
        init_rss = process_rss_mb(os.getpid())
        stt = KageSTT()
        init_ms = (time.perf_counter() - init_started) * 1000
        loaded_rss = process_rss_mb(os.getpid())

        # Match the warm production path: model startup is reported separately,
        # then one discarded utterance warms kernels and file I/O.
        warmup_started = time.perf_counter()
        stt.transcribe(files[0])
        warmup_ms = (time.perf_counter() - warmup_started) * 1000

        for repeat in range(1, repeats + 1):
            for wav in files:
                target_pid = nemo_server_pid() if name == "nemotron-fr" else os.getpid()
                target_pid = target_pid or os.getpid()
                cpu_before = process_cpu_seconds(target_pid)
                started = time.perf_counter()
                with ResourceSampler(target_pid) as resources:
                    result = stt.transcribe(wav)
                wall_ms = (time.perf_counter() - started) * 1000
                cpu_after = process_cpu_seconds(target_pid)
                cpu_ms = None
                if cpu_before is not None and cpu_after is not None:
                    cpu_ms = max(0.0, (cpu_after - cpu_before) * 1000)
                row = {
                    "case": name,
                    "repeat": repeat,
                    "file": wav.name,
                    "audio_duration_ms": wav_duration_ms(wav),
                    "engine_init_ms": round(init_ms, 1),
                    "engine_init_rss_delta_mb": (
                        round(loaded_rss - init_rss, 1)
                        if loaded_rss is not None and init_rss is not None else None
                    ),
                    "warmup_ms": round(warmup_ms, 1),
                    "measured_wall_ms": round(wall_ms, 1),
                    "process_cpu_ms": round(cpu_ms, 1) if cpu_ms is not None else None,
                    "cpu_equivalent_cores": (
                        round(cpu_ms / wall_ms, 2) if cpu_ms is not None and wall_ms else None
                    ),
                    "cpu_capacity_percent": (
                        round(cpu_ms / wall_ms / max(1, os.cpu_count()) * 100, 1)
                        if cpu_ms is not None and wall_ms else None
                    ),
                    "peak_rss_mb": resources.peak_mb,
                    "endpoint_projections_ms": {
                        str(endpoint): round(endpoint + wall_ms, 1)
                        for endpoint in DEFAULT_ENDPOINTS_MS
                    },
                    **result.log_payload(),
                }
                reference = references.get(wav.name)
                if reference is not None:
                    row.update(error_rates(reference, result.text))
                rows.append(row)
                print(json.dumps(row, ensure_ascii=False))
        del stt
        gc.collect()
    return rows


def summarize(rows):
    grouped = {}
    for row in rows:
        grouped.setdefault(row["case"], []).append(row)
    summary = {}
    for case, items in grouped.items():
        times = [float(item["measured_wall_ms"]) for item in items]
        summary[case] = {
            "count": len(items),
            "median_stt_ms": round(statistics.median(times), 1),
            "mean_stt_ms": round(statistics.mean(times), 1),
            "p95_stt_ms": round(sorted(times)[max(0, int(len(times) * 0.95) - 1)], 1),
            "mean_cpu_capacity_percent": round(statistics.mean(
                item["cpu_capacity_percent"] for item in items
                if item["cpu_capacity_percent"] is not None
            ), 1),
            "max_peak_rss_mb": max(
                item["peak_rss_mb"] for item in items if item["peak_rss_mb"] is not None
            ),
            "mean_wer": (
                round(statistics.mean(item["wer"] for item in items if "wer" in item), 4)
                if any("wer" in item for item in items) else None
            ),
            "exact_normalized": sum(bool(item.get("exact_normalized")) for item in items),
            "fallbacks": sum(bool(item.get("fallback_used")) for item in items),
        }
    return summary


def main():
    parser = argparse.ArgumentParser(
        description="Compare the exact live Whisper-English baseline, corrected French, and Nemotron."
    )
    parser.add_argument("--audio", default=r"C:\Kage\stt_bench_audio")
    parser.add_argument("--output", default=r"C:\Kage\stt_diagnostics\stt_ab_results.json")
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--skip-nemotron", action="store_true")
    args = parser.parse_args()

    files = collect_audio(args.audio)
    if not files:
        raise SystemExit(f"No WAV files found at {args.audio}.")
    references = load_references(Path(args.audio) if Path(args.audio).is_dir() else Path(args.audio).parent)

    all_rows = []
    failures = []
    for name, environment in CASES:
        if name == "nemotron-fr" and args.skip_nemotron:
            continue
        try:
            all_rows.extend(run_case(name, environment, files, references, args.repeats))
        except Exception as exc:
            failure = {"case": name, "error": f"{type(exc).__name__}: {exc}"}
            failures.append(failure)
            print(json.dumps(failure, ensure_ascii=False), file=sys.stderr)

    report = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "logical_cpu_count": os.cpu_count(),
        "audio_files": [str(item) for item in files],
        "endpoint_values_ms": list(DEFAULT_ENDPOINTS_MS),
        "results": all_rows,
        "failures": failures,
        "summary": summarize(all_rows),
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\nSUMMARY")
    print(json.dumps(report["summary"], ensure_ascii=False, indent=2))
    print(f"\nReport: {output}")
    if failures:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
