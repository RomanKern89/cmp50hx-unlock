#!/bin/bash
# Install the CMP 50HX userspace unlock as a boot-time service.
#
#   sudo ./install.sh            # install / update
#   sudo ./install.sh --remove   # uninstall and restore normal driver loading
#
# The unlock has to run before nvidia ever probes the cards, because a card that
# a driver has already initialised this power cycle no longer accepts it. The
# installer therefore:
#   1. installs the binary, firmware and config,
#   2. blacklists nvidia for the initramfs (rd.driver.blacklist=) and for the
#      real root (/etc/modprobe.d),
#   3. installs cmp50-unlock.service, which unlocks the cards and then loads
#      the driver.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
BIN=/usr/bin/cmp50-unlock-us
FW=/lib/firmware/cmp50hx
BLOBS="$here/../efi-unlock/blobs"
CONF=/etc/default/cmp50-unlock
MODPROBE=/etc/modprobe.d/zz-cmp50-unlock.conf
UNIT=/etc/systemd/system/cmp50-unlock.service
DOCDIR=/usr/share/doc/cmp50hx-unlock
GRUBFILE=/etc/default/grub
BLACKLIST_CMDLINE="rd.driver.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm"

usage() {
    echo "usage: $0 [--remove] [--fb 20g|10g|auto]" >&2
    exit 2
}

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }

if [ "${1:-}" = "--remove" ]; then
    systemctl disable --now cmp50-unlock.service 2>/dev/null || true
    rm -f "$UNIT" "$MODPROBE" "$CONF" "$BIN"
    rm -rf "$FW" "$DOCDIR"
    if [ -f "$GRUBFILE" ]; then
        sed -i "s/ *$BLACKLIST_CMDLINE//g" "$GRUBFILE"
        command -v update-grub >/dev/null && update-grub
    fi
    systemctl daemon-reload || true
    command -v update-initramfs >/dev/null && update-initramfs -u || true
    echo "removed. Next boot loads nvidia normally."
    exit 0
fi

FB=20g
while [ $# -gt 0 ]; do
    case "$1" in
    --fb) [ $# -ge 2 ] || usage; FB=$2; shift 2 ;;
    *) usage ;;
    esac
done

[ -x "$here/cmp50-unlock-us" ] || { echo "build first: make" >&2; exit 1; }
for f in v67_payload.bin booter_ucode_prod.bin gsp_rm_boot_dbg.bin \
         fwsec_50hx_prod.bin bl_gsp_tu102.bin; do
    [ -r "$BLOBS/$f" ] || { echo "missing blob: $BLOBS/$f" >&2; exit 1; }
done

install -D -m 0755 "$here/cmp50-unlock-us" "$BIN"
mkdir -p "$FW"
for f in v67_payload.bin booter_ucode_prod.bin gsp_rm_boot_dbg.bin \
         fwsec_50hx_prod.bin bl_gsp_tu102.bin; do
    install -m 0644 "$BLOBS/$f" "$FW/$f"
done

printf 'CMP50_FB=%s\n' "$FB" > "$CONF"
install -D -m 0644 "$here/deploy/zz-cmp50-unlock.conf" "$MODPROBE"
install -D -m 0644 "$here/deploy/cmp50-unlock.service" "$UNIT"
install -D -m 0644 "$here/README.md" "$DOCDIR/README.md"

# rd.driver.blacklist keeps nvidia out of the dracut initramfs, where it would
# otherwise bind the cards at ~11 s, long before userspace.
if [ -f "$GRUBFILE" ]; then
    if ! grep -q "$BLACKLIST_CMDLINE" "$GRUBFILE"; then
        sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=\"\(.*\)\"|GRUB_CMDLINE_LINUX_DEFAULT=\"\1 $BLACKLIST_CMDLINE\"|" \
            "$GRUBFILE"
        sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT=" /GRUB_CMDLINE_LINUX_DEFAULT="/' "$GRUBFILE"
    fi
    command -v update-grub >/dev/null && update-grub
fi

systemctl daemon-reload
systemctl enable cmp50-unlock.service

echo
echo "installed:"
echo "  binary   $BIN"
echo "  firmware $FW"
echo "  config   $CONF (CMP50_FB=$FB)"
echo "  service  $UNIT (runs the unlock, then loads nvidia)"
echo "  cmdline  $BLACKLIST_CMDLINE"
echo
echo "Reboot to apply. Afterwards:"
echo "  systemctl status cmp50-unlock"
echo "  journalctl -b -u cmp50-unlock"
echo "  cmp50-fma 200000 0        # FP32 should be far above the stock value"
