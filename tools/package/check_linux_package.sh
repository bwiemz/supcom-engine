#!/usr/bin/env bash
# Run the Linux packages in <dist dir> (M227b): the AppImage and the tarball
# must each start and print their version, and the game must ask no more of
# the system than the Vulkan loader and glibc -- no newer glibc than the
# oldest one supported (the release workflow builds on Ubuntu 22.04).
#
# Usage: tools/package/check_linux_package.sh <dist dir>
set -euo pipefail

# The newest glibc symbol version the game may need: Ubuntu 22.04's. (A
# build on a newer system needs a newer one: set MAX_GLIBC to check it.)
readonly MAX_GLIBC="${MAX_GLIBC:-2.35}"
# What the game may link from the system.
readonly ALLOWED_NEEDED="libvulkan.so.1 libm.so.6 libc.so.6 ld-linux-x86-64.so.2"

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <dist dir>" >&2
    exit 2
fi
dist="$1"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
failed=0

shopt -s nullglob
appimages=("${dist}"/OpenSupCom-*-x86_64.AppImage)
tarballs=("${dist}"/OpenSupCom-*-linux-x86_64.tar.gz)
shopt -u nullglob
if [[ ${#appimages[@]} -ne 1 || ${#tarballs[@]} -ne 1 ]]; then
    echo "expected one AppImage and one tarball in ${dist}" >&2
    exit 1
fi
appimage="${appimages[0]}"
tarball="${tarballs[0]}"
version="$(basename "${appimage}" | sed -E 's/^OpenSupCom-([0-9.]+)-x86_64\.AppImage$/\1/')"

# check_version <what> <output>
check_version() {
    if [[ "$2" =~ ^OpenSupCom\ ${version//./\\.}\ \(.+\)$ ]]; then
        echo "$1: $2"
    else
        echo "$1: unexpected --version output: $2" >&2
        failed=1
    fi
}

# The AppImage, as CI can run it (no FUSE)
check_version "AppImage" "$("${appimage}" --appimage-extract-and-run --version)"

# The tarball
tar -xzf "${tarball}" -C "${work}"
game="$(find "${work}" -path '*/bin/opensupcom' -type f)"
check_version "tarball" "$("${game}" --version)"

# What it needs of the system
needed="$(readelf -d "${game}" | sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p')"
for lib in ${needed}; do
    if [[ " ${ALLOWED_NEEDED} " != *" ${lib} "* ]]; then
        echo "the game needs ${lib}, which a player's system may lack" >&2
        failed=1
    fi
done
newest="$(objdump -T "${game}" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sed 's/GLIBC_//' | sort -uV | tail -1)"
echo "needs: ${needed//$'\n'/ }; glibc ${newest}"
if [[ "$(printf '%s\n%s\n' "${newest}" "${MAX_GLIBC}" | sort -V | tail -1)" != "${MAX_GLIBC}" ]]; then
    echo "the game needs glibc ${newest}, newer than ${MAX_GLIBC}" >&2
    failed=1
fi
exit "${failed}"
