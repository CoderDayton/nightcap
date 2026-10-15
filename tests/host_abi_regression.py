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

"""Known Roblox builds shared by the HostAbi deriver and solver tests.

Every fixture holds answers that a real launch of that build accepted. A
build's library is used only when its bytes match the fixture's SHA-256.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

PROJECT_ROOT = Path(__file__).resolve().parents[1]
PAYLOAD_ROOT = Path.home() / ".local" / "share" / "mocktail" / "payloads"
# Holds <payload_id>/libroblox.so for builds outside the payload store.
REGRESSION_CACHE = Path.home() / ".cache" / "mocktail" / "host_abi_regression"
REGRESSION_FIXTURES = PROJECT_ROOT / "tests" / "fixtures" / "host_abi_regression"


def load_regression_build(name: str) -> dict[str, Any]:
    path = REGRESSION_FIXTURES / f"{name}.json"
    return json.loads(path.read_text(encoding="utf-8"))


def regression_library(build: dict[str, Any]) -> Path | None:
    for root in (PAYLOAD_ROOT, REGRESSION_CACHE):
        library = root / build["payload_id"] / "libroblox.so"
        if not library.is_file():
            continue
        digest = hashlib.sha256()
        with library.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                digest.update(chunk)
        if digest.hexdigest() == build["libroblox_sha256"]:
            return library
    return None


def expected_runtime(reference: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    """The candidate's runtime answers that the reference has inputs for."""
    expected = json.loads(json.dumps(candidate["runtime_compatibility"]))
    source = reference["runtime_compatibility"].get("fmod_output_device_bridge", {})
    bridge = expected.get("fmod_output_device_bridge")
    if bridge is not None:
        # Input methods are derived only from a reference that lists them.
        for field in [name for name in bridge if name.startswith("input_")]:
            if field not in source:
                del bridge[field]
    return expected


def regression_sidecar(build: dict[str, Any]) -> dict[str, Any]:
    """Returns the build as the reference sidecar the deriver reads."""
    sidecar = {
        key: build[key]
        for key in (
            "schema_version",
            "elf_build_id",
            "payload_id",
            "reference",
            "profile",
            "derivation_anchors",
        )
    }
    sidecar["payload_sha256"] = build["libroblox_sha256"]
    sidecar["payload_path"] = f"payloads/{build['payload_id']}"
    return sidecar


def regression_manifest(build: dict[str, Any]) -> dict[str, Any]:
    """Returns the build's runtime answers as a reference compatibility manifest."""
    return {
        "schema_version": 1,
        "profiles": [
            {
                "elf_build_id": build["elf_build_id"],
                "allow_host_abi_bridges": True,
                **build["runtime_compatibility"],
            }
        ],
    }
