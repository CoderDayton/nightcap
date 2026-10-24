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

"""Evidence-scoring solver that finds HostAbi addresses in a new libroblox.

Each function in both builds is indexed by `roblox_function_index.py`. A
reference function is matched through independent clues:

- the same instruction shape, unique in both builds
- callers and callees that are already matched
- the strings it references
- its data references, shifted by the distance mapped for those addresses
- its place between matched neighbours

A match is accepted only with a score of at least ACCEPT_SCORE and a lead of
at least ACCEPT_LEAD over the runner-up. `roblox_abi_targets.py` turns the
matches into a profile with the same shape as the one
`derive_roblox_host_abi_profile.py` writes, `roblox_abi_vr.py` into the VR
device bridge, and `roblox_abi_check.py` compares the two on one update.
"""

from __future__ import annotations

import bisect
import collections
from dataclasses import dataclass
import difflib
import importlib.util
from pathlib import Path
import sys
from typing import Any, Callable, Sequence


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


INDEX = _load_sibling("mocktail_roblox_function_index", "roblox_function_index.py")
TARGETS = _load_sibling("mocktail_roblox_abi_targets", "roblox_abi_targets.py")
VR = _load_sibling("mocktail_roblox_abi_vr", "roblox_abi_vr.py")
DERIVER = INDEX.DERIVER
FunctionIndex = INDEX.FunctionIndex
Instruction = INDEX.Instruction
AnalyzerError = DERIVER.AnalyzerError
format_rva = DERIVER.format_rva
check_inputs = TARGETS.check_inputs
validate_fmod_source = TARGETS.validate_fmod_source

ACCEPT_SCORE = 0.6
ACCEPT_LEAD = 0.2
# Candidates that get the full, disassembling score after the cheap one.
FULL_SCORE_LIMIT = 64
# A data address used by more functions than this is too common to narrow
# the candidates by itself.
XREF_FANOUT_LIMIT = 64
# Scores are float sums; a lead that equals ACCEPT_LEAD on paper may be
# a few ulps short.
_TOLERANCE = 1e-9


def choose_candidate(
    scored: Sequence[tuple[float, int]], tie_breaker: int | None = None
) -> int | None:
    """Returns the leading candidate, or None when no lead is clear.

    `tie_breaker` picks among the candidates that clear ACCEPT_SCORE and sit
    within ACCEPT_LEAD of the best; it cannot lift or overrule any other.
    """
    if not scored:
        return None
    ranked = sorted(scored, key=lambda item: item[0], reverse=True)
    best_score, best = ranked[0]
    runner_up = ranked[1][0] if len(ranked) > 1 else 0.0
    if best_score >= ACCEPT_SCORE and best_score - runner_up >= ACCEPT_LEAD - _TOLERANCE:
        return best
    tied = {
        rva for value, rva in ranked
        if value >= ACCEPT_SCORE and best_score - value < ACCEPT_LEAD - _TOLERANCE
    }
    return tie_breaker if tie_breaker in tied else None


def align_instructions(
    reference_shapes: Sequence[str], candidate_shapes: Sequence[str]
) -> dict[int, int]:
    """Maps reference instruction indexes to candidate indexes of equal shape."""
    matcher = difflib.SequenceMatcher(
        None, reference_shapes, candidate_shapes, autojunk=False
    )
    mapping = {}
    for block in matcher.get_matching_blocks():
        for offset in range(block.size):
            mapping[block.a + offset] = block.b + offset
    return mapping


@dataclass(frozen=True)
class Match:
    rva: int
    score: float
    lead: float
    # "score" when the score alone decided, "vtable" when a slot broke a tie.
    decided_by: str


def _share(expected: Sequence[Any], present: set[Any]) -> float | None:
    if not expected:
        return None
    return sum(1 for item in expected if item in present) / len(expected)


def _weighted(parts: Sequence[tuple[float | None, float]]) -> float:
    present = [(value, weight) for value, weight in parts if value is not None]
    return sum(value * weight for value, weight in present) / sum(
        weight for _, weight in present
    )


class Solver(TARGETS.TargetDerivation, VR.VrTargets):
    def __init__(self, reference: Any, candidate: Any, cache_dir: Path | None = None):
        self._start(FunctionIndex(reference, cache_dir), FunctionIndex(candidate, cache_dir))

    @classmethod
    def from_indexes(cls, reference: Any, candidate: Any) -> "Solver":
        solver = cls.__new__(cls)
        solver._start(reference, candidate)
        return solver

    def _start(self, reference: Any, candidate: Any) -> None:
        self.reference = reference
        self.candidate = candidate
        self.pairs: dict[int, int] = {}
        self._owners: dict[int, int] = {}
        for start, other in self._shape_anchors().items():
            self._pair(start, other)
        self._propagate_calls()
        self.data_map = self._map_data_references()
        self.matches: dict[str, Match] = {}
        self._claimed: dict[int, int] = {}

    def _pair(self, start: int, other: int) -> None:
        """Pairs two functions, dropping any pair that held either one."""
        owner = self._owners.get(other)
        if owner is not None and owner != start:
            del self.pairs[owner]
        previous = self.pairs.get(start)
        if previous is not None and previous != other:
            del self._owners[previous]
        self.pairs[start] = other
        self._owners[other] = start

    def _shape_anchors(self) -> dict[int, int]:
        reference_hashes = collections.defaultdict(list)
        candidate_hashes = collections.defaultdict(list)
        for start, info in self.reference.functions.items():
            reference_hashes[info.shape_hash].append(start)
        for start, info in self.candidate.functions.items():
            candidate_hashes[info.shape_hash].append(start)
        pairs = {}
        for shape_hash, starts in reference_hashes.items():
            others = candidate_hashes.get(shape_hash, ())
            if len(starts) == 1 and len(others) == 1:
                pairs[starts[0]] = others[0]
        return pairs

    def _propagate_calls(self) -> None:
        # Matched functions with as many calls pair their callees in order.
        reference, candidate = self.reference.functions, self.candidate.functions
        pending = list(self.pairs.items())
        while pending:
            discovered = []
            for reference_start, candidate_start in pending:
                reference_calls = reference[reference_start].calls
                candidate_calls = candidate[candidate_start].calls
                if len(reference_calls) != len(candidate_calls):
                    continue
                for callee, other in zip(reference_calls, candidate_calls):
                    if (
                        callee in self.pairs
                        or other in self._owners
                        or callee not in reference
                        or other not in candidate
                    ):
                        continue
                    size = reference[callee].size
                    if abs(size - candidate[other].size) <= max(16, size // 4):
                        self._pair(callee, other)
                        discovered.append((callee, other))
            pending = discovered

    def _map_data_references(self) -> dict[int, int]:
        votes: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
        for reference_start, candidate_start in self.pairs.items():
            reference_refs = self.reference.functions[reference_start].refs
            candidate_refs = self.candidate.functions[candidate_start].refs
            if len(reference_refs) == len(candidate_refs):
                for reference_target, candidate_target in zip(reference_refs, candidate_refs):
                    votes[reference_target][candidate_target] += 1
        return {target: counter.most_common(1)[0][0] for target, counter in votes.items()}

    def _gap_candidates(self, start: int) -> list[int]:
        starts = self.reference.starts
        index = bisect.bisect_left(starts, start)
        below = next(
            (self.pairs[starts[i]] for i in range(index - 1, -1, -1) if starts[i] in self.pairs),
            self.candidate.text.address,
        )
        above = next(
            (self.pairs[starts[i]] for i in range(index + 1, len(starts)) if starts[i] in self.pairs),
            self.candidate.text.address + self.candidate.text.size,
        )
        if above <= below:
            return []
        low = bisect.bisect_left(self.candidate.starts, below)
        high = bisect.bisect_right(self.candidate.starts, above)
        return self.candidate.starts[low:high]

    def _neighbour_candidates(self, start: int) -> set[int]:
        found = set()
        for callee in self.reference.functions[start].calls:
            if callee in self.pairs:
                found.update(self.candidate.callers.get(self.pairs[callee], ()))
        for caller in self.reference.callers.get(start, ()):
            if caller in self.pairs:
                found.update(
                    target
                    for target in self.candidate.functions[self.pairs[caller]].calls
                    if target in self.candidate.functions
                )
        return found

    def _pattern_candidates(self, start: int) -> set[int]:
        # Data a function touches usually moves as one block, so its
        # references shifted by a mapped distance find it in the candidate.
        refs = set(self.reference.functions[start].refs)
        distances = collections.Counter(
            self.data_map[target] - target for target in refs if target in self.data_map
        )
        # A common address adds a vote but cannot bring in a candidate alone.
        votes = collections.Counter()
        found_by_rare = set()
        for target in refs:
            users = set()
            for distance, _ in distances.most_common(3):
                found = self.candidate.data_users.get(target + distance, ())
                users.update(found)
                if len(found) <= XREF_FANOUT_LIMIT:
                    found_by_rare.update(found)
            votes.update(users)
        needed = max(1, len(refs) // 2)
        return {user for user in found_by_rare if votes[user] >= needed}

    def _graph_score(self, start: int, other: int) -> float:
        reference = self.reference.functions[start]
        candidate = self.candidate.functions[other]
        size = min(reference.size, candidate.size) / max(reference.size, candidate.size)
        calls = [self.pairs[c] for c in reference.calls if c in self.pairs]
        callers = [self.pairs[c] for c in self.reference.callers.get(start, ()) if c in self.pairs]
        data = [self.data_map[t] for t in reference.refs if t in self.data_map]
        return _weighted(
            (
                (size, 1.0),
                (_share(calls, set(candidate.calls)), 2.0),
                (_share(callers, set(self.candidate.callers.get(other, ()))), 2.0),
                (_share(data, set(candidate.refs)), 2.0),
            )
        )

    def _full_score(self, start: int, other: int, graph: float) -> float:
        similarity = difflib.SequenceMatcher(
            None, self.reference.shapes(start), self.candidate.shapes(other), autojunk=False
        ).ratio()
        strings = [
            text
            for text in map(self.reference.string_at, self.reference.functions[start].refs)
            if text is not None
        ]
        candidate_strings = set(
            map(self.candidate.string_at, self.candidate.functions[other].refs)
        )
        return _weighted(
            (
                (similarity, 3.0),
                (_share(strings, candidate_strings), 2.0),
                (graph, 5.0),
            )
        )

    def match_function(
        self,
        name: str,
        start: int,
        contract: Callable[[Any, int], bool] | None = None,
        vtable_slot: int | None = None,
    ) -> int:
        """Matches one reference function.

        A contract drops candidates outright. `vtable_slot` is the candidate
        that sits in the matching slot of an already located vtable; it only
        breaks a tie between candidates that pass on their own evidence.
        """
        if start not in self.reference.functions:
            raise AnalyzerError(f"reference {name} is not a function start")
        if name in self.matches:
            return self.matches[name].rva
        candidates = set(self._gap_candidates(start))
        candidates |= self._neighbour_candidates(start)
        candidates |= self._pattern_candidates(start)
        if start in self.pairs:
            candidates.add(self.pairs[start])
        if vtable_slot in self.candidate.functions:
            candidates.add(vtable_slot)
        size = self.reference.functions[start].size
        candidates = {
            other
            for other in candidates
            if self._claimed.get(other, start) == start
            and self.candidate.functions[other].size <= 8 * size + 64
            and (contract is None or contract(self.candidate.image, other))
        }
        graph = {other: self._graph_score(start, other) for other in candidates}
        shortlist = sorted(candidates, key=graph.__getitem__, reverse=True)[:FULL_SCORE_LIMIT]
        if vtable_slot in candidates and vtable_slot not in shortlist:
            shortlist.append(vtable_slot)
        scored = [(self._full_score(start, other, graph[other]), other) for other in shortlist]
        chosen = choose_candidate(scored)
        decided_by = "score"
        if chosen is None and vtable_slot is not None:
            chosen = choose_candidate(scored, tie_breaker=vtable_slot)
            decided_by = "vtable"
        if chosen is None:
            ranked = sorted(scored, reverse=True)[:2]
            raise AnalyzerError(
                f"{name} has no clear candidate: "
                + ", ".join(f"{format_rva(rva)}={value:.2f}" for value, rva in ranked)
            )
        chosen_score = next(value for value, other in scored if other == chosen)
        runner_up = max((value for value, other in scored if other != chosen), default=0.0)
        self.matches[name] = Match(chosen, chosen_score, chosen_score - runner_up, decided_by)
        self._claimed[chosen] = start
        self._pair(start, chosen)
        return chosen

    def _aligned(self, name: str, start: int) -> tuple[list[Instruction], list[Instruction], dict[int, int]]:
        other = self.match_function(name, start)
        reference = self.reference.instructions(start)
        candidate = self.candidate.instructions(other)
        mapping = align_instructions(
            [item.shape for item in reference], [item.shape for item in candidate]
        )
        return reference, candidate, mapping

    def map_instruction(self, name: str, rva: int) -> int:
        start = self.reference.containing(rva)
        if start is None:
            raise AnalyzerError(f"reference {name} is outside every function")
        reference, candidate, mapping = self._aligned(f"{name} function", start)
        index = next((i for i, item in enumerate(reference) if item.address == rva), None)
        if index is None or index not in mapping:
            raise AnalyzerError(f"{name} instruction has no counterpart")
        return candidate[mapping[index]].address

    def map_data(self, name: str, function_name: str, start: int, target: int) -> int:
        reference, candidate, mapping = self._aligned(function_name, start)
        found = set()
        for index, item in enumerate(reference):
            if target not in item.rip_targets:
                continue
            if index not in mapping:
                raise AnalyzerError(f"{name} reference instruction has no counterpart")
            other = candidate[mapping[index]]
            if len(other.rip_targets) != len(item.rip_targets):
                raise AnalyzerError(f"{name} RIP-relative operand count changed")
            found.update(
                other.rip_targets[position]
                for position, value in enumerate(item.rip_targets)
                if value == target
            )
        if len(found) != 1:
            raise AnalyzerError(f"{name} has no unique mapped data address")
        return found.pop()
