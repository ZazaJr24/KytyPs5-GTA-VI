#!/usr/bin/env python3
"""Capture paired cold/warm Kyty results in isolated working directories."""

import argparse
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys


def capture(executable, game, cwd, state, seconds, args):
    report_path = cwd / f"{state}.json"
    command = [
        str(executable), "--game", str(game), "--shader-cache",
        "--benchmark-output", str(report_path),
        "--benchmark-cache-state", state, "--benchmark-seconds", str(seconds),
        *args,
    ]
    log_path = cwd / f"{state}.log"
    try:
        with log_path.open("wb") as log:
            subprocess.run(
                command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                timeout=seconds + 60, check=True,
            )
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise RuntimeError(f"{cwd.name} {state} failed. Log: {log_path}") from error
    if (
        report.get("schema_version") != 1
        or report.get("run", {}).get("cache_state") != state
        or not report.get("ended_at_deadline")
        or report.get("presented_guest_frames", 0) < 2
        or report.get("frame_intervals_truncated")
        or report.get("guest_extent_changed")
    ):
        raise RuntimeError(f"Incomplete or inconsistent capture: {report_path}")
    return report


def summarize(reports):
    def metrics(report):
        return {
            "first_present_ns": report["first_present_ns"],
            "p50_ns": report["frame_percentiles_ns"]["p50"],
            "p95_ns": report["frame_percentiles_ns"]["p95"],
            "p99_ns": report["frame_percentiles_ns"]["p99"],
            "guest_read_bytes_per_second": report["guest_read_bytes_per_second"],
            "peak_process_resident_bytes": report["peak_process_resident_bytes"],
            "peak_sampled_device_memory_bytes": report["peak_sampled_device_memory_bytes"],
        }

    points = [metrics(report) for report in reports]
    return {
        key: statistics.median(point[key] for point in points)
        if all(point[key] is not None for point in points) else None
        for key in points[0]
    }


def compare(results):
    summary = {}
    comparable = (
        "gpu", "vulkan_driver_version", "vulkan_vendor_id", "vulkan_device_id",
        "requested_window_width", "requested_window_height", "present_mode", "vblank_hz",
    )
    for state in ("cold", "warm"):
        baseline = results["baseline"][state]
        candidate = results["candidate"][state]
        reference = baseline[0]["run"]
        extent = baseline[0]["first_guest_width"], baseline[0]["first_guest_height"]
        for report in baseline + candidate:
            if (
                any(report["run"][key] != reference[key] for key in comparable)
                or (report["first_guest_width"], report["first_guest_height"]) != extent
            ):
                raise RuntimeError("Captures used different GPU settings or guest dimensions.")
        reference_metrics, candidate_metrics = summarize(baseline), summarize(candidate)
        summary[state] = {
            "baseline_medians": reference_metrics,
            "candidate_medians": candidate_metrics,
            "candidate_change_percent": {
                key: (candidate_metrics[key] / value - 1) * 100
                if value and candidate_metrics[key] is not None else None
                for key, value in reference_metrics.items()
            },
        }
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path, help="Instrumented reference emulator")
    parser.add_argument("--candidate", required=True, type=Path, help="Candidate emulator")
    parser.add_argument("--game", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New output directory")
    parser.add_argument("--seconds", type=int, default=120)
    parser.add_argument("--trials", type=int, default=5, help="Cold/warm pairs per emulator")
    parser.add_argument("--seed", type=Path, help="Optional fixture with identical local save data")
    parser.add_argument("--common-arg", action="append", default=[])
    parser.add_argument("--candidate-arg", action="append", default=[])
    options = parser.parse_args()
    if options.seconds < 1 or options.trials < 1:
        parser.error("--seconds and --trials must be positive")
    executables = {name: getattr(options, name).resolve() for name in ("baseline", "candidate")}
    for name, executable in executables.items():
        if not executable.is_file():
            parser.error(f"{name} emulator is missing: {executable}")
    game = options.game.resolve()
    if not game.exists():
        parser.error(f"game is missing: {game}")
    seed = options.seed.resolve() if options.seed else None
    if seed is not None and not seed.is_dir():
        parser.error(f"seed directory is missing: {seed}")
    output = options.output.resolve()
    if seed is not None and (output == seed or seed in output.parents):
        parser.error("--output must be outside --seed")
    output.mkdir(parents=True)
    results = {name: {"cold": [], "warm": []} for name in executables}

    for index in range(options.trials):
        order = list(executables)
        if index % 2:
            order.reverse()
        for name in order:
            cwd = output / f"{index + 1:02d}-{name}"
            if seed is None:
                cwd.mkdir()
            else:
                shutil.copytree(seed, cwd, ignore=shutil.ignore_patterns("_PipelineCache"))
            args = options.common_arg + (options.candidate_arg if name == "candidate" else [])
            for state in ("cold", "warm"):
                print(f"Trial {index + 1}: {name} {state}", flush=True)
                results[name][state].append(capture(
                    executables[name], game, cwd, state, options.seconds, args,
                ))

    summary_path = output / "comparison.json"
    summary_path.write_text(json.dumps(compare(results), indent=2) + "\n", encoding="utf-8")
    print(summary_path)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
