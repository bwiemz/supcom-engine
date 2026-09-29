#!/usr/bin/env bash
# Build OpenSupCom's AppImage (M227b) from a Release build.
#
# Usage: tools/package/appimage.sh <build dir> <output dir>
#
# Installs the build into an AppDir, adds the AppImage's entry points (AppRun,
# and the desktop entry and icon at its root), and packs it with a pinned
# appimagetool and type-2 runtime, each checked against its sha256. Prints the
# path of <output dir>/OpenSupCom-<version>-x86_64.AppImage.
#
# APPIMAGE_TOOLS_DIR: where the downloaded tools are kept (default: the build
# dir's appimage-tools), so another run needs no network.
set -euo pipefail

# Pinned releases. The runtime's signature was checked against its release
# key (AppImage type 2 runtime, EDDSA 570C77ACEA40C0F1B758902CBF96CCA56490F695)
# when it was pinned.
readonly APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
readonly APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
readonly RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
readonly RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <build dir> <output dir>" >&2
    exit 2
fi
build="$1"
out="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="$(sed -nE 's/^project\([A-Za-z]+ VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${root}/CMakeLists.txt")"
if [[ -z "${version}" ]]; then
    echo "no project(VERSION) in ${root}/CMakeLists.txt" >&2
    exit 1
fi
tools="${APPIMAGE_TOOLS_DIR:-${build}/appimage-tools}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# fetch <url> <sha256> <file>: download unless already there, and check it
fetch() {
    local url="$1" sum="$2" file="$3"
    if ! { [[ -f "${file}" ]] && echo "${sum}  ${file}" | sha256sum --check --status; }; then
        curl -fsSL --retry 3 -o "${file}.part" "${url}"
        if ! echo "${sum}  ${file}.part" | sha256sum --check --status; then
            echo "${url}: sha256 mismatch" >&2
            rm -f "${file}.part"
            exit 1
        fi
        mv "${file}.part" "${file}"
    fi
    chmod +x "${file}"
}
mkdir -p "${tools}" "${out}"
fetch "${APPIMAGETOOL_URL}" "${APPIMAGETOOL_SHA256}" "${tools}/appimagetool-x86_64.AppImage"
fetch "${RUNTIME_URL}" "${RUNTIME_SHA256}" "${tools}/runtime-x86_64"

appdir="${work}/OpenSupCom.AppDir"
cmake --install "${build}" --prefix "${appdir}/usr" > /dev/null
ln -s usr/bin/opensupcom "${appdir}/AppRun"
cp "${appdir}/usr/share/applications/opensupcom.desktop" "${appdir}/"
cp "${appdir}/usr/share/icons/hicolor/scalable/apps/opensupcom.svg" "${appdir}/"

target="$(cd "${out}" && pwd)/OpenSupCom-${version}-x86_64.AppImage"
# --appimage-extract-and-run: CI has no FUSE to mount the tool itself
ARCH=x86_64 "${tools}/appimagetool-x86_64.AppImage" --appimage-extract-and-run \
    --runtime-file "${tools}/runtime-x86_64" --no-appstream \
    "${appdir}" "${target}" >&2
echo "${target}"
