#!/usr/bin/env bash
# latch uninstall.
#
#   ./uninstall.sh                 remove from /usr/local (sudo if needed)
#   ./uninstall.sh --prefix DIR    remove from DIR
#
# Removes exactly the paths in share/latch/install-manifest.txt, and only those
# inside the prefix, then latch's own empty folders. Apps already built keep
# working (the door is compiled into them); a rebuild needs latch again.
set -euo pipefail

PREFIX=/usr/local
while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "uninstall.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
PREFIX="${PREFIX%/}"
MANIFEST="$PREFIX/share/latch/install-manifest.txt"
if [[ ! -f "$MANIFEST" ]]; then echo "uninstall.sh: latch isn't installed in $PREFIX" >&2; exit 1; fi
SUDO=""
[[ -w "$PREFIX/share/latch" ]] || SUDO=sudo

while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    case "$f" in
        "$PREFIX"/*) $SUDO rm -f -- "$f" && echo "  removed $f" ;;
        *) echo "  skipped (outside $PREFIX): $f" ;;
    esac
done < "$MANIFEST"
for d in "$PREFIX/include/latch" "$PREFIX"/lib*/cmake/latch "$PREFIX/share/latch"; do
    [[ -d "$d" ]] && $SUDO rmdir -- "$d" 2>/dev/null || true
done
echo "latch removed from $PREFIX"
