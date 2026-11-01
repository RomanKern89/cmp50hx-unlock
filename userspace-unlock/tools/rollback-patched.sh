#!/bin/bash
# Remove the patched driver and return to the stock, Canonical-signed module.
# This is the recovery path if the patched boot does not come up healthy.
set -euo pipefail

K=$(uname -r)
DST=/lib/modules/$K/updates/cmp50hx

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }

if [ -d "$DST" ]; then
    rm -rf "$DST"
    echo "removed $DST"
else
    echo "nothing to remove at $DST"
fi

depmod "$K" 2>/dev/null || depmod -a
command -v update-initramfs >/dev/null && update-initramfs -u || true

echo "stock module now resolves to: $(modinfo -F filename nvidia 2>/dev/null || echo '?')"
echo "reboot to return to it; the cards will be locked again."
