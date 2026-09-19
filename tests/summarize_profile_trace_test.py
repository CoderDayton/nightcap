#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
# Licensed under the Apache License, Version 2.0.

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "scripts/summarize_profile_trace.py"
SPEC = importlib.util.spec_from_file_location("summarize_profile_trace",
                                              SCRIPT)
assert SPEC is not None and SPEC.loader is not None
summary = importlib.util.module_from_spec(SPEC)
# Dataclasses resolve their module through sys.modules.
sys.modules[SPEC.name] = summary
SPEC.loader.exec_module(summary)


def frame(ts_us: int, interval_ms: float) -> dict:
    return {"name": "frame interval (ms)", "ph": "C", "ts": ts_us,
            "pid": 1, "tid": 1, "args": {"value": interval_ms}}


def slice_event(name: str, dur_us: int, ts_us: int = 0) -> dict:
    return {"name": name, "cat": "test", "ph": "X", "ts": ts_us,
            "dur": dur_us, "pid": 1, "tid": 1}


class SummarizeTest(unittest.TestCase):

    def test_frame_interval_percentiles_use_nearest_rank(self) -> None:
        events = [frame(index, float(index)) for index in range(1, 101)]

        result = summary.summarize(events)

        frames = result.frames
        self.assertEqual(frames.count, 100)
        self.assertAlmostEqual(frames.mean, 50.5)
        self.assertEqual(frames.p50, 50.0)
        self.assertEqual(frames.p95, 95.0)
        self.assertEqual(frames.p99, 99.0)
        self.assertEqual(frames.max, 100.0)

    def test_slices_are_grouped_by_name_in_milliseconds(self) -> None:
        events = [
            slice_event("vkQueuePresentKHR", 2_000),
            slice_event("vkQueuePresentKHR", 4_000),
            slice_event("vkCreateGraphicsPipelines", 30_000),
        ]

        result = summary.summarize(events)

        present = result.slices["vkQueuePresentKHR"]
        self.assertEqual(present.count, 2)
        self.assertAlmostEqual(present.total, 6.0)
        self.assertAlmostEqual(present.mean, 3.0)
        self.assertAlmostEqual(present.max, 4.0)
        self.assertEqual(result.slices["vkCreateGraphicsPipelines"].count, 1)

    def test_other_events_are_ignored(self) -> None:
        events = [
            {"name": "thread", "ph": "M", "ts": 0, "pid": 1, "tid": 1},
            {"name": "other counter", "ph": "C", "ts": 0, "pid": 1, "tid": 1,
             "args": {"value": 5.0}},
        ]

        result = summary.summarize(events)

        self.assertIsNone(result.frames)
        self.assertEqual(result.slices, {})

    def test_markdown_puts_each_trace_in_its_own_column(self) -> None:
        before = summary.summarize([frame(1, 20.0), slice_event("a", 1_000)])
        after = summary.summarize([frame(1, 10.0), slice_event("a", 500)])

        table = summary.to_markdown([("before.json", before),
                                     ("after.json", after)])

        lines = table.splitlines()
        self.assertEqual(lines[0], "| metric | before.json | after.json |")
        self.assertIn("| frame interval p50 (ms) | 20.00 | 10.00 |", lines)
        self.assertIn("| a mean (ms) | 1.00 | 0.50 |", lines)

    def test_markdown_marks_missing_values(self) -> None:
        with_slice = summary.summarize([slice_event("a", 1_000)])
        without = summary.summarize([])

        table = summary.to_markdown([("one", with_slice), ("two", without)])

        self.assertIn("| a count | 1 | - |", table.splitlines())
        self.assertIn("| frames | - | - |", table.splitlines())

    def test_main_reads_trace_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.json"
            path.write_text(json.dumps([frame(1, 16.0),
                                        slice_event("b", 2_000)]))
            output = io.StringIO()

            with contextlib.redirect_stdout(output):
                code = summary.main([str(path)])

        self.assertEqual(code, 0)
        self.assertIn("| frame interval p99 (ms) | 16.00 |",
                      output.getvalue())


if __name__ == "__main__":
    unittest.main()
