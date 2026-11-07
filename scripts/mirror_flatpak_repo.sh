#!/usr/bin/env bash
# Copies the published Flatpak repository into a new local repository, so a
# publish adds its branch to every branch already served instead of replacing
# them. Pages serves only what the last deploy uploaded.
# Usage: mirror_flatpak_repo.sh <repo-url> <public-key> <new-repo> <refs-out>
# Writes the published refs to <refs-out>, one per line.

set -Eeuo pipefail
umask 022

readonly URL="${1:?repository URL is required}"
readonly PUBLIC_KEY="${2:?GPG public key path is required}"
readonly REPOSITORY="${3:?new repository path is required}"
readonly REFS_OUT="${4:?refs output path is required}"
readonly ATTEMPTS=4

[[ -s "${PUBLIC_KEY}" && ! -L "${PUBLIC_KEY}" ]] || {
  printf 'GPG public key is missing or unsafe: %s\n' "${PUBLIC_KEY}" >&2
  exit 1
}
[[ ! -e "${REPOSITORY}" ]] || {
  printf 'Repository path already exists: %s\n' "${REPOSITORY}" >&2
  exit 1
}

ostree --repo="${REPOSITORY}" init --mode=archive-z2
# Every commit and the summary must carry the release key's signature, so a
# tampered or foreign repository is never republished under it.
ostree --repo="${REPOSITORY}" remote add \
  --set=gpg-verify=true \
  --set=gpg-verify-summary=true \
  --gpg-import="${PUBLIC_KEY}" \
  published "${URL}"

for ((attempt = 1; ; attempt++)); do
  # --depth=1 keeps each branch's previous commit for update deltas.
  if ostree --repo="${REPOSITORY}" pull --mirror --depth=1 published; then
    break
  fi
  if ((attempt == ATTEMPTS)); then
    printf 'Cannot mirror %s after %d attempts\n' "${URL}" "${ATTEMPTS}" >&2
    exit 1
  fi
  sleep $((attempt * 15))
done
ostree --repo="${REPOSITORY}" remote delete published

ostree --repo="${REPOSITORY}" refs | LC_ALL=C sort >"${REFS_OUT}"
grep -q '^app/' "${REFS_OUT}" || {
  printf 'The published repository at %s has no application\n' "${URL}" >&2
  exit 1
}
printf 'Mirrored %s:\n' "${URL}"
sed 's/^/  /' "${REFS_OUT}"
