#!/usr/bin/env bash
# Benchmarks a base commit against the working tree and prints the change.
#
#   scripts/bench_compare.sh [--base REF] [--mode sim|real|all]
#                            [--profile menu|ingame] [--runs N] [--clean]
#
# The base is built once per commit in a cached worktree under
# ${XDG_CACHE_HOME:-~/.cache}/mocktail-bench, and the working tree gets its
# own build there with the same settings, so build/ is never used. Both
# sides run the working tree's benchmark code, so older commits work too.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/mocktail-bench"
BASE=HEAD
MODE=sim
PROFILE=ingame
RUNS=3
JOBS="${JOBS:-$(nproc)}"

usage() {
  sed -n 4,5p "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

while (($#)); do
  case "$1" in
    --base) BASE="${2:?}"; shift 2 ;;
    --mode) MODE="${2:?}"; shift 2 ;;
    --profile) PROFILE="${2:?}"; shift 2 ;;
    --runs) RUNS="${2:?}"; shift 2 ;;
    --clean)
      for tree in "${CACHE}"/src-*; do
        [[ -d "${tree}" ]] && git -C "${ROOT}" worktree remove --force "${tree}"
      done
      rm -rf "${CACHE}"
      git -C "${ROOT}" worktree prune
      echo "removed ${CACHE}"
      exit 0
      ;;
    *) usage ;;
  esac
done
[[ "${MODE}" =~ ^(sim|real|all)$ ]] || usage
[[ "${PROFILE}" =~ ^(menu|ingame)$ ]] || usage

sha="$(git -C "${ROOT}" rev-parse --verify "${BASE}^{commit}")"
short="${sha:0:9}"
tree="${CACHE}/src-${sha}"
base_build="${CACHE}/build-${sha}"
current_build="${CACHE}/build-working"
# Each run replaces the previous run's results.
out="${CACHE}/last-compare"
rm -rf "${out}"
mkdir -p "${out}"

if [[ "${MODE}" != sim ]] &&
   ! git -C "${ROOT}" diff --quiet "${sha}" -- \
       config/roblox_payload.json config/roblox_compatibility.json; then
  echo "${short} pins a different Roblox payload than the working tree." >&2
  echo "Running it could install or remove payloads; use --mode sim." >&2
  exit 2
fi

targets=()
[[ "${MODE}" != real ]] && targets+=(mocktail_legacy_jnivm)
# Without mocktail_updater a launch skips its startup version check.
[[ "${MODE}" != sim ]] && targets+=(mocktail mocktail_updater mocktail_heap_counter)

mkdir -p "${CACHE}"
if [[ ! -f "${tree}/.bench-ready" ]]; then
  echo "== checking out ${short}"
  [[ -d "${tree}" ]] && git -C "${ROOT}" worktree remove --force "${tree}"
  git -C "${ROOT}" worktree add --detach --quiet "${tree}" "${sha}"
  # Submodules resolve to local clones, which git blocks by default.
  if ! git -C "${tree}" -c protocol.file.allow=always \
      submodule update --init --recursive --quiet; then
    git -C "${ROOT}" worktree remove --force "${tree}"
    exit 1
  fi
  touch "${tree}/.bench-ready"
fi
configure() {
  cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF >"$3"
}
echo "== building ${short} (cached in ${base_build})"
configure "${tree}" "${base_build}" "${out}/base-configure.log"
# The heap counter target is new, so older commits may not have it.
base_targets=()
for target in "${targets[@]}"; do
  [[ "${target}" == mocktail_heap_counter ]] || base_targets+=("${target}")
done
nice cmake --build "${base_build}" -j"${JOBS}" --target "${base_targets[@]}" \
  >"${out}/base-build.log" || {
  tail -20 "${out}/base-build.log" >&2
  exit 1
}
echo "== building working tree (in ${current_build})"
configure "${ROOT}" "${current_build}" "${out}/current-configure.log"
nice cmake --build "${current_build}" -j"${JOBS}" --target "${targets[@]}" \
  >"${out}/current-build.log" || {
  tail -20 "${out}/current-build.log" >&2
  exit 1
}

compare() {
  python3 -I "${ROOT}/scripts/startup_benchmark.py" compare "$1" "$2" |
    sed "1s/before/${short}/; 1s/after/ working/"
}

if [[ "${MODE}" != real ]]; then
  for side in base current; do
    if [[ "${side}" == base ]]; then src="${tree}" lib="${base_build}"
    else src="${ROOT}" lib="${current_build}"; fi
    nice c++ -O3 -DNDEBUG -std=c++17 -fno-exceptions \
      -I"${src}/include" -I"${src}/third_party/jni/include" \
      "${ROOT}/tests/perf/jnivm_session_benchmark.cc" \
      "${lib}/libmocktail_legacy_jnivm.a" -lpthread -ldl -o "${out}/sim-${side}"
  done
  # Alternate the two sides so machine noise hits both equally.
  for round in 1 2; do
    for side in base current; do
      echo "== simulated ${PROFILE} session: ${side} (round ${round}/2)"
      "${out}/sim-${side}" --profile "${PROFILE}" --repeat "${RUNS}" \
        --json "${out}/sim-${side}-${round}.json" >/dev/null
    done
  done
  python3 -I - "${out}" <<'EOF'
import json, statistics, sys
out = sys.argv[1]
for side in ("base", "current"):
    rounds = [json.load(open(f"{out}/sim-{side}-{r}.json")) for r in (1, 2)]
    merged = dict(rounds[0])
    merged["metrics"] = {
        name: statistics.median(r["metrics"][name] for r in rounds)
        for name in rounds[0]["metrics"]}
    json.dump(merged, open(f"{out}/sim-{side}.json", "w"))
EOF
  echo
  echo "simulated JNI session (${PROFILE})"
  compare "${out}/sim-base.json" "${out}/sim-current.json"
fi

if [[ "${MODE}" != sim ]]; then
  for side in base current; do
    if [[ "${side}" == base ]]; then bin="${base_build}/mocktail"
    else bin="${current_build}/mocktail"; fi
    echo "== real app: ${side}"
    python3 -I "${ROOT}/scripts/startup_benchmark.py" run --bin "${bin}" \
      --profile "$([[ "${PROFILE}" == ingame ]] && echo ingame || echo default)" \
      --runs "${RUNS}" --json "${out}/real-${side}.json" >"${out}/real-${side}.txt"
  done
  echo
  echo "real app (${PROFILE})"
  compare "${out}/real-base.json" "${out}/real-current.json"
fi
echo
echo "results and logs: ${out}"
