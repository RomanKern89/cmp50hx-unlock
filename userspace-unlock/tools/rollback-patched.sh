#!/bin/bash
# Remove the patched driver and return to the stock, Canonical-signed module.
# This is the recovery path if the patched boot does not come up healthy; it
# only touches /lib/modules/<kver>/updates, so the stock module in kernel/ is
# never destroyed.
set -euo pipefail

K=$(uname -r)
UPDATES=/lib/modules/$K/updates
STAMP=$(date +%Y%m%d-%H%M%S)
ASIDE=/var/tmp/cmp50hx-patched-$STAMP

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }

found=0
mkdir -p "$ASIDE"
for mod in nvidia nvidia-uvm nvidia-modeset nvidia-drm nvidia-peermem; do
    src="$UPDATES/$mod.ko"
    [ -f "$src" ] || continue
    mv "$src" "$ASIDE/$mod.ko"
    found=$((found + 1))
done
echo "moved $found module(s) aside to $ASIDE"

depmod "$K" 2>/dev/null || depmod -a
command -v update-initramfs >/dev/null && update-initramfs -u || true

echo "stock module now resolves to: $(modinfo -n nvidia 2>/dev/null || echo '?')"
echo "version: $(modinfo -F version nvidia 2>/dev/null || echo '?')"
echo "signer:  $(modinfo -F signer nvidia 2>/dev/null || echo '?')"
echo "reboot to return to it; the cards will be locked again."
