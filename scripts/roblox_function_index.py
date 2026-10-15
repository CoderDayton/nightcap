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

"""Function index of one libroblox image for `roblox_abi_solver.py`.

Every function listed in `.eh_frame_hdr` is decoded once and reduced to a
normalized instruction shape, its direct calls and its RIP-relative data
references. Shapes drop branch targets, RIP displacements and immediates of
0x1000 and above, so a function moved by the linker keeps its shape.
"""

from __future__ import annotations

import bisect
import collections
from dataclasses import dataclass
import gzip
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
from typing import Any


def _load_deriver():
    name = "mocktail_host_abi_deriver_for_solver"
    if name in sys.modules:
        return sys.modules[name]
    path = Path(__file__).resolve().with_name("derive_roblox_host_abi_profile.py")
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load the HostAbi deriver")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


DERIVER = _load_deriver()
AnalyzerError = DERIVER.AnalyzerError

_RIP_OPERAND = re.compile(r"\[rip ([+-]) (0x[0-9a-f]+|\d+)\]")
_IMMEDIATE = re.compile(r"0x[0-9a-f]+|\b\d+\b")
# Smaller immediates are offsets and flags that stay put between builds.
# Larger ones are addresses and sizes that move.
_STABLE_IMMEDIATE_LIMIT = 0x1000


def _stable_immediate(match: re.Match) -> str:
    return "I" if int(match.group(0), 0) >= _STABLE_IMMEDIATE_LIMIT else match.group(0)


@dataclass(frozen=True)
class Instruction:
    address: int
    shape: str
    rip_targets: tuple[int, ...]
    branch_target: int | None


def decode_function(
    decoder: Any, code: bytes, start: int
) -> list[Instruction]:
    end = start + len(code)
    instructions = []
    for address, size, mnemonic, operands in decoder.disasm_lite(code, start):
        if (mnemonic == "call" or mnemonic.startswith("j")) and operands.startswith("0x"):
            target = int(operands, 16)
            if mnemonic == "call":
                shape = "call"
            else:
                shape = mnemonic + (" in" if start <= target < end else " out")
            instructions.append(Instruction(address, shape, (), target))
            continue
        rip_targets = []
        for match in _RIP_OPERAND.finditer(operands):
            displacement = int(match.group(2), 0)
            rip_targets.append(
                address + size + (-displacement if match.group(1) == "-" else displacement)
            )
        operands = _IMMEDIATE.sub(_stable_immediate, _RIP_OPERAND.sub("[rip]", operands))
        instructions.append(
            Instruction(address, f"{mnemonic} {operands}", tuple(rip_targets), None)
        )
    return instructions


@dataclass(frozen=True)
class FunctionInfo:
    size: int
    shape_hash: bytes
    calls: tuple[int, ...]
    refs: tuple[int, ...]


def function_table(image: Any) -> list[tuple[int, int]]:
    """Returns (start, size) for every FDE indexed by `.eh_frame_hdr`."""
    header = image.sections.get(".eh_frame_hdr")
    if header is None:
        raise AnalyzerError("libroblox has no .eh_frame_hdr")
    raw = image.section_bytes(header)
    # Version 1, a 4-byte eh_frame_ptr, udata4 count, datarel sdata4 table.
    if len(raw) < 12 or raw[0] != 1 or raw[2] != 0x03 or raw[3] != 0x3B:
        raise AnalyzerError(".eh_frame_hdr uses an unsupported encoding")
    if raw[1] == 0xFF or raw[1] & 0x0F not in (0x03, 0x0B):
        raise AnalyzerError(".eh_frame_hdr eh_frame_ptr encoding is not 4 bytes")
    count = struct.unpack_from("<I", raw, 8)[0]
    if 12 + count * 8 > len(raw):
        raise AnalyzerError(".eh_frame_hdr table exceeds its section")
    table = []
    for index in range(count):
        location, fde = struct.unpack_from("<ii", raw, 12 + index * 8)
        start = header.address + location
        fde_rva = header.address + fde
        offset = image.rva_to_offset(fde_rva, 16)
        length, _, begin, size = struct.unpack_from("<IIiI", image.data, offset)
        # An FDE starts with its length, CIE pointer, then a pcrel sdata4
        # pc_begin and its udata4 range.
        if length == 0xFFFFFFFF or fde_rva + 8 + begin != start:
            raise AnalyzerError(".eh_frame FDE does not match its index entry")
        table.append((start, size))
    table.sort()
    return table


_SOURCE_DIGEST = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:16]


def cache_path(cache_dir: Path, library_sha256: str) -> Path:
    """Cache file for one library; shapes depend on this code and capstone."""
    version = getattr(DERIVER.capstone, "__version__", "unknown")
    return cache_dir / f"{library_sha256}-{_SOURCE_DIGEST}-capstone{version}.json.gz"


def link_functions(
    functions: dict[int, FunctionInfo],
) -> tuple[dict[int, list[int]], dict[int, list[int]]]:
    """Returns each function's callers and each data address's users, once each."""
    callers: dict[int, list[int]] = collections.defaultdict(list)
    data_users: dict[int, list[int]] = collections.defaultdict(list)
    for start, info in functions.items():
        for target in dict.fromkeys(info.calls):
            if target in functions:
                callers[target].append(start)
        for target in dict.fromkeys(info.refs):
            data_users[target].append(start)
    return callers, data_users


def _int_list(value: Any) -> tuple[int, ...]:
    if not isinstance(value, list) or not all(type(item) is int for item in value):
        raise ValueError("cached index entry is malformed")
    return tuple(value)


def _read_cache(path: Path) -> dict[int, FunctionInfo] | None:
    """Returns the cached functions, or None when the file is missing or bad."""
    try:
        with gzip.open(path, "rt", encoding="utf-8") as stream:
            entries = json.load(stream)
        if not isinstance(entries, list) or not entries:
            raise ValueError("cached index is empty")
        functions = {}
        for entry in entries:
            start, size, shape_hash, calls, refs = entry
            if type(start) is not int or type(size) is not int:
                raise ValueError("cached index entry is malformed")
            functions[start] = FunctionInfo(
                size, bytes.fromhex(shape_hash), _int_list(calls), _int_list(refs)
            )
        return functions
    except (OSError, EOFError, ValueError, TypeError):
        return None


def _write_cache(path: Path, functions: dict[int, FunctionInfo]) -> None:
    # A cache that cannot be written only costs the next run a rebuild.
    entries = [
        [start, info.size, info.shape_hash.hex(), list(info.calls), list(info.refs)]
        for start, info in functions.items()
    ]
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    except OSError:
        return
    try:
        with os.fdopen(descriptor, "wb") as raw, gzip.open(raw, "wt", encoding="utf-8") as stream:
            json.dump(entries, stream, separators=(",", ":"))
        os.replace(temporary, path)
    except OSError:
        Path(temporary).unlink(missing_ok=True)


class FunctionIndex:
    """Normalized shapes, calls and data references of every function.

    With `cache_dir`, the per-function data is kept in a file named after the
    library's SHA-256, this module's source and the capstone version, and read
    back instead of decoding the library again.
    """

    def __init__(self, image: Any, cache_dir: Path | None = None):
        DERIVER.require_capstone()
        self.image = image
        self.decoder = DERIVER.capstone.Cs(
            DERIVER.capstone.CS_ARCH_X86, DERIVER.capstone.CS_MODE_64
        )
        self.text = image.sections[".text"]
        self._decoded: dict[int, list[Instruction]] = {}
        path = None if cache_dir is None else cache_path(Path(cache_dir), image.sha256)
        functions = None if path is None else _read_cache(path)
        if functions is None:
            functions = self._decode_all()
            if path is not None:
                _write_cache(path, functions)
        self.functions = functions
        self.starts = sorted(self.functions)
        self.callers, self.data_users = link_functions(self.functions)

    def _decode_all(self) -> dict[int, FunctionInfo]:
        text_end = self.text.address + self.text.size
        functions = {}
        for start, size in function_table(self.image):
            if size == 0 or not self.text.address <= start < text_end:
                continue
            instructions = self.instructions(start, size, cache=False)
            shape_hash = hashlib.blake2b(
                "\n".join(item.shape for item in instructions).encode(), digest_size=8
            ).digest()
            calls = tuple(
                item.branch_target
                for item in instructions
                if item.branch_target is not None
                and (item.shape == "call" or item.shape.endswith(" out"))
            )
            refs = tuple(target for item in instructions for target in item.rip_targets)
            functions[start] = FunctionInfo(size, shape_hash, calls, refs)
        return functions

    def instructions(
        self, start: int, size: int | None = None, cache: bool = True
    ) -> list[Instruction]:
        if cache and start in self._decoded:
            return self._decoded[start]
        if size is None:
            size = self.functions[start].size
        offset = self.text.offset + start - self.text.address
        decoded = decode_function(self.decoder, self.image.data[offset : offset + size], start)
        if cache:
            self._decoded[start] = decoded
        return decoded

    def shapes(self, start: int) -> list[str]:
        # Scoring reads each candidate once; only aligned functions stay cached.
        return [item.shape for item in self.instructions(start, cache=start in self._decoded)]

    def containing(self, rva: int) -> int | None:
        index = bisect.bisect_right(self.starts, rva) - 1
        if index < 0:
            return None
        start = self.starts[index]
        return start if rva < start + self.functions[start].size else None

    def string_at(self, rva: int) -> bytes | None:
        segment = self.image.segment_for_rva(rva)
        if segment is None or not segment.contains_file_data(rva):
            return None
        offset = self.image.rva_to_offset(rva)
        raw = self.image.data[offset : offset + 96]
        end = raw.find(b"\0")
        if end < 4 or not all(32 <= byte < 127 for byte in raw[:end]):
            return None
        return raw[:end]
