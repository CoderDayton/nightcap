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

"""HostAbi targets that `roblox_abi_solver.py` derives from its matches.

`TargetDerivation` turns matched functions into the HostAbi profile, the
derivation anchors and the runtime compatibility block, in the shape that
`derive_roblox_host_abi_profile.py` writes. It reuses the deriver's input
checks, contracts and post-processing.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from typing import Any, Callable


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
FMOD_OUTPUT_FIELDS = DERIVER.FMOD_OUTPUT_FIELDS
FMOD_INPUT_FIELDS = DERIVER.FMOD_INPUT_FIELDS
fmod_slots = DERIVER.fmod_vtable_slots


def check_inputs(reference: Any, candidate: Any, reference_sidecar: dict[str, Any]) -> None:
    """Applies the deriver's identity and preflight checks to both builds."""
    DERIVER.check_derivation_inputs(reference, candidate, reference_sidecar)


def validate_fmod_source(source: Any) -> int:
    """Checks a reference FMOD bridge the way the deriver does; returns its layout."""
    if (
        not isinstance(source, dict)
        or not FMOD_OUTPUT_FIELDS <= source.keys()
        or source.keys() - FMOD_OUTPUT_FIELDS - set(FMOD_INPUT_FIELDS) - {"vtable_layout_version"}
    ):
        raise AnalyzerError("reference FMOD output-device profile is incomplete")
    present = [field for field in FMOD_INPUT_FIELDS if field in source]
    if present and len(present) != len(FMOD_INPUT_FIELDS):
        raise AnalyzerError("reference FMOD input profile is incomplete")
    layout = source.get("vtable_layout_version", 1)
    if type(layout) is not int or layout not in (1, 2):
        raise AnalyzerError("reference FMOD vtable layout is unsupported")
    return layout


class TargetDerivation:
    """Mixin for `Solver`: needs `reference`, `candidate`, `matches`,
    `match_function`, `map_data` and `map_instruction`."""

    def _check_registry_allocator(
        self,
        reference_registry: int,
        candidate_registry: int,
        reference_allocate: int,
        candidate_allocate: int,
    ) -> None:
        # The registry initializer's tenth instruction calls the allocator bridge.
        for index, start, allocate, side in (
            (self.reference, reference_registry, reference_allocate, "reference"),
            (self.candidate, candidate_registry, candidate_allocate, "candidate"),
        ):
            code = index.instructions(start)
            if len(code) <= 9 or code[9].shape != "call" or code[9].branch_target != allocate:
                raise AnalyzerError(
                    f"{side} registry initializer does not call its allocator bridge"
                )

    def derive_profile(
        self, reference_sidecar: dict[str, Any]
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        reference_profile = reference_sidecar["profile"]
        anchors = reference_sidecar["derivation_anchors"]
        DERIVER.validate_profile_shape(reference_profile, anchors)
        reference_image = self.reference.image
        candidate_image = self.candidate.image
        check_inputs(reference_image, candidate_image, reference_sidecar)
        parse = DERIVER.parse_rva
        bridges = []
        found = {}
        for entry in reference_profile["bridge_entries"]:
            rva = self.match_function(entry["label"], parse(entry["rva"], entry["label"]))
            found[entry["label"]] = rva
            bridges.append({"rva": format_rva(rva), "kind": entry["kind"], "label": entry["label"]})

        seeds = reference_profile["data_seeds"]
        arena_initializer = parse(seeds["arena_initializer"], "arena initializer")
        thread_initializer = parse(seeds["allocator_thread_initializer"], "thread initializer")
        self.match_function("arena-initializer", arena_initializer)
        self.match_function("allocator-thread-initializer", thread_initializer)

        bootstrap = reference_profile["native_pre_jni_bootstrap"]
        DERIVER.validate_reference_registry_slot(reference_image, bootstrap)
        registry = self.match_function(
            "registry-initializer", parse(bootstrap["registry_initializer"], "registry")
        )
        reference_allocate = parse(
            DERIVER.bridge_map(reference_profile)["allocate"]["rva"], "reference allocate"
        )
        self._check_registry_allocator(
            parse(bootstrap["registry_initializer"], "registry"),
            registry, reference_allocate, found["allocate"],
        )
        registry_slot = DERIVER.derive_registry_slot(candidate_image, registry)

        reference_init = reference_image.init_array_relocations()
        candidate_init = candidate_image.init_array_relocations()
        begin = DERIVER.LEADING_CONSTRUCTOR_BEGIN
        count = DERIVER.LEADING_CONSTRUCTOR_COUNT
        leading = candidate_init[begin : begin + count]
        offsets = []
        for index in range(begin, begin + count):
            expected = parse(anchors["constructor_rvas"][str(index)], f"constructor {index}")
            if reference_init[index] != expected:
                raise AnalyzerError(f"reference constructor {index} differs from its anchor")
            rva = self.match_function(f"constructor-{index}", expected)
            if leading.count(rva) != 1:
                raise AnalyzerError(f"constructor {index} left the leading constructors")
            offsets.append(leading.index(rva))
        if sorted(offsets) != list(range(count)):
            raise AnalyzerError("leading constructors do not map one to one")

        singleton = parse(anchors["jni_singleton_initializer_rva"], "JNI singleton")
        jni_singleton = self.match_function("jni-singleton-initializer", singleton)
        allocator_anchor = parse(anchors["allocator_object_initializer_rva"], "allocator anchor")
        empty_anchor = parse(anchors["empty_string_initializer_rva"], "empty string anchor")
        allocator_function = self.reference.containing(allocator_anchor)
        empty_function = self.reference.containing(empty_anchor)
        if allocator_function is None or empty_function is None:
            raise AnalyzerError("reference anchor is outside every function")

        slots = {
            "allocator_object_slot": self.map_data(
                "allocator object slot", "allocator-object-initializer function",
                allocator_function, parse(seeds["allocator_object_slot"], "allocator slot"),
            ),
            "empty_string_slot": self.map_data(
                "empty string slot", "empty-string-initializer function",
                empty_function, parse(seeds["empty_string_slot"], "empty string slot"),
            ),
            "jni_singleton_slot": self.map_data(
                "JNI singleton slot", "jni-singleton-initializer",
                singleton, parse(seeds["jni_singleton_slot"], "JNI singleton slot"),
            ),
            "arena_guard_slot": self.map_data(
                "arena guard slot", "arena-initializer",
                arena_initializer, parse(seeds["arena_guard_slot"], "arena guard slot"),
            ),
            "arena_table_slot": self.map_data(
                "arena table slot", "usable-size",
                parse(DERIVER.bridge_map(reference_profile)["usable-size"]["rva"], "usable size"),
                parse(seeds["arena_table_slot"], "arena table slot"),
            ),
        }
        for slot in slots.values():
            candidate_image.require_writable_rva(slot, 8)

        candidate_count = len(candidate_init)
        reference_count = reference_profile["init_array_count"]
        boundary = reference_profile["native_mimalloc_thread_initializer_after_constructor"]
        if not begin <= boundary < begin + count:
            raise AnalyzerError("native allocator TLS boundary is no longer verified")
        thread_boundary = begin + offsets[boundary - begin]
        native_ranges = DERIVER.remapped_constructor_ranges(
            reference_profile["native_mimalloc_constructor_run_ranges"],
            offsets, reference_count, candidate_count,
        )
        if not any(item["begin"] <= thread_boundary < item["end_exclusive"] for item in native_ranges):
            raise AnalyzerError("native allocator TLS boundary is no longer verified")
        profile = {
            "elf_build_id": candidate_image.build_id,
            "bridge_entries": bridges,
            "data_seeds": {
                "allocator_object_slot": format_rva(slots["allocator_object_slot"]),
                "empty_string_slot": format_rva(slots["empty_string_slot"]),
                "jni_singleton_slot": format_rva(slots["jni_singleton_slot"]),
                "jni_singleton_bytes": seeds["jni_singleton_bytes"],
                "arena_initializer": format_rva(self.matches["arena-initializer"].rva),
                "allocator_thread_initializer": format_rva(
                    self.matches["allocator-thread-initializer"].rva
                ),
                "arena_guard_slot": format_rva(slots["arena_guard_slot"]),
                "arena_table_slot": format_rva(slots["arena_table_slot"]),
                "arena_table_slot_count": seeds["arena_table_slot_count"],
            },
            "native_allocator": {
                "allocate": format_rva(found["allocate"]),
                "deallocate": format_rva(found["free"]),
            },
            "init_array_offset": format_rva(candidate_image.sections[".init_array"].address),
            "init_array_count": candidate_count,
            "constructor_run_ranges": DERIVER.remapped_constructor_ranges(
                reference_profile["constructor_run_ranges"],
                offsets, reference_count, candidate_count,
            ),
            "native_mimalloc_constructor_run_ranges": native_ranges,
            "native_mimalloc_thread_initializer_after_constructor": thread_boundary,
            "native_pre_jni_bootstrap": {
                "registry_initializer": format_rva(registry),
                "registry_slot": format_rva(registry_slot),
            },
            "default_allocator_strategy": "native_mimalloc",
        }
        derived_anchors = {
            "signature_version": 1,
            "allocator_object_initializer_rva": format_rva(
                self.map_instruction("allocator-object-initializer", allocator_anchor)
            ),
            "empty_string_initializer_rva": format_rva(
                self.map_instruction("empty-string-initializer", empty_anchor)
            ),
            "jni_singleton_initializer_rva": format_rva(jni_singleton),
            "constructor_rvas": {
                str(index): format_rva(candidate_init[index])
                for index in range(begin, begin + count)
            },
        }
        DERIVER.validate_profile_shape(profile, derived_anchors)
        return profile, derived_anchors

    def derive_runtime_compatibility(self, source: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        fullscreen = source.get("user_game_settings_fullscreen_setter_rva")
        if fullscreen is not None:
            reference_setter = DERIVER.parse_rva(fullscreen, "fullscreen setter")
            if not DERIVER.has_fullscreen_setter_contract(self.reference.image, reference_setter):
                raise AnalyzerError("reference fullscreen setter contract changed")
            setter = self.match_function(
                "fullscreen-setter", reference_setter, DERIVER.has_fullscreen_setter_contract
            )
            result["user_game_settings_fullscreen_setter_rva"] = format_rva(setter)
        fmod = source.get("fmod_output_device_bridge")
        if fmod is not None:
            result["fmod_output_device_bridge"] = self._derive_fmod_bridge(fmod)
        return result

    def _derive_fmod_bridge(self, source: dict[str, Any]) -> dict[str, Any]:
        layout = validate_fmod_source(source)
        has_input = FMOD_INPUT_FIELDS[0] in source
        slots = fmod_slots(layout, has_input)
        parse = DERIVER.parse_rva
        vtable = parse(source["vtable_rva"], "FMOD vtable")
        reference_relocations = DERIVER.relative_relocation_map(self.reference.image)
        methods = {}
        unresolved = []
        for field, slot in slots.items():
            method = parse(source[field], field)
            if reference_relocations.get(vtable + slot * 8) != method:
                raise AnalyzerError("reference FMOD vtable does not match its methods")
            try:
                methods[field] = self.match_function(
                    f"fmod-{field}", method, self._fmod_contract(layout, field)
                )
            except AnalyzerError:
                unresolved.append(field)
        string_constructor = parse(source["string_constructor_rva"], "FMOD string constructor")
        if not DERIVER.has_fmod_string_constructor_contract(self.reference.image, string_constructor):
            raise AnalyzerError("reference FMOD string constructor contract changed")
        candidate_string_constructor = self.match_function(
            "fmod-string-constructor",
            string_constructor,
            DERIVER.has_fmod_string_constructor_contract,
        )

        relocations = DERIVER.relative_relocation_map(self.candidate.image)
        located = self._locate_fmod_vtables(relocations, methods)
        vtables = sorted({vtable for vtable, _ in located})
        if len(vtables) != 1:
            raise AnalyzerError(f"FMOD output vtable matched {len(vtables)} candidate locations")
        # The located vtable breaks ties for the methods that did not resolve.
        # Count and info share slots 5 and 6 in both layouts, so the vtable can
        # fit both; exactly one layout must then hold every method.
        solutions = []
        for located_vtable, target_layout in located:
            target_slots = fmod_slots(target_layout, has_input)
            resolved = dict(methods)
            try:
                for field in unresolved:
                    resolved[field] = self.match_function(
                        f"fmod-{field}",
                        parse(source[field], field),
                        self._fmod_contract(target_layout, field),
                        relocations.get(located_vtable + target_slots[field] * 8),
                    )
            except AnalyzerError:
                continue
            if self._locate_fmod_vtables(relocations, resolved) != [
                (located_vtable, target_layout)
            ]:
                continue
            if target_layout == 2 and not DERIVER.has_fmod_device_lists_select_contract(
                self.candidate.image, resolved["select_method_rva"]
            ):
                continue
            solutions.append((located_vtable, target_layout, target_slots, resolved))
        if len(solutions) != 1:
            raise AnalyzerError(f"FMOD output vtable holds every method in {len(solutions)} layouts")
        located_vtable, target_layout, target_slots, methods = solutions[0]
        self.candidate.image.require_relro_rva(
            located_vtable, (max(target_slots.values()) + 1) * 8
        )
        result = {"vtable_rva": format_rva(located_vtable)}
        result["string_constructor_rva"] = format_rva(candidate_string_constructor)
        result.update((field, format_rva(rva)) for field, rva in methods.items())
        if target_layout != 1:
            result["vtable_layout_version"] = target_layout
        return result

    @staticmethod
    def _fmod_contract(layout: int, field: str) -> Callable[[Any, int], bool] | None:
        if layout == 2 and field == "select_method_rva":
            return DERIVER.has_fmod_device_lists_select_contract
        return None

    @staticmethod
    def _locate_fmod_vtables(
        relocations: dict[int, int], methods: dict[str, int]
    ) -> list[tuple[int, int]]:
        """Returns (vtable, layout) for each vtable holding every method."""
        located = []
        for layout in (1, 2):
            slots = fmod_slots(layout, True)
            held = {slots[field]: rva for field, rva in methods.items()}
            located.extend(
                (vtable, layout)
                for vtable in TargetDerivation._vtables_holding(relocations, held)
            )
        return sorted(located)

    @staticmethod
    def _vtables_holding(relocations: dict[int, int], methods: dict[int, int]) -> list[int]:
        """Returns every vtable whose slots hold all of `methods`."""
        if not methods:
            raise AnalyzerError("no vtable method was matched")
        slot, method = next(iter(methods.items()))
        return sorted(
            offset - slot * 8
            for offset, value in relocations.items()
            if value == method
            and offset >= slot * 8
            and (offset - slot * 8) % 8 == 0
            and all(
                relocations.get(offset - slot * 8 + index * 8) == rva
                for index, rva in methods.items()
            )
        )
