#!/usr/bin/env bash
# latch install -- headers, liblatch.a and the CMake package, so an app's
# CMakeLists can say find_package(latch) and link latch::latch.
#
#   ./install.sh                 build, then install to /usr/local (sudo if needed)
#   ./install.sh --prefix DIR    install to DIR (e.g. ~/.local -- no sudo)
#
# Writes share/latch/install-manifest.txt listing every path installed, which
# uninstall.sh removes exactly. After changing latch: ./install.sh again, THEN
# rebuild the apps -- they compile in whatever latch is installed.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

PREFIX=/usr/local
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

./build.sh
SUDO=""
mkdir -p "$PREFIX" 2>/dev/null || true
[[ -w "$PREFIX" ]] || SUDO=sudo

$SUDO cmake --install build --prefix "$PREFIX"
MANIFEST="$PREFIX/share/latch/install-manifest.txt"
tmp="$(mktemp)"
{ cat build/install_manifest.txt; echo; echo "$MANIFEST"; } | sed '/^$/d' > "$tmp"
$SUDO install -D -m 644 "$tmp" "$MANIFEST"
rm -f "$tmp"
echo ""
echo "Installed latch $(sed -n 's/^project(latch VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt) to $PREFIX"
echo "  $(wc -l < "$MANIFEST") files -- listed in $MANIFEST"
