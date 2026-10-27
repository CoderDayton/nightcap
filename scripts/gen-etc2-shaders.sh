#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compiler="${GLSLANG_VALIDATOR:-glslangValidator}"
shaders="$root/src/graphics/shaders"
generated="$root/src/graphics/generated"

if ! command -v "$compiler" >/dev/null 2>&1; then
  echo "gen-etc2-shaders: $compiler not found; set GLSLANG_VALIDATOR" >&2
  exit 2
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
stale=0
for pair in etc2_decode:kEtc2DecodeSpv etc2_transcode:kEtc2TranscodeSpv \
  etc2_scale:kEtc2ScaleSpv; do
  name="${pair%%:*}"
  symbol="${pair##*:}"
  header="$generated/${name}_spv.h"
  "$compiler" -V --target-env vulkan1.0 --vn "$symbol" -o "$scratch/raw.h" \
    "$shaders/$name.comp" >/dev/null
  grep -v '^[[:space:]]*//' "$scratch/raw.h" >"$scratch/${name}_spv.h"
  if [[ "${1:-}" == "--check" ]]; then
    if ! cmp -s "$scratch/${name}_spv.h" "$header"; then
      echo "gen-etc2-shaders: $header is stale; run scripts/gen-etc2-shaders.sh" >&2
      stale=1
    fi
  else
    mkdir -p "$generated"
    cp "$scratch/${name}_spv.h" "$header"
  fi
done
exit "$stale"
