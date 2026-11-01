#!/bin/bash
# Sign the built patched modules with the local MOK key and stage them.
#
# Signing works before enrolment; loading does not. Staging therefore keeps the
# machine on its stock driver until finish-mok.sh installs them after the key is
# enrolled.
#
#   sudo ./sign-stage.sh [ARTIFACT_DIR]
set -euo pipefail

K=$(uname -r)
ART=${1:-$(cd "$(dirname "$0")/../.." && pwd)/artifacts/610.43.03-$K}
STAGE=${CMP50_STAGE:-/opt/cmp50hx/driver-610.43.03}
SIGN=/usr/src/linux-headers-$K/scripts/sign-file
MOK=/var/lib/shim-signed/mok

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }
[ -x "$SIGN" ] || { echo "sign-file not found: $SIGN" >&2; exit 1; }
[ -r "$MOK/MOK.priv" ] && [ -r "$MOK/MOK.der" ] || {
    echo "MOK key pair missing in $MOK" >&2; exit 1; }
[ -d "$ART" ] || {
    echo "artifacts missing: $ART" >&2
    echo "build them first: (cd ../.. && bash ./build.sh --card cmp50hx)" >&2
    exit 1; }

mkdir -p "$STAGE"
for ko in "$ART"/nvidia*.ko; do
    b=$(basename "$ko")
    cp -f "$ko" "$STAGE/$b"
    "$SIGN" sha256 "$MOK/MOK.priv" "$MOK/MOK.der" "$STAGE/$b" "$STAGE/$b"
done

echo "staged in $STAGE:"
for ko in "$STAGE"/nvidia*.ko; do
    printf '  %-24s version=%-10s signer=%s\n' "$(basename "$ko")" \
        "$(modinfo -F version "$ko" 2>/dev/null)" \
        "$(modinfo -F signer "$ko" 2>/dev/null || echo '<none>')"
done
