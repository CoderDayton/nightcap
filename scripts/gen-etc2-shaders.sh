#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compiler="${GLSLANG_VALIDATOR:-glslangValidator}"
source="$root/src/graphics/shaders/etc2_decode.comp"
header="$root/src/graphics/generated/etc2_decode_spv.h"

if ! command -v "$compiler" >/dev/null 2>&1; then
  echo "gen-etc2-shaders: $compiler not found; set GLSLANG_VALIDATOR" >&2
  exit 2
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
"$compiler" -V --target-env vulkan1.0 --vn kEtc2DecodeSpv -o "$scratch/raw.h" "$source" >/dev/null
grep -v '^[[:space:]]*//' "$scratch/raw.h" >"$scratch/etc2_decode_spv.h"

if [[ "${1:-}" == "--check" ]]; then
  if ! cmp -s "$scratch/etc2_decode_spv.h" "$header"; then
    echo "gen-etc2-shaders: $header is stale; run scripts/gen-etc2-shaders.sh" >&2
    exit 1
  fi
  exit 0
fi

mkdir -p "$(dirname "$header")"
cp "$scratch/etc2_decode_spv.h" "$header"
