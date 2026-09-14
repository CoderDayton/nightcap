#!/usr/bin/env bash
# Copyright 2026 Mocktail Project Authors
# SPDX-License-Identifier: Apache-2.0

set -Eeuo pipefail

readonly build_dir="${1:?build directory is required}"
readonly install_root="$(mktemp -d)"
trap 'rm -rf -- "${install_root}"' EXIT

DESTDIR="${install_root}" cmake --install "${build_dir}" --prefix /usr \
  >/dev/null

readonly binary="${install_root}/usr/bin/mocktail"
if [[ -d "${install_root}/usr/lib64/mocktail" ]]; then
  readonly runtime="${install_root}/usr/lib64/mocktail"
else
  readonly runtime="${install_root}/usr/lib/mocktail"
fi
readonly data="${install_root}/usr/share/mocktail"
readonly desktop="${install_root}/usr/share/applications/space.bigrat.mocktail.desktop"
readonly metainfo="${install_root}/usr/share/metainfo/space.bigrat.mocktail.metainfo.xml"
readonly icon="${install_root}/usr/share/icons/hicolor/scalable/apps/space.bigrat.mocktail.svg"

[[ -x "${binary}" ]]
[[ -x "${runtime}/mocktail_updater" ]]
[[ -x "${runtime}/mocktail_failure_dialog" ]]
[[ -x "${runtime}/mocktail_webview_helper" ]]
[[ -x "${runtime}/mocktail_freebsd_socket_helper" ]]
LC_ALL=C readelf -h "${runtime}/mocktail_freebsd_socket_helper" |
  grep -Fq 'UNIX - FreeBSD'
readelf -h "${binary}" | grep -Fq 'ELF64'
readelf -h "${runtime}/mocktail_updater" | grep -Fq 'ELF64'
readelf -d "${binary}" | grep -Eq '\$ORIGIN/\.\./(lib|lib64)/mocktail'
readelf -d "${runtime}/mocktail_failure_dialog" | grep -Fq 'libadwaita-1.so.0'
! readelf -d "${binary}" | grep -Fq 'libadwaita-1.so.0'

[[ -f "${runtime}/libOpenSLES.so" ]]
[[ -f "${data}/metadata/roblox_compatibility.json" ]]
[[ -f "${data}/metadata/roblox_host_abi_reference.json" ]]
[[ -f "${data}/metadata/roblox_signing_certificates.json" ]]
[[ ! -d "${data}/scripts" ]]
if find "${install_root}/usr" -type f \( -name '*.py' -o -name '*.sh' \) \
    -print -quit | grep -q .; then
  echo 'system install contains a first-party Python or shell runtime' >&2
  exit 1
fi

[[ -f "${desktop}" ]]
grep -Fq 'Name=Mocktail' "${desktop}"
grep -Fxq 'Exec=env SDL_VIDEODRIVER=wayland,x11 /usr/bin/mocktail %u' "${desktop}"
grep -Fq 'Icon=space.bigrat.mocktail' "${desktop}"
[[ -f "${metainfo}" ]]
grep -Fq '<id>space.bigrat.mocktail</id>' "${metainfo}"
[[ -f "${icon}" ]]
for icon_size in 16 22 24 32 36 48 64 72 96 128 192 256 512; do
  raster_icon="${install_root}/usr/share/icons/hicolor/${icon_size}x${icon_size}/apps/space.bigrat.mocktail.png"
  [[ -f "${raster_icon}" ]]
  file "${raster_icon}" | grep -Fq "${icon_size} x ${icon_size}"
done

readonly updater_home="${install_root}/updater-home"
mkdir -p "${updater_home}"
HOME="${updater_home}" \
MOCKTAIL_DATA_ROOT="${updater_home}/data" \
MOCKTAIL_CACHE_ROOT="${updater_home}/cache" \
MOCKTAIL_STATE_ROOT="${updater_home}/state" \
  "${runtime}/mocktail_updater" status |
  grep -Fq '"current": null'

"${binary}" --help | grep -Fq 'Usage:'
"${runtime}/mocktail" --help | grep -Fq 'Usage:'
updater_usage="$("${runtime}/mocktail_updater" 2>&1 || true)"
grep -Fq 'Usage:' <<<"${updater_usage}"

printf 'system install native runtime layout test passed\n'
