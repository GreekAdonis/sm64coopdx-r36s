#!/usr/bin/env bash
# Rebuild lib/coopnet/linux/libcoopnet-arm64.a from the patched CoopNet
# source so it stays reproducible. Run inside an arm64 build environment:
#
#   docker run --rm --platform linux/arm64 \
#     -v "$PWD":/build -w /build debian:bullseye \
#     bash scripts/rebuild-coopnet.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

COOPNET_REPO="${COOPNET_REPO:-https://github.com/coop-deluxe/coopnet.git}"
COOPNET_COMMIT="${COOPNET_COMMIT:-9d9b3dd4e87dba2fa3ca542ae32b73f43df32b0e}"
PATCH="$REPO_ROOT/lib/coopnet/executable-hash-override.patch"
OUT="$REPO_ROOT/lib/coopnet/linux/libcoopnet-arm64.a"

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }
command -v g++ >/dev/null || { echo "g++ is required" >&2; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

git clone --quiet "$COOPNET_REPO" "$tmp/coopnet"
cd "$tmp/coopnet"
git checkout --quiet "$COOPNET_COMMIT"
git apply --check "$PATCH"
git apply "$PATCH"
make -s lib
cp bin/libcoopnet.a "$OUT"
echo "Wrote $OUT (coopnet $COOPNET_COMMIT + executable-hash-override.patch)"