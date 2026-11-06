#!/usr/bin/env bash
# Stable and the nightly Flatpak share one data folder, and every reader of
# these files refuses any schema_version but its own. Writing version 2 from
# main would stop the released build from starting after someone ran nightly.
# Bump one only with a migration that the released build can already read.
# Usage: shared_data_schema_test.sh <source-root>

set -Eeuo pipefail

readonly ROOT="${1:?source root is required}"

found="$(
  grep -rEon \
    -e '\{"schema_version", *[0-9]+\}' \
    -e 'SchemaVersion *(= *|\{ *)[0-9]+' \
    -e '\["schema_version"\] *= *[0-9]+' \
    "${ROOT}/src" || true
)"
[[ -n "${found}" ]] || {
  printf 'shared data schema test found no schema versions in %s/src\n' \
    "${ROOT}" >&2
  exit 1
}

# One line per match, so each version is checked on its own.
bumped="$(grep -Ev '[^0-9]1\}?$' <<<"${found}" || true)"
if [[ -n "${bumped}" ]]; then
  printf 'A shared data file schema changed; the released build cannot read it:\n%s\n' \
    "${bumped}" >&2
  exit 1
fi
printf 'Shared data schemas unchanged (%d writers)\n' "$(wc -l <<<"${found}")"
