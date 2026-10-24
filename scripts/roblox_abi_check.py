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

"""Run the HostAbi deriver and the solver on one update and compare them.

Usage:
    python3 roblox_abi_check.py --reference-lib OLD.so \\
        --reference-profile OLD_PROFILE.json \\
        --reference-compatibility config/roblox_compatibility.json \\
        --candidate-lib NEW.so

Prints every field where the two disagree. Exit status: 0 agree, 1 differ or
one side failed, 2 both failed.

With --vr, the VR device bridge is compared too. `find_roblox_vr_offsets.py`
finds it in each build on its own: its answer for the reference build is the
solver's input, and its answer for the candidate joins the deriver's side.
"""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys
from typing import Any, Sequence


def _load_sibling(name: str, file_name: str):
    if name in sys.modules:
        return sys.modules[name]
    path = Path(__file__).resolve().with_name(file_name)
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {file_name}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


SOLVER = _load_sibling("mocktail_roblox_abi_solver", "roblox_abi_solver.py")
FINDER = _load_sibling("mocktail_find_roblox_vr_offsets", "find_roblox_vr_offsets.py")
DERIVER = SOLVER.DERIVER
AnalyzerError = SOLVER.AnalyzerError
format_rva = SOLVER.format_rva

# Where find_roblox_vr_offsets.py reports each VR bridge field.
_FINDER_FIELDS = {
    "vtable_rva": ("vr_debug_device_bridge", "vtable_rva"),
    "constructor_rva": ("vr_debug_device_bridge", "constructor_rva"),
    "state_getter_rva": ("vr_debug_device_bridge", "state_getter_rva"),
    "eye_getter_rva": ("vr_debug_device_bridge", "eye_getter_rva"),
    "eye_initializer_rva": ("vr_debug_device_bridge", "eye_initializer_rva"),
    "emulator_flag_storage_rva": ("vr_debug_device_bridge", "emulator_flag_storage_rva"),
    "device_create_framebuffer_vtable_offset": (
        "vr_debug_device_bridge", "device_create_framebuffer_vtable_offset"
    ),
    "state_memcpy_rva": ("vr_bridge_supplement", "state_getter_memcpy_rva"),
    "pointer_frame_rva": ("vr_bridge_supplement", "world_pointer_getter_rva"),
    "workspace_getter_rva": ("vr_bridge_supplement", "workspace_getter_rva"),
    "camera_vtable_offset": ("vr_bridge_supplement", "workspace_camera_vtable_offset"),
    "hand_pitch_storage_rva": ("vr_bridge_supplement", "new_vr_system_hand_pitch_storage_rva"),
    "haptic_sink_rva": ("vr_bridge_supplement", "haptic_sink_rva"),
    "pointer_hand_selector_offset": ("vr_bridge_supplement", "usercframe_selector_offset"),
    "camera_head_scale_offset": ("vr_bridge_supplement", "camera_head_scale_offset"),
    "keep_haptics_flag_rva": ("vr_bridge_supplement", "keep_haptics_flag_rva"),
    "vr_haptics_flag_rva": ("vr_bridge_supplement", "vr_haptics_flag_rva"),
    "support_generic_haptics_flag_rva": (
        "vr_bridge_supplement", "support_generic_haptics_flag_rva"
    ),
    "fix_register_motors_flag_rva": ("vr_bridge_supplement", "fix_register_motors_flag_rva"),
}


def vr_bridge_from_finder(
    result: dict[str, Any], fields: Sequence[str] = SOLVER.VR.VR_FIELDS
) -> dict[str, str]:
    """The VR bridge fields from a find_roblox_vr_offsets.py result."""
    bridge = {}
    for field in fields:
        section, key = _FINDER_FIELDS[field]
        try:
            bridge[field] = result[section][key]
        except (KeyError, TypeError) as error:
            raise AnalyzerError(f"VR offset finder result lacks {key}") from error
    return bridge


def _find_vr_bridge(library: Path) -> dict[str, str]:
    try:
        return vr_bridge_from_finder(FINDER.find_vr_offsets(library))
    except FINDER.AnalyzerError as error:
        raise AnalyzerError(f"VR offset finder: {error}") from error


def compare_results(derived: Any, solved: Any, path: str = "") -> list[str]:
    """Returns one line for every leaf value where the two results differ."""
    if isinstance(derived, dict) and isinstance(solved, dict):
        lines = []
        for key in sorted(derived.keys() | solved.keys(), key=str):
            child = f"{path}.{key}" if path else str(key)
            if key not in solved:
                lines.append(f"{child}: deriver {derived[key]}, solver missing")
            elif key not in derived:
                lines.append(f"{child}: deriver missing, solver {solved[key]}")
            else:
                lines.extend(compare_results(derived[key], solved[key], child))
        return lines
    if isinstance(derived, list) and isinstance(solved, list) and len(derived) == len(solved):
        return [
            line
            for index, (left, right) in enumerate(zip(derived, solved))
            for line in compare_results(left, right, f"{path}[{index}]")
        ]
    return [] if derived == solved else [f"{path}: deriver {derived}, solver {solved}"]


def check_status(
    derived: Any, solved: Any, deriver_error: str | None, solver_error: str | None
) -> tuple[int, list[str]]:
    """Exit status and report lines: 0 agree, 1 differ or one failed, 2 both failed."""
    lines = []
    if deriver_error is not None:
        lines.append(f"deriver failed: {deriver_error}")
    if solver_error is not None:
        lines.append(f"solver failed: {solver_error}")
    if deriver_error is not None and solver_error is not None:
        return 2, lines
    if deriver_error is not None or solver_error is not None:
        return 1, lines
    differences = compare_results(derived, solved)
    if differences:
        return 1, differences
    return 0, ["deriver and solver agree on every field"]


def _solver_runtime_source(source: dict[str, Any]) -> dict[str, Any]:
    # The deriver's loader returns integers; the solver reads hex strings.
    result: dict[str, Any] = {}
    for key, value in source.items():
        if isinstance(value, dict):
            result[key] = {
                field: item if field == "vtable_layout_version" else format_rva(item)
                for field, item in value.items()
            }
        else:
            result[key] = format_rva(value)
    return result


def _describe(error: Exception) -> str:
    if isinstance(error, AnalyzerError):
        return str(error)
    return f"{type(error).__name__}: {error}"


def check(arguments: argparse.Namespace) -> int:
    """Runs the deriver and the solver on one update and compares them."""
    sidecar = DERIVER.validated_sidecar(Path(arguments.reference_profile))
    manifests = [Path(path) for path in arguments.reference_compatibility]
    cache_dir = Path(arguments.cache_dir) if arguments.cache_dir else None
    derived = solved = deriver_error = solver_error = None
    reference_vr = _find_vr_bridge(Path(arguments.reference_lib)) if arguments.vr else None
    with DERIVER.ElfImage(Path(arguments.reference_lib)) as reference, DERIVER.ElfImage(
        Path(arguments.candidate_lib)
    ) as candidate:
        try:
            profile, anchors = DERIVER.derive_profile(reference, candidate, sidecar)
            runtime = DERIVER.derive_runtime_compatibility(reference, candidate, manifests)
            derived = {"profile": profile, "derivation_anchors": anchors, "runtime": runtime}
            if reference_vr is not None:
                derived["vr_device_bridge"] = _find_vr_bridge(Path(arguments.candidate_lib))
        except Exception as error:
            deriver_error = _describe(error)
        try:
            # The input checks are cheap; indexing both libraries is not.
            SOLVER.check_inputs(reference, candidate, sidecar)
            solver = SOLVER.Solver(reference, candidate, cache_dir)
            profile, anchors = solver.derive_profile(sidecar)
            source = DERIVER.load_reference_runtime_profile(manifests, reference.build_id)
            runtime = solver.derive_runtime_compatibility(_solver_runtime_source(source))
            solved = {"profile": profile, "derivation_anchors": anchors, "runtime": runtime}
            if reference_vr is not None:
                solved["vr_device_bridge"] = solver.derive_vr(reference_vr)
        except Exception as error:
            solver_error = _describe(error)
    status, lines = check_status(derived, solved, deriver_error, solver_error)
    for line in lines:
        print(line)
    return status


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run the HostAbi deriver and the solver on one update and compare them"
    )
    parser.add_argument("--reference-lib", required=True, metavar="REF")
    parser.add_argument("--reference-profile", required=True, metavar="REFJSON")
    parser.add_argument(
        "--reference-compatibility", action="append", default=[], metavar="MANIFEST"
    )
    parser.add_argument("--candidate-lib", required=True, metavar="LIB")
    parser.add_argument(
        "--cache-dir",
        default=str(Path.home() / ".cache" / "mocktail" / "function_index"),
        help="where function indexes are kept; empty to disable",
    )
    parser.add_argument(
        "--vr", action="store_true", help="also compare the VR device bridge"
    )
    arguments = parser.parse_args(sys.argv[1:] if argv is None else argv)
    try:
        return check(arguments)
    except AnalyzerError as error:
        print(f"roblox-abi-check: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
