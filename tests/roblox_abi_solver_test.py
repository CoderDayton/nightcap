#!/usr/bin/env python3
# Copyright 2026 Mocktail Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

from host_abi_regression import (
    REGRESSION_CACHE,
    expected_runtime,
    load_regression_build,
    regression_library,
    regression_manifest,
    regression_sidecar,
)

PROJECT_ROOT = Path(__file__).resolve().parents[1]
# Each pair solves the second build from the first.
REGRESSION_PAIRS = (
    ("2908", "2998"), ("2998", "3092"), ("3092", "3222"), ("3222", "3092")
)


def load_script(name: str, file_name: str):
    specification = importlib.util.spec_from_file_location(
        name, PROJECT_ROOT / "scripts" / file_name
    )
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {file_name}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


# The check script reuses the solver loaded under this module name.
SOLVER = load_script("mocktail_roblox_abi_solver", "roblox_abi_solver.py")
CHECK = load_script("mocktail_roblox_abi_check", "roblox_abi_check.py")
DERIVER = SOLVER.DERIVER


class CandidateChoiceTest(unittest.TestCase):
    def test_accepts_a_clear_leader(self) -> None:
        self.assertEqual(SOLVER.choose_candidate([(0.80, 0x10), (0.41, 0x20)]), 0x10)

    def test_rejects_a_near_tie(self) -> None:
        self.assertIsNone(SOLVER.choose_candidate([(0.80, 0x10), (0.70, 0x20)]))

    def test_order_of_the_scores_does_not_matter(self) -> None:
        self.assertEqual(SOLVER.choose_candidate([(0.41, 0x20), (0.80, 0x10)]), 0x10)

    def test_a_lead_of_exactly_the_threshold_is_accepted(self) -> None:
        self.assertEqual(SOLVER.choose_candidate([(0.70, 0x10), (0.50, 0x20)]), 0x10)

    def test_a_single_candidate_still_needs_the_floor(self) -> None:
        self.assertEqual(SOLVER.choose_candidate([(0.80, 0x10)]), 0x10)
        self.assertIsNone(SOLVER.choose_candidate([(0.50, 0x10)]))
        self.assertIsNone(SOLVER.choose_candidate([]))


class InstructionAlignmentTest(unittest.TestCase):
    def test_maps_matching_instructions_across_an_insertion(self) -> None:
        self.assertEqual(
            SOLVER.align_instructions(["push", "mov", "ret"], ["push", "nop", "mov", "ret"]),
            {0: 0, 1: 2, 2: 3},
        )

    def test_leaves_replaced_instructions_unmapped(self) -> None:
        self.assertEqual(
            SOLVER.align_instructions(["push", "add", "ret"], ["push", "sub", "ret"]),
            {0: 0, 2: 2},
        )


class VtableTieBreakTest(unittest.TestCase):
    def test_breaks_a_tie_between_candidates_that_pass_on_their_own(self) -> None:
        scored = [(0.90, 0x10), (0.85, 0x20)]
        self.assertIsNone(SOLVER.choose_candidate(scored))
        self.assertEqual(SOLVER.choose_candidate(scored, tie_breaker=0x20), 0x20)

    def test_cannot_lift_a_candidate_below_the_floor(self) -> None:
        scored = [(0.90, 0x10), (0.85, 0x20), (0.50, 0x30)]
        self.assertIsNone(SOLVER.choose_candidate(scored, tie_breaker=0x30))

    def test_cannot_overrule_a_clear_leader(self) -> None:
        scored = [(0.95, 0x10), (0.65, 0x20)]
        self.assertEqual(SOLVER.choose_candidate(scored, tie_breaker=0x20), 0x10)

    def test_a_missing_slot_candidate_costs_nothing(self) -> None:
        scored = [(0.90, 0x10), (0.30, 0x20)]
        self.assertEqual(SOLVER.choose_candidate(scored, tie_breaker=0x99), 0x10)


def function(size: int = 32, shape: bytes = b"", calls=(), refs=()) -> object:
    return SOLVER.INDEX.FunctionInfo(size, shape, tuple(calls), tuple(refs))


class FakeIndex:
    """The parts of FunctionIndex the solver reads, from hand-made data."""

    def __init__(self, functions, data_users=None, instructions=None, image=None):
        self.functions = functions
        self.starts = sorted(functions)
        self.callers = SOLVER.INDEX.link_functions(functions)[0]
        self.data_users = data_users or {}
        self._instructions = instructions or {}
        self.image = image

    def instructions(self, start):
        return self._instructions[start]


def call(address: int, target: int):
    return SOLVER.Instruction(address, "call", (), target)


def plain(address: int):
    return SOLVER.Instruction(address, "nop ", (), None)


class FunctionLinkTest(unittest.TestCase):
    def test_lists_each_caller_and_data_user_once(self) -> None:
        callers, data_users = SOLVER.INDEX.link_functions({
            0x100: function(calls=(0x300, 0x300, 0x300), refs=(0x900, 0x900)),
            0x200: function(calls=(0x300,), refs=(0x900,)),
            0x300: function(),
        })
        self.assertEqual(callers[0x300], [0x100, 0x200])
        self.assertEqual(data_users[0x900], [0x100, 0x200])


class DataPatternTest(unittest.TestCase):
    def solver(self, refs, data_users):
        solver = SOLVER.Solver.from_indexes(
            FakeIndex({0x100: function(refs=refs)}), FakeIndex({}, data_users)
        )
        solver.data_map = {target: target + 0x1000 for target in refs}
        return solver

    def test_an_address_used_by_too_many_functions_casts_no_votes(self) -> None:
        hot_users = list(range(0x2000, 0x2001 + SOLVER.XREF_FANOUT_LIMIT))
        solver = self.solver((0x9000, 0x9100), {0xA000: hot_users, 0xA100: [0x3000]})
        self.assertEqual(solver._pattern_candidates(0x100), {0x3000})

    def test_a_common_address_still_backs_a_candidate_found_by_a_rare_one(self) -> None:
        hot_users = [0x3000, *range(0x2000, 0x2001 + SOLVER.XREF_FANOUT_LIMIT)]
        refs = (0x9000, 0x9100, 0x9200, 0x9300)
        solver = self.solver(refs, {0xA000: hot_users, 0xA100: [0x3000]})
        self.assertEqual(solver._pattern_candidates(0x100), {0x3000})

    def test_a_repeated_reference_casts_one_vote(self) -> None:
        refs = (0x9000, 0x9100, 0x9200, 0x9300)
        solver = self.solver(
            refs, {0xA000: [0x4000, 0x4000, 0x4000], 0xA100: [0x5000], 0xA200: [0x5000]}
        )
        self.assertEqual(solver._pattern_candidates(0x100), {0x5000})


class OneToOnePairTest(unittest.TestCase):
    def test_call_propagation_never_pairs_two_functions_with_one(self) -> None:
        reference = FakeIndex({
            0x100: function(shape=b"A", calls=(0x200, 0x300)),
            0x200: function(shape=b"x1"),
            0x300: function(shape=b"x2"),
        })
        candidate = FakeIndex({
            0x1100: function(shape=b"A", calls=(0x1200, 0x1200)),
            0x1200: function(shape=b"c"),
        })
        solver = SOLVER.Solver.from_indexes(reference, candidate)
        self.assertEqual(solver.pairs, {0x100: 0x1100, 0x200: 0x1200})

    def test_a_new_pair_replaces_the_one_holding_its_candidate(self) -> None:
        solver = SOLVER.Solver.from_indexes(FakeIndex({}), FakeIndex({}))
        solver._pair(0x200, 0x1200)
        solver._pair(0x300, 0x1200)
        self.assertEqual(solver.pairs, {0x300: 0x1200})


class RegistryAllocatorTest(unittest.TestCase):
    def solver(self, candidate_call_index: int):
        reference_code = [plain(0x100 + i) for i in range(12)]
        reference_code[9] = call(0x109, 0x500)
        candidate_code = [plain(0x1100 + i) for i in range(12)]
        candidate_code[candidate_call_index] = call(0x1100 + candidate_call_index, 0x1500)
        return SOLVER.Solver.from_indexes(
            FakeIndex({0x100: function()}, instructions={0x100: reference_code}),
            FakeIndex({0x1100: function()}, instructions={0x1100: candidate_code}),
        )

    def test_accepts_the_allocator_call_at_the_tenth_instruction(self) -> None:
        self.solver(9)._check_registry_allocator(0x100, 0x1100, 0x500, 0x1500)

    def test_rejects_an_allocator_call_anywhere_else(self) -> None:
        with self.assertRaisesRegex(SOLVER.AnalyzerError, "allocator"):
            self.solver(3)._check_registry_allocator(0x100, 0x1100, 0x500, 0x1500)

    def test_a_reference_profile_without_its_free_bridge_is_rejected(self) -> None:
        build = load_regression_build("3092")
        sidecar = regression_sidecar(build)
        sidecar["profile"]["bridge_entries"] = [
            entry for entry in sidecar["profile"]["bridge_entries"] if entry["label"] != "free"
        ]
        solver = SOLVER.Solver.from_indexes(FakeIndex({}), FakeIndex({}))
        with self.assertRaisesRegex(SOLVER.AnalyzerError, "allocator bridges"):
            solver.derive_profile(sidecar)


FMOD_FIELDS = {
    "vtable_rva": "0x1000",
    "string_constructor_rva": "0x10",
    "count_method_rva": "0x20",
    "info_method_rva": "0x30",
    "current_method_rva": "0x40",
    "select_method_rva": "0x50",
}
FMOD_INPUT_FIELDS = DERIVER.FMOD_INPUT_FIELDS


class FmodSourceTest(unittest.TestCase):
    def test_accepts_both_layouts(self) -> None:
        self.assertEqual(SOLVER.validate_fmod_source(FMOD_FIELDS), 1)
        self.assertEqual(
            SOLVER.validate_fmod_source({**FMOD_FIELDS, "vtable_layout_version": 2}), 2
        )

    def test_rejects_a_partial_input_profile(self) -> None:
        source = {**FMOD_FIELDS, FMOD_INPUT_FIELDS[0]: "0x60"}
        with self.assertRaisesRegex(SOLVER.AnalyzerError, "incomplete"):
            SOLVER.validate_fmod_source(source)

    def test_rejects_a_missing_output_field(self) -> None:
        source = dict(FMOD_FIELDS)
        del source["select_method_rva"]
        with self.assertRaisesRegex(SOLVER.AnalyzerError, "incomplete"):
            SOLVER.validate_fmod_source(source)

    def test_rejects_an_unknown_layout(self) -> None:
        for layout in (3, "2", True):
            with self.subTest(layout=layout), self.assertRaisesRegex(
                SOLVER.AnalyzerError, "layout"
            ):
                SOLVER.validate_fmod_source({**FMOD_FIELDS, "vtable_layout_version": layout})

    def test_finds_a_vtable_that_moved_to_the_other_layout(self) -> None:
        vtable = 0x8000
        relocations = {vtable + 5 * 8: 0xA, vtable + 6 * 8: 0xB, vtable + 8 * 8: 0xC}
        methods = {"count_method_rva": 0xA, "info_method_rva": 0xB, "current_method_rva": 0xC}
        self.assertEqual(
            SOLVER.Solver._locate_fmod_vtables(relocations, methods), [(vtable, 2)]
        )


class EhFrameHeaderTest(unittest.TestCase):
    def test_rejects_an_eight_byte_frame_pointer(self) -> None:
        # Version 1, eh_frame_ptr udata8, count udata4, table datarel sdata4.
        raw = bytes((1, 0x04, 0x03, 0x3B)) + bytes(16)
        header = types.SimpleNamespace(address=0x1000)
        image = types.SimpleNamespace(
            sections={".eh_frame_hdr": header}, section_bytes=lambda section: raw
        )
        with self.assertRaisesRegex(SOLVER.AnalyzerError, "eh_frame_ptr"):
            SOLVER.INDEX.function_table(image)


class FmodLayoutTieTest(unittest.TestCase):
    def test_resolves_current_and_select_from_the_located_vtable(self) -> None:
        reference_vtable, vtable = 0x1000, 0x8000
        source = {
            "vtable_rva": hex(reference_vtable), "string_constructor_rva": "0x10",
            "count_method_rva": "0x20", "info_method_rva": "0x30",
            "current_method_rva": "0x40", "select_method_rva": "0x50",
        }
        reference_relocations = {
            reference_vtable + slot * 8: method
            for slot, method in ((5, 0x20), (6, 0x30), (7, 0x40), (17, 0x50))
        }
        candidate_relocations = {
            vtable + slot * 8: method
            for slot, method in ((5, 0xA), (6, 0xB), (7, 0xC), (17, 0xD))
        }
        reference_image = types.SimpleNamespace(require_relro_rva=lambda *_: None)
        candidate_image = types.SimpleNamespace(require_relro_rva=lambda *_: None)
        solver = SOLVER.Solver.from_indexes(
            FakeIndex({}, image=reference_image), FakeIndex({}, image=candidate_image)
        )
        first_pass = {
            "fmod-count_method_rva": 0xA,
            "fmod-info_method_rva": 0xB,
            "fmod-string-constructor": 0x99,
        }

        def match(name, start, contract=None, vtable_slot=None):
            if name in first_pass:
                return first_pass[name]
            if vtable_slot is not None:
                return vtable_slot
            raise SOLVER.AnalyzerError(f"{name} has no clear candidate")

        with mock.patch.object(solver, "match_function", side_effect=match), mock.patch.object(
            DERIVER,
            "relative_relocation_map",
            side_effect=lambda image: (
                reference_relocations if image is reference_image else candidate_relocations
            ),
        ), mock.patch.object(
            DERIVER, "has_fmod_string_constructor_contract", return_value=True
        ), mock.patch.object(
            DERIVER, "has_fmod_device_lists_select_contract", return_value=False
        ):
            result = solver._derive_fmod_bridge(source)
        self.assertEqual(result["vtable_rva"], DERIVER.format_rva(vtable))
        self.assertEqual(result["current_method_rva"], DERIVER.format_rva(0xC))
        self.assertEqual(result["select_method_rva"], DERIVER.format_rva(0xD))
        self.assertNotIn("vtable_layout_version", result)


def regression_inputs(reference_name: str, candidate_name: str):
    """Both fixtures and libraries, or None when either library is missing."""
    reference = load_regression_build(reference_name)
    candidate = load_regression_build(candidate_name)
    reference_library = regression_library(reference)
    candidate_library = regression_library(candidate)
    if reference_library is None or candidate_library is None:
        return None
    return reference, candidate, reference_library, candidate_library


def regression_images(test: unittest.TestCase, reference_name: str, candidate_name: str):
    inputs = regression_inputs(reference_name, candidate_name)
    if inputs is None:
        test.skipTest(f"exact {reference_name} and {candidate_name} payloads are unavailable")
    return inputs


@unittest.skipUnless(DERIVER.capstone is not None, "Python capstone is unavailable")
class InputCheckTest(unittest.TestCase):
    def test_rejects_inputs_the_deriver_rejects(self) -> None:
        reference, _, reference_library, candidate_library = regression_images(
            self, "3092", "3222"
        )
        sidecar = regression_sidecar(reference)
        with DERIVER.ElfImage(reference_library) as reference_image, DERIVER.ElfImage(
            candidate_library
        ) as candidate_image:
            SOLVER.check_inputs(reference_image, candidate_image, sidecar)
            with self.assertRaisesRegex(SOLVER.AnalyzerError, "distinct"):
                SOLVER.check_inputs(reference_image, reference_image, sidecar)
            wrong = {**sidecar, "elf_build_id": "00" * 20}
            with self.assertRaisesRegex(SOLVER.AnalyzerError, "identity"):
                SOLVER.check_inputs(reference_image, candidate_image, wrong)
            solver = SOLVER.Solver.from_indexes(
                FakeIndex({}, image=reference_image), FakeIndex({}, image=candidate_image)
            )
            with self.assertRaisesRegex(SOLVER.AnalyzerError, "identity"):
                solver.derive_profile(wrong)


@unittest.skipUnless(DERIVER.capstone is not None, "Python capstone is unavailable")
class FunctionIndexCacheTest(unittest.TestCase):
    def library(self) -> Path:
        library = regression_library(load_regression_build("3222"))
        if library is None:
            self.skipTest("exact 3222 payload is unavailable")
        return library

    def test_reuses_a_cached_index_without_decoding(self) -> None:
        with tempfile.TemporaryDirectory() as cache, DERIVER.ElfImage(self.library()) as image:
            built = SOLVER.FunctionIndex(image, Path(cache))
            self.assertEqual(len(list(Path(cache).iterdir())), 1)
            with mock.patch.object(
                SOLVER.INDEX, "decode_function", side_effect=AssertionError("decoded")
            ):
                loaded = SOLVER.FunctionIndex(image, Path(cache))
            self.assertEqual(loaded.functions, built.functions)
            self.assertEqual(loaded.callers, built.callers)
            self.assertEqual(loaded.data_users, built.data_users)

    def test_rebuilds_a_damaged_cache(self) -> None:
        with tempfile.TemporaryDirectory() as cache, DERIVER.ElfImage(self.library()) as image:
            built = SOLVER.FunctionIndex(image, Path(cache))
            (entry,) = Path(cache).iterdir()
            entry.write_bytes(b"not an index")
            self.assertEqual(SOLVER.FunctionIndex(image, Path(cache)).functions, built.functions)

    def test_a_new_disassembler_version_gets_a_new_entry(self) -> None:
        before = SOLVER.INDEX.cache_path(Path("/cache"), "ab" * 32)
        with mock.patch.object(DERIVER.capstone, "__version__", "0.0.0"):
            after = SOLVER.INDEX.cache_path(Path("/cache"), "ab" * 32)
        self.assertNotEqual(before, after)


class CheckReportTest(unittest.TestCase):
    def test_lists_each_differing_field_by_path(self) -> None:
        derived = {"profile": {"a": "0x1", "b": ["0x2", "0x3"]}, "runtime": {"c": "0x4"}}
        solved = {"profile": {"a": "0x1", "b": ["0x2", "0x9"]}, "runtime": {}}
        self.assertEqual(
            CHECK.compare_results(derived, solved),
            ["profile.b[1]: deriver 0x3, solver 0x9", "runtime.c: deriver 0x4, solver missing"],
        )

    def test_exit_status_follows_agreement(self) -> None:
        same = {"profile": {"a": "0x1"}}
        self.assertEqual(CHECK.check_status(same, same, None, None)[0], 0)
        self.assertEqual(CHECK.check_status(same, {"profile": {}}, None, None)[0], 1)
        status, lines = CHECK.check_status(None, same, "no unique match", None)
        self.assertEqual(status, 1)
        self.assertIn("deriver failed: no unique match", lines)
        self.assertEqual(CHECK.check_status(None, None, "a", "b")[0], 2)


@unittest.skipUnless(DERIVER.capstone is not None, "Python capstone is unavailable")
class CheckCommandTest(unittest.TestCase):
    def run_check(self, reference: dict, reference_library, candidate_library):
        with tempfile.TemporaryDirectory() as temporary:
            sidecar = Path(temporary) / "reference.json"
            sidecar.write_text(json.dumps(regression_sidecar(reference)), encoding="utf-8")
            manifest = Path(temporary) / "compatibility.json"
            manifest.write_text(json.dumps(regression_manifest(reference)), encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = CHECK.main([
                    "--reference-lib", str(reference_library),
                    "--reference-profile", str(sidecar),
                    "--reference-compatibility", str(manifest),
                    "--candidate-lib", str(candidate_library),
                    "--cache-dir", str(REGRESSION_CACHE / "function_index"),
                ])
        return status, output.getvalue()

    def test_reports_agreement_on_a_real_update(self) -> None:
        reference, _, reference_library, candidate_library = regression_images(
            self, "2998", "3092"
        )
        status, output = self.run_check(reference, reference_library, candidate_library)
        self.assertEqual(status, 0, output)
        self.assertIn("deriver and solver agree", output)

    def test_rejects_a_wrong_reference_before_indexing(self) -> None:
        reference, _, _, candidate_library = regression_images(self, "2998", "3092")
        third = regression_library(load_regression_build("3222"))
        if third is None:
            self.skipTest("exact 3222 payload is unavailable")
        with mock.patch.object(
            SOLVER, "FunctionIndex", side_effect=AssertionError("indexed")
        ):
            status, output = self.run_check(reference, third, candidate_library)
        self.assertEqual(status, 2, output)
        self.assertIn("solver failed: reference ELF bytes do not match", output)

    def test_reports_any_error_as_that_side_failing(self) -> None:
        reference, _, reference_library, candidate_library = regression_images(
            self, "2998", "3092"
        )
        with mock.patch.object(
            DERIVER, "derive_profile", side_effect=ValueError("bad slot")
        ), mock.patch.object(
            SOLVER.Solver, "derive_profile", side_effect=KeyError("free")
        ), mock.patch.object(SOLVER, "FunctionIndex", return_value=FakeIndex({})):
            status, output = self.run_check(reference, reference_library, candidate_library)
        self.assertEqual(status, 2, output)
        self.assertIn("deriver failed: ValueError: bad slot", output)
        self.assertIn("solver failed: KeyError: 'free'", output)


# The smallest lead of any match the score decided alone, per pair. A
# change that brings matches closer to a tie fails here first.
WEAKEST_SCORE_LEAD = {
    ("2908", "2998"): 0.22,
    ("2998", "3092"): 0.22,
    ("3092", "3222"): 0.22,
    ("3222", "3092"): 0.21,
}


@unittest.skipUnless(DERIVER.capstone is not None, "Python capstone is unavailable")
class SolverRegressionTest(unittest.TestCase):
    def test_solves_each_build_from_its_predecessor(self) -> None:
        for reference_name, candidate_name in REGRESSION_PAIRS:
            with self.subTest(reference=reference_name, candidate=candidate_name):
                self.solve_pair(reference_name, candidate_name)

    def solve_pair(self, reference_name: str, candidate_name: str) -> None:
        reference, candidate, reference_library, candidate_library = regression_images(
            self, reference_name, candidate_name
        )
        with DERIVER.ElfImage(reference_library) as reference_image, DERIVER.ElfImage(
            candidate_library
        ) as candidate_image:
            solver = SOLVER.Solver(
                reference_image, candidate_image, REGRESSION_CACHE / "function_index"
            )
            profile, anchors = solver.derive_profile(regression_sidecar(reference))
            runtime = solver.derive_runtime_compatibility(reference["runtime_compatibility"])
        self.assertEqual(profile, candidate["profile"])
        self.assertEqual(anchors, candidate["derivation_anchors"])
        self.assertEqual(runtime, expected_runtime(reference, candidate))
        weakest = min(
            match.lead for match in solver.matches.values() if match.decided_by == "score"
        )
        self.assertGreaterEqual(
            round(weakest, 2), WEAKEST_SCORE_LEAD[(reference_name, candidate_name)]
        )


if __name__ == "__main__":
    unittest.main()
