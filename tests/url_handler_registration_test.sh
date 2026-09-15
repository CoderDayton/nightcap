#!/usr/bin/env bash
# Modified by vii from komaruworld/mocktail. See README "About this fork".
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly registration_script="${1:?registration script is required}"
readonly desktop_source="${2:?desktop source is required}"
test_root="$(mktemp -d)"
trap 'rm -rf -- "${test_root}"' EXIT

mkdir -p "${test_root}/bin" "${test_root}/home" "${test_root}/data"
mocktail_binary="${test_root}/mocktail"
printf '#!/usr/bin/env bash\nexit 0\n' >"${mocktail_binary}"
chmod 0755 "${mocktail_binary}"

cat >"${test_root}/bin/update-desktop-database" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"${DESKTOP_DATABASE_CALLS:?}"
EOF
chmod 0755 "${test_root}/bin/update-desktop-database"

cat >"${test_root}/bin/desktop-file-validate" <<'EOF'
#!/usr/bin/env bash
grep -Fxq 'Type=Application' "$1"
grep -Fxq 'MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;' "$1"
EOF
chmod 0755 "${test_root}/bin/desktop-file-validate"

cat >"${test_root}/bin/xdg-mime" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
  default)
    printf '%s\n' "$*" >>"${XDG_MIME_CALLS:?}"
    [[ "$2" == io.github.CoderDayton.nightcap.desktop ]]
    [[ "$3" == x-scheme-handler/roblox ]]
    [[ "$4" == x-scheme-handler/roblox-player ]]
    touch "${XDG_MIME_SELECTED:?}"
    ;;
  query)
    [[ -f "${XDG_MIME_SELECTED:?}" ]] || exit 0
    printf '%s\n' io.github.CoderDayton.nightcap.desktop
    ;;
  *) exit 2 ;;
esac
EOF
chmod 0755 "${test_root}/bin/xdg-mime"

export HOME="${test_root}/home"
export XDG_DATA_HOME="${test_root}/data"
export DESKTOP_DATABASE_CALLS="${test_root}/desktop-database-calls"
export XDG_MIME_CALLS="${test_root}/xdg-mime-calls"
export XDG_MIME_SELECTED="${test_root}/xdg-mime-selected"
export PATH="${test_root}/bin:${PATH}"

"${registration_script}" --set-default \
  --executable "${mocktail_binary}" --desktop-file "${desktop_source}" \
  >/dev/null

readonly installed_desktop="${XDG_DATA_HOME}/applications/io.github.CoderDayton.nightcap.desktop"
[[ -f "${installed_desktop}" && ! -L "${installed_desktop}" ]]
grep -Fxq "Exec=env SDL_VIDEODRIVER=wayland,x11 ${mocktail_binary} %u" \
  "${installed_desktop}"
grep -Fxq \
  'MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;' \
  "${installed_desktop}"
grep -Fxq 'X-Mocktail-Managed=true' "${installed_desktop}"
grep -Fxq -- "-q ${XDG_DATA_HOME}/applications" \
  "${DESKTOP_DATABASE_CALLS}"
grep -Fxq \
  'default io.github.CoderDayton.nightcap.desktop x-scheme-handler/roblox x-scheme-handler/roblox-player' \
  "${XDG_MIME_CALLS}"

rm -f "${XDG_MIME_SELECTED}"
"${registration_script}" --install-only \
  --executable "${mocktail_binary}" --desktop-file "${desktop_source}" \
  >/dev/null
[[ "$(wc -l <"${XDG_MIME_CALLS}")" == 1 ]]

fixture_project="${test_root}/fixture-project"
mkdir -p "${fixture_project}/scripts" "${fixture_project}/build"
cp -- "${registration_script}" \
  "${fixture_project}/scripts/register_url_handler.sh"
cp -- "${mocktail_binary}" "${fixture_project}/build/mocktail"
"${fixture_project}/scripts/register_url_handler.sh" --install-only \
  --executable "${fixture_project}/build/mocktail" \
  --desktop-file "${desktop_source}" >/dev/null
grep -Fxq \
  "Exec=env SDL_VIDEODRIVER=wayland,x11 ${fixture_project}/build/mocktail %u" \
  "${installed_desktop}"
grep -Fxq "Path=${fixture_project}" "${installed_desktop}"

unsafe_binary="${test_root}/mocktail unsafe"
cp -- "${mocktail_binary}" "${unsafe_binary}"
if "${registration_script}" --install-only \
    --executable "${unsafe_binary}" --desktop-file "${desktop_source}" \
    >/dev/null 2>&1; then
  echo 'registration accepted an unsafe desktop Exec path' >&2
  exit 1
fi

rm -f "${installed_desktop}"
ln -s "${desktop_source}" "${installed_desktop}"
if "${registration_script}" --install-only \
    --executable "${mocktail_binary}" --desktop-file "${desktop_source}" \
    >/dev/null 2>&1; then
  echo 'registration replaced a symlink desktop entry' >&2
  exit 1
fi

echo 'URL handler registration checks passed'
