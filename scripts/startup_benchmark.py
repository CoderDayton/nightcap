#!/usr/bin/env python3
"""Manual benchmark of the real runtime: startup, frame rate, CPU and memory.

Starts build/mocktail, waits for the first presented frame, lets it run for
--exit-after-ms, and reads the results from the process and its log.
No sign-in is needed; it measures the app up to and on the login screen.

  scripts/startup_benchmark.py run [--profile ingame] [--runs N] [--json OUT]
  scripts/startup_benchmark.py compare BEFORE.json AFTER.json

The ingame profile uses the owner's in-game setup: a 3440x1440 window,
a 120 FPS cap and vsync off. It still runs on the login screen.
"""

import argparse
import json
import os
import re
import signal
import statistics
import subprocess
import shutil
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def xdg_home(name, fallback):
    value = os.environ.get(name, "")
    return Path(value) if os.path.isabs(value) else Path.home() / fallback


CONFIG = xdg_home("XDG_CONFIG_HOME", ".config") / "mocktail" / "config.yaml"
# The runtime saves the window size here on exit.
WINDOW_STATE = xdg_home("XDG_STATE_HOME", ".local/state") / "mocktail" / "window-state.json"
# Each run replaces the previous run's logs.
LOG_DIR = xdg_home("XDG_CACHE_HOME", ".cache") / "mocktail-bench" / "last-startup"

PROFILES = {
    "default": {},
    "ingame": {
        "MOCKTAIL_WIN_WIDTH": "3440",
        "MOCKTAIL_WIN_HEIGHT": "1440",
        "MOCKTAIL_FRAME_RATE_LIMIT": "120",
        "MOCKTAIL_VSYNC": "off",
    },
}

FIRST_FRAME = re.compile(r"\[window\] first Roblox (?:Vulkan )?frame presented")
STAGE = re.compile(r"^\[stage (\d+)\]")
PRESENT = re.compile(r"\[perf\] present_sample frame=(\d+) monotonic_ns=(\d+)")
RSS = re.compile(r"\[perf\] process rss_mib=(\d+)")
STARTUP_RETURNED = re.compile(r"\[engine\] legacy startup call sequence returned")


def read_config():
    try:
        return CONFIG.read_bytes()
    except OSError:
        return None


def restore_config(original):
    """Puts config.yaml back as it was before the runs, keeping it 0600."""
    if read_config() == original:
        return
    print(f"warning: {CONFIG} changed during the benchmark; restoring it")
    if original is None:
        CONFIG.unlink(missing_ok=True)
        return
    temporary = CONFIG.with_name(f".{CONFIG.name}.bench-restore")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as file:
        file.write(original)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, CONFIG)


def instance_running():
    result = subprocess.run(["pgrep", "-x", "mocktail"], capture_output=True)
    return result.returncode == 0


def run_once(args, index, log_dir):
    env = dict(os.environ)
    env["MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS"] = str(args.exit_after_ms)
    env.setdefault("SDL_VIDEODRIVER", "wayland,x11")
    env.update(PROFILES[args.profile])
    if args.unthrottled:
        env["MOCKTAIL_FRAME_RATE_LIMIT"] = "unlimited"
        env["MOCKTAIL_VSYNC"] = "off"
    heap_prefix = None
    if args.heap_counter:
        heap_prefix = str(log_dir / f"heap{index}")
        env["LD_PRELOAD"] = str(Path(args.heap_counter).resolve())
        env["MOCKTAIL_HEAP_COUNTER_OUT"] = heap_prefix

    log_path = log_dir / f"run{index}.log"
    marks = {}
    presents = []
    rss = []
    start = time.monotonic()
    process = subprocess.Popen(
        [args.bin], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, start_new_session=True)

    def reader():
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            for raw in process.stdout:
                line = raw.decode("utf-8", errors="replace")
                log.write(line)
                elapsed_ms = (time.monotonic() - start) * 1000.0
                if "startup.first_frame_ms" not in marks and FIRST_FRAME.search(line):
                    marks["startup.first_frame_ms"] = elapsed_ms
                elif STARTUP_RETURNED.search(line):
                    marks.setdefault("startup.engine_started_ms", elapsed_ms)
                if match := STAGE.match(line):
                    marks.setdefault(f"startup.stage{match.group(1)}_ms", elapsed_ms)
                if match := PRESENT.search(line):
                    presents.append((int(match.group(1)), int(match.group(2))))
                if match := RSS.search(line):
                    rss.append(int(match.group(1)))

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    deadline = start + args.timeout
    status = None
    usage = None
    while status is None:
        pid, wait_status, usage = os.wait4(process.pid, os.WNOHANG)
        if pid != 0:
            status = os.waitstatus_to_exitcode(wait_status)
            break
        if time.monotonic() > deadline:
            stop_group(process.pid)
            print(f"  run {index}: timed out after {args.timeout}s, log {log_path}")
            return None
        time.sleep(0.05)
    wall_ms = (time.monotonic() - start) * 1000.0
    process.returncode = status
    thread.join(timeout=5)
    if status != 0:
        print(f"  run {index}: exit status {status}, log {log_path}")
        return None
    if "startup.first_frame_ms" not in marks:
        print(f"  run {index}: no frame presented, log {log_path}")
        return None

    cpu_ms = (usage.ru_utime + usage.ru_stime) * 1000.0
    metrics = dict(marks)
    metrics.update({
        "process.wall_ms": wall_ms,
        "cpu.user_ms": usage.ru_utime * 1000.0,
        "cpu.system_ms": usage.ru_stime * 1000.0,
        "cpu.percent_of_one_core": cpu_ms / wall_ms * 100.0,
        "memory.peak_rss_mib": usage.ru_maxrss / 1024.0,
        "memory.minor_faults": usage.ru_minflt,
        "memory.major_faults": usage.ru_majflt,
        "sched.voluntary_switches": usage.ru_nvcsw,
        "sched.involuntary_switches": usage.ru_nivcsw,
    })
    if rss:
        metrics["memory.sampled_rss_mib"] = max(rss)
    def fps_between(first, last):
        (first_frame, first_ns), (last_frame, last_ns) = first, last
        if last_ns <= first_ns:
            return None
        return (last_frame - first_frame) / ((last_ns - first_ns) / 1e9)

    if len(presents) >= 2:
        metrics["frames.average_fps"] = fps_between(presents[0], presents[-1])
    # Frames 1-240 include the startup ramp; later samples show the cap.
    if len(presents) >= 3:
        metrics["frames.steady_fps"] = fps_between(presents[1], presents[-1])
    metrics = {name: value for name, value in metrics.items() if value is not None}
    if heap_prefix:
        heap_file = Path(f"{heap_prefix}.{process.pid}")
        try:
            metrics.update(json.loads(heap_file.read_text()))
        except (OSError, ValueError):
            print(f"  run {index}: heap counter wrote nothing (process skipped exit hooks)")
    return metrics


def stop_group(pid):
    """SIGTERM the run's process group, then SIGKILL it after 10 s."""
    try:
        os.killpg(pid, signal.SIGTERM)
        for _ in range(200):
            if os.waitpid(pid, os.WNOHANG)[0] != 0:
                return
            time.sleep(0.05)
        os.killpg(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except (ProcessLookupError, ChildProcessError):
        return


def command_run(args):
    if not os.access(args.bin, os.X_OK):
        sys.exit(f"missing binary {args.bin}; run `make build JOBS=16` first")
    if instance_running():
        sys.exit("another mocktail is running; close it first so this run starts fresh")
    shutil.rmtree(LOG_DIR, ignore_errors=True)
    LOG_DIR.mkdir(parents=True)
    log_dir = LOG_DIR
    config = read_config()
    try:
        window_state = WINDOW_STATE.read_bytes()
    except OSError:
        window_state = None
    results = []
    try:
        for index in range(1, args.runs + 1):
            print(f"run {index}/{args.runs}...", flush=True)
            metrics = run_once(args, index, log_dir)
            if metrics:
                results.append(metrics)
    finally:
        if window_state is not None:
            WINDOW_STATE.write_bytes(window_state)
        else:
            # The runs created it; leave no benchmark window size behind.
            WINDOW_STATE.unlink(missing_ok=True)
        restore_config(config)
    if not results:
        sys.exit(f"no successful runs; logs in {log_dir}")

    names = sorted({name for metrics in results for name in metrics})
    summary = {
        name: statistics.median(m[name] for m in results if name in m)
        for name in names
    }
    print(f"\nmocktail startup benchmark: profile {args.profile}, median of "
          f"{len(results)} runs (exit {args.exit_after_ms} ms after first frame"
          f"{', unthrottled' if args.unthrottled else ''})")
    for name in names:
        print(f"  {name:<30} {summary[name]:12.1f}")
    print(f"logs: {log_dir}")
    if args.json:
        Path(args.json).write_text(json.dumps({
            "benchmark": "mocktail_startup",
            "profile": args.profile,
            "runs": len(results),
            "unthrottled": args.unthrottled,
            "metrics": summary,
        }, indent=2) + "\n")


def command_compare(args):
    before = json.loads(Path(args.before).read_text())["metrics"]
    after = json.loads(Path(args.after).read_text())["metrics"]
    print(f"{'metric':<30} {'before':>12} {'after':>12} {'change':>9}")
    for name in sorted(set(before) | set(after)):
        old, new = before.get(name), after.get(name)
        if old is None or new is None:
            print(f"{name:<30} {old if old is not None else '-':>12} "
                  f"{new if new is not None else '-':>12}")
            continue
        change = f"{(new - old) / old * 100:+.1f}%" if old else "-"
        print(f"{name:<30} {old:12.1f} {new:12.1f} {change:>9}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="benchmark the real runtime")
    run.add_argument("--bin", default=str(ROOT / "build" / "mocktail"))
    run.add_argument("--profile", choices=sorted(PROFILES), default="default")
    run.add_argument("--runs", type=int, default=3)
    run.add_argument("--exit-after-ms", type=int, default=5000)
    run.add_argument("--timeout", type=int, default=180)
    run.add_argument("--unthrottled", action="store_true",
                     help="turn off the frame limit and vsync")
    run.add_argument("--heap-counter",
                     help="path to libmocktail_heap_counter.so (adds overhead; "
                          "run without it for timing)")
    run.add_argument("--json", help="write the medians here")
    run.set_defaults(func=command_run)
    compare = sub.add_parser("compare", help="compare two JSON results")
    compare.add_argument("before")
    compare.add_argument("after")
    compare.set_defaults(func=command_compare)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
