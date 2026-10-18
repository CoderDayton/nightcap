#!/usr/bin/env bash
# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0
#
# Prints the native package version for the current ref. A release tag gives
# its own version; anything else gives 0.0.0, which sorts below every release.

set -Eeuo pipefail

if [[ "${1:-}" == --nightly ]]; then
  if described="$(git describe --tags --long --abbrev=7 \
      --match 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null)"; then
    [[ "${described}" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)-([0-9]+)-g([0-9a-f]+)$ ]] || {
      printf 'unexpected git describe output: %s\n' "${described}" >&2
      exit 1
    }
    printf '%s+r%s.g%s\n' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
      "${BASH_REMATCH[3]}"
  else
    printf '0.0.0+r%s.g%s\n' "$(git rev-list --count HEAD)" \
      "$(git rev-parse --short=7 HEAD)"
  fi
  exit 0
fi

ref="${1:-${GITHUB_REF_NAME:-}}"
version="${ref#v}"

if [[ "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf '%s\n' "${version}"
else
  printf '0.0.0\n'
fi
