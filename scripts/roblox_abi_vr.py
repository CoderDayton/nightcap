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

"""VR device bridge targets that `roblox_abi_solver.py` derives.

`VrTargets.derive_vr` takes one build's VR bridge (the `VrBuildContract`
fields in `src/vr/roblox_vr_device_bridge.cc`, as hex strings) and returns
the same fields for the candidate build.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import sys
from typing import Any


def _load_index():
    name = "mocktail_roblox_function_index"
    if name in sys.modules:
        return sys.modules[name]
    path = Path(__file__).resolve().with_name("roblox_function_index.py")
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load roblox_function_index.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


DERIVER = _load_index().DERIVER
AnalyzerError = DERIVER.AnalyzerError
format_rva = DERIVER.format_rva

VR_FIELDS = (
    "vtable_rva",
    "constructor_rva",
    "state_getter_rva",
    "eye_getter_rva",
    "eye_initializer_rva",
    "emulator_flag_storage_rva",
    "device_create_framebuffer_vtable_offset",
    "state_memcpy_rva",
    "pointer_frame_rva",
    "workspace_getter_rva",
    "camera_vtable_offset",
    "hand_pitch_storage_rva",
    "haptic_sink_rva",
    "pointer_hand_selector_offset",
    "camera_head_scale_offset",
    "keep_haptics_flag_rva",
    "vr_haptics_flag_rva",
    "support_generic_haptics_flag_rva",
    "fix_register_motors_flag_rva",
)
# Fast flag and setting storage, found through the functions that use it.
_STORAGE_FIELDS = (
    "emulator_flag_storage_rva",
    "hand_pitch_storage_rva",
    "keep_haptics_flag_rva",
    "vr_haptics_flag_rva",
    "support_generic_haptics_flag_rva",
    "fix_register_motors_flag_rva",
)
# Storage users tried, smallest first; registration functions are small.
_STORAGE_USER_LIMIT = 8
_VTABLE_SLOT_LIMIT = 0x400
# The constructor stores the vtable address plus the Itanium header.
_VTABLE_REFERENCE_SPAN = 0x40
_NUMBER = r"(0x[0-9a-f]+|\d+)"
_SLOT_LOAD = re.compile(r"^mov (\w+), qword ptr \[\1 \+ ")
# How far back the load of the vtable pointer itself may sit.
_VTABLE_LOAD_WINDOW = 6


def _vtable_call(instructions: list[Any], index: int) -> bool:
    """A call through a vtable slot, or a load of the slot from a vtable
    pointer loaded just before into the same register."""
    shape = instructions[index].shape
    if shape.startswith("call qword ptr ["):
        return True
    load = _SLOT_LOAD.match(shape)
    if load is None:
        return False
    vtable_load = re.compile(rf"^mov {load.group(1)}, qword ptr \[\w+\]$")
    return any(
        vtable_load.match(previous.shape)
        for previous in instructions[max(0, index - _VTABLE_LOAD_WINDOW) : index]
    )


class VrTargets:
    """Mixin for `Solver`: needs `reference`, `candidate`, `matches`,
    `match_function`, `map_data` and `_aligned`."""

    def map_operand(
        self, name: str, function_name: str, start: int, value: int, vtable_call: bool = False
    ) -> int:
        """Maps a field offset that a matched function uses as a displacement.

        Stack-relative uses are ignored; with `vtable_call`, so is every use
        that is not a virtual call. An instruction whose shape changed is
        looked for between its nearest aligned neighbours.
        """
        reference, candidate, mapping = self._aligned(function_name, start)
        token = re.compile(rf"(?<![0-9a-fx]){re.escape(hex(value))}(?![0-9a-f])")
        found = set()
        for index, item in enumerate(reference):
            if not token.search(item.shape) or "rsp" in item.shape or "rbp" in item.shape:
                continue
            if vtable_call and not _vtable_call(reference, index):
                continue
            pattern = re.compile(
                "^" + token.sub(lambda _: _NUMBER, re.escape(item.shape)) + "$"
            )
            if index in mapping:
                window = range(mapping[index], mapping[index] + 1)
            else:
                below = max((k for k in mapping if k < index), default=None)
                above = min((k for k in mapping if k > index), default=None)
                low = 0 if below is None else mapping[below] + 1
                high = len(candidate) if above is None else mapping[above]
                window = range(low, high)
            values = []
            for position in window:
                if vtable_call and not _vtable_call(candidate, position):
                    continue
                matched = pattern.match(candidate[position].shape)
                if matched is not None and len({*matched.groups()}) == 1:
                    values.append(int(matched.group(1), 0))
            if len(values) != 1:
                raise AnalyzerError(f"{name} has {len(values)} candidate offsets")
            found.add(values[0])
        if len(found) != 1:
            raise AnalyzerError(f"{name} maps to {len(found)} offsets")
        return found.pop()

    def map_call_target(self, name: str, function_name: str, start: int, target: int) -> int:
        """Maps the target of a direct call inside a matched function."""
        reference, candidate, mapping = self._aligned(function_name, start)
        found = set()
        for index, item in enumerate(reference):
            if item.shape != "call" or item.branch_target != target:
                continue
            if index not in mapping:
                raise AnalyzerError(f"{name} call has no counterpart")
            found.add(candidate[mapping[index]].branch_target)
        if len(found) != 1 or None in found:
            raise AnalyzerError(f"{name} has no unique call target")
        return found.pop()

    def _map_storage(self, name: str, target: int) -> int:
        users = sorted(
            self.reference.data_users.get(target, ()),
            key=lambda start: self.reference.functions[start].size,
        )[:_STORAGE_USER_LIMIT]
        found = set()
        for user in users:
            user_name = f"{name} user {format_rva(user)}"
            try:
                self.match_function(user_name, user)
                found.add(self.map_data(name, user_name, user, target))
            except AnalyzerError:
                continue
        if len(found) != 1:
            raise AnalyzerError(f"{name} maps to {len(found)} addresses")
        return found.pop()

    def _map_vtable(self, constructor: int, vtable: int) -> int:
        found = set()
        for item in self.reference.instructions(constructor):
            for target in item.rip_targets:
                if 0 <= target - vtable < _VTABLE_REFERENCE_SPAN:
                    mapped = self.map_data("VR vtable", "vr-constructor", constructor, target)
                    found.add(mapped - (target - vtable))
        if len(found) != 1:
            raise AnalyzerError(f"VR vtable maps to {len(found)} addresses")
        return found.pop()

    @staticmethod
    def _slot(relocations: dict[int, int], vtable: int, method: int) -> int:
        slots = [
            offset
            for offset in range(0, _VTABLE_SLOT_LIMIT, 8)
            if relocations.get(vtable + offset) == method
        ]
        if not slots:
            raise AnalyzerError(f"reference VR vtable does not hold {format_rva(method)}")
        return slots[0]

    def derive_vr(self, source: dict[str, str]) -> dict[str, str]:
        missing = [field for field in VR_FIELDS if field not in source]
        if missing:
            raise AnalyzerError("reference VR bridge lacks " + ", ".join(missing))
        value = {field: DERIVER.parse_rva(source[field], field) for field in VR_FIELDS}
        result = {}
        constructor = self.match_function("vr-constructor", value["constructor_rva"])
        result["constructor_rva"] = constructor
        vtable = self._map_vtable(value["constructor_rva"], value["vtable_rva"])
        result["vtable_rva"] = vtable
        reference_relocations = DERIVER.relative_relocation_map(self.reference.image)
        relocations = DERIVER.relative_relocation_map(self.candidate.image)
        slots = {
            field: self._slot(reference_relocations, value["vtable_rva"], value[field])
            for field in (
                "state_getter_rva", "eye_getter_rva", "eye_initializer_rva", "haptic_sink_rva"
            )
        }
        self.candidate.image.require_relro_rva(vtable, max(slots.values()) + 8)
        for field in ("state_getter_rva", "eye_getter_rva", "eye_initializer_rva"):
            result[field] = self.match_function(
                f"vr-{field}", value[field], vtable_slot=relocations.get(vtable + slots[field])
            )
        # The haptic sink is a one-byte ret stub, so only its slot identifies it.
        sink = relocations.get(vtable + slots["haptic_sink_rva"])
        if sink is None or sink not in self.candidate.functions or (
            self.candidate.functions[sink].size != 1
        ):
            raise AnalyzerError("VR haptic sink slot does not hold a ret stub")
        result["haptic_sink_rva"] = sink
        result["state_memcpy_rva"] = self.map_call_target(
            "VR state memcpy", "vr-state_getter_rva",
            value["state_getter_rva"], value["state_memcpy_rva"],
        )
        result["device_create_framebuffer_vtable_offset"] = self.map_operand(
            "VR framebuffer slot", "vr-eye_initializer_rva",
            value["eye_initializer_rva"], value["device_create_framebuffer_vtable_offset"],
            vtable_call=True,
        )
        result["pointer_frame_rva"] = self.match_function(
            "vr-pointer-frame", value["pointer_frame_rva"]
        )
        result["workspace_getter_rva"] = self.map_call_target(
            "VR workspace getter", "vr-pointer-frame",
            value["pointer_frame_rva"], value["workspace_getter_rva"],
        )
        for field in (
            "camera_vtable_offset", "pointer_hand_selector_offset", "camera_head_scale_offset"
        ):
            result[field] = self.map_operand(
                field, "vr-pointer-frame", value["pointer_frame_rva"], value[field],
                vtable_call=field == "camera_vtable_offset",
            )
        for field in _STORAGE_FIELDS:
            result[field] = self._map_storage(field, value[field])
            self.candidate.image.require_writable_rva(result[field], 1)
        return {field: format_rva(result[field]) for field in VR_FIELDS}
