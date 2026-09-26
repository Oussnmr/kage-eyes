import argparse
import json
import os
import statistics
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
BACKEND_DIR = REPO_ROOT / "backend"
sys.path.insert(0, str(BACKEND_DIR))

from stt_engine import KageSTT


def collect_audio(path):
    path = Path(path)
    if path.is_file():
        return [path]
    if path.is_dir():
        return sorted(path.glob("*.wav"))
    return []


def run_case(name, engine, language, files):
    os.environ["KAGE_STT_ENGINE"] = engine
    os.environ["KAGE_STT_LANGUAGE"] = language
    stt = KageSTT()
    rows = []
    for wav in files:
        result = stt.transcribe(wav)
        row = {"case": name, "file": str(wav), **result.log_payload()}
        rows.append(row)
        print(json.dumps(row, ensure_ascii=False))
    return rows


def summarize(rows):
    grouped = {}
    for row in rows:
        grouped.setdefault(row["case"], []).append(row)
    summary = {}
    for case, items in grouped.items():
        times = [float(item["duration_ms"]) for item in items]
        summary[case] = {
            "count": len(items),
            "median_ms": round(statistics.median(times), 1),
            "mean_ms": round(statistics.mean(times), 1),
            "min_ms": round(min(times), 1),
            "max_ms": round(max(times), 1),
            "fallbacks": sum(bool(item.get("fallback_used")) for item in items),
        }
    return summary


def main():
    parser = argparse.ArgumentParser(
        description="Compare current Whisper-English, Whisper-French, and Nemotron-French."
    )
    parser.add_argument("--audio", default=r"C:\Kage\stt_bench_audio")
    parser.add_argument("--output", default=r"C:\Kage\stt_diagnostics\stt_ab_results.json")
    args = parser.parse_args()

    files = collect_audio(args.audio)
    if not files:
        raise SystemExit(f"No WAV files found at {args.audio}.")

    all_rows = []
    all_rows.extend(run_case("whisper-en-current", "whisper", "en", files))
    all_rows.extend(run_case("whisper-fr-fixed", "whisper", "fr", files))
    all_rows.extend(run_case("nemotron-fr", "nemotron", "fr", files))

    report = {
        "audio_files": [str(item) for item in files],
        "results": all_rows,
        "summary": summarize(all_rows),
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("\nSUMMARY")
    print(json.dumps(report["summary"], ensure_ascii=False, indent=2))
    print(f"\nReport: {output}")


if __name__ == "__main__":
    main()
