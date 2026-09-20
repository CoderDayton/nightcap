#!/usr/bin/env bash
# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0
#
# Prints the native package version for the current ref. A release tag gives
# its own version; anything else gives 0.0.0, which sorts below every release.

set -Eeuo pipefail

ref="${1:-${GITHUB_REF_NAME:-}}"
version="${ref#v}"

if [[ "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf '%s\n' "${version}"
else
  printf '0.0.0\n'
fi
