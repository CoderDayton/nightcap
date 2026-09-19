#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.
"""Summarizes `--profile` traces as a Markdown table for pull requests.

Usage: summarize_profile_trace.py TRACE [TRACE ...]

Each trace gets its own column, so `before.json after.json` compares them.
Frame times come from the "frame interval (ms)" counter; every other row is
a slice name recorded by the Vulkan adapter.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys
from typing import Iterable

FRAME_COUNTER = "frame interval (ms)"


@dataclass(frozen=True)
class Distribution:
    count: int
    total: float
    mean: float
    p50: float
    p95: float
    p99: float
    max: float


@dataclass(frozen=True)
class Summary:
    frames: Distribution | None
    slices: dict[str, Distribution]


def _nearest_rank(ordered: list[float], percent: float) -> float:
    rank = max(1, math.ceil(percent / 100 * len(ordered)))
    return ordered[rank - 1]


def _distribution(values: list[float]) -> Distribution:
    ordered = sorted(values)
    total = sum(ordered)
    return Distribution(count=len(ordered), total=total,
                        mean=total / len(ordered),
                        p50=_nearest_rank(ordered, 50),
                        p95=_nearest_rank(ordered, 95),
                        p99=_nearest_rank(ordered, 99), max=ordered[-1])


def summarize(events: Iterable[dict]) -> Summary:
    frames: list[float] = []
    slices: dict[str, list[float]] = {}
    for event in events:
        phase = event.get("ph")
        if phase == "C" and event.get("name") == FRAME_COUNTER:
            frames.append(float(event["args"]["value"]))
        elif phase == "X":
            slices.setdefault(event["name"], []).append(event["dur"] / 1000)
    return Summary(
        frames=_distribution(frames) if frames else None,
        slices={name: _distribution(values)
                for name, values in slices.items()})


def _row(label: str, cells: list[str]) -> str:
    return "| " + " | ".join([label, *cells]) + " |"


def _cell(distribution: Distribution | None, field: str) -> str:
    if distribution is None:
        return "-"
    if field == "count":
        return str(distribution.count)
    return f"{getattr(distribution, field):.2f}"


def _metric_row(label: str, distributions: list[Distribution | None],
                field: str) -> str:
    return _row(label, [_cell(item, field) for item in distributions])


def to_markdown(traces: list[tuple[str, Summary]]) -> str:
    summaries = [summary for _, summary in traces]
    lines = [_row("metric", [name for name, _ in traces]),
             _row("---", ["---:"] * len(traces))]

    frames = [summary.frames for summary in summaries]
    lines.append(_metric_row("frames", frames, "count"))
    if any(frames):
        lines += [_metric_row(f"frame interval {field} (ms)", frames, field)
                  for field in ("mean", "p50", "p95", "p99", "max")]

    # Largest total time first, taken from whichever trace spent the most.
    totals: dict[str, float] = {}
    for summary in summaries:
        for name, distribution in summary.slices.items():
            totals[name] = max(totals.get(name, 0.0), distribution.total)
    for name in sorted(totals, key=lambda name: -totals[name]):
        column = [summary.slices.get(name) for summary in summaries]
        lines.append(_metric_row(f"{name} count", column, "count"))
        lines += [_metric_row(f"{name} {field} (ms)", column, field)
                  for field in ("total", "mean", "p99", "max")]
    return "\n".join(lines)


def main(arguments: list[str]) -> int:
    if not arguments:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    traces = []
    for argument in arguments:
        path = Path(argument)
        traces.append((path.name, summarize(json.loads(path.read_text()))))
    print(to_markdown(traces))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
