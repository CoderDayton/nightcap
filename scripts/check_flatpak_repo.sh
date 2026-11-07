#!/usr/bin/env bash
# Reads a finished Flatpak repository the way a user's Flatpak will, in a
# throwaway installation, and fails unless every expected ref is listed with a
# commit signed by the key.
# Usage: check_flatpak_repo.sh <repo> <public-key> <ref>...

set -Eeuo pipefail

readonly REPOSITORY="${1:?repository path is required}"
readonly PUBLIC_KEY="${2:?GPG public key path is required}"
shift 2
(($# > 0)) || {
  printf 'No refs to check\n' >&2
  exit 1
}

SCRATCH="$(mktemp -d)"
readonly SCRATCH
trap 'rm -rf -- "${SCRATCH}"' EXIT
export FLATPAK_USER_DIR="${SCRATCH}/user"
export FLATPAK_CONFIG_DIR="${SCRATCH}/config"
mkdir -p -- "${FLATPAK_CONFIG_DIR}"

flatpak --user remote-add --gpg-import="${PUBLIC_KEY}" check \
  "file://$(realpath -- "${REPOSITORY}")"
flatpak --user remote-ls --all --columns=ref check | LC_ALL=C sort \
  >"${SCRATCH}/listed"

status=0
for ref in "$@"; do
  if ! grep -Fxq -- "${ref}" "${SCRATCH}/listed"; then
    printf 'Missing from the repository: %s\n' "${ref}" >&2
    status=1
    continue
  fi
  # remote-info loads the commit, which checks its signature.
  if ! flatpak --user remote-info check "${ref}" >/dev/null; then
    printf 'Unreadable or unsigned: %s\n' "${ref}" >&2
    status=1
  fi
done
((status == 0)) || exit 1
printf 'Checked %d refs\n' "$#"
