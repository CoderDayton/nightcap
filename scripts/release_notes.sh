#!/usr/bin/env bash
# Print the CHANGELOG.md section for one version, for use as GitHub release
# notes. Usage: scripts/release_notes.sh v0.1.0 [CHANGELOG.md]
# Exits 1 when the version has no section, so a release cannot ship without
# notes.

set -Eeuo pipefail

readonly tag="${1:?tag is required, e.g. v0.1.0}"
readonly changelog="${2:-$(dirname -- "${BASH_SOURCE[0]}")/../CHANGELOG.md}"
readonly version="${tag#v}"

notes="$(awk -v version="${version}" '
  /^## \[/ {
    if (found) exit
    found = ($0 ~ "^## \\[" version "\\]")
    next
  }
  /^\[[^]]+\]: / { if (found) exit }
  found { print }
' "${changelog}")"

# Trim leading and trailing blank lines.
notes="$(printf '%s\n' "${notes}" | sed -e '/./,$!d' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}')"

[[ -n "${notes}" ]] || {
  printf 'CHANGELOG.md has no section for version %s\n' "${version}" >&2
  exit 1
}
printf '%s\n' "${notes}"
