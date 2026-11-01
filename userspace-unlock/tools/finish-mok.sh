#!/bin/bash
# Install the patched 610.43.03 driver, which performs the unlock itself.
#
# Refuses to run until the local MOK key is enrolled: before that the modules
# would be rejected at load time and the machine would come up without a GPU
# driver. See ../FINISH-MOK.md for the two-minute enrolment step.
#
#   sudo ./finish-mok.sh            # install, then verify instructions
#   sudo ./finish-mok.sh --reboot   # install and reboot
#   sudo ./rollback-patched.sh      # back to the stock signed driver
set -euo pipefail

K=$(uname -r)
STAGE=${CMP50_STAGE:-/opt/cmp50hx/driver-610.43.03}
DST=/lib/modules/$K/updates/cmp50hx
EXPECT_SIGNER="lmserver Secure Boot Module Signature key"
REBOOT=0
[ "${1:-}" = "--reboot" ] && REBOOT=1

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }

keys=$(mokutil --list-enrolled 2>/dev/null | grep -c '^\[key' || true)
if [ "${keys:-0}" -lt 2 ]; then
    cat >&2 <<EOF
The local MOK key is not enrolled yet ($keys key(s) enrolled).

  sudo mokutil --import /var/lib/shim-signed/mok/MOK.der
  sudo reboot
  # at MokManager: Enroll MOK -> Continue -> Yes -> password -> Reboot

Then run this script again. Details: $(dirname "$0")/../FINISH-MOK.md
EOF
    exit 1
fi

for ko in "$STAGE"/nvidia*.ko; do
    [ -f "$ko" ] || { echo "staged module missing: $ko" >&2; exit 1; }
    signer=$(modinfo -F signer "$ko" 2>/dev/null || true)
    if [ "$signer" != "$EXPECT_SIGNER" ]; then
        echo "$(basename "$ko"): signer '${signer:-<none>}' is not the enrolled key" >&2
        echo "re-sign it: $0 expects the output of tools/sign-stage.sh" >&2
        exit 1
    fi
done

install -d "$DST"
install -m 0644 "$STAGE"/nvidia*.ko "$DST"/
depmod "$K" 2>/dev/null || depmod -a
command -v update-initramfs >/dev/null && update-initramfs -u || true

echo "installed:"
for ko in "$DST"/nvidia*.ko; do
    printf '  %-24s %s\n' "$(basename "$ko")" "$(modinfo -F version "$ko")"
done
echo
echo "active module on next boot: $(modinfo -F filename nvidia 2>/dev/null || echo '?')"
echo
cat <<EOF
After the reboot verify:
  cat /sys/module/nvidia/version                 # 610.43.03
  modinfo -F signer nvidia                       # $EXPECT_SIGNER
  cd $(dirname "$0")/../../verify && ./rm_issue_rate 0 > /tmp/rm.json
  python3 verify.py /tmp/rm.json                 # PASS_CMP50HX_ISSUE_RATE_AND_COUNTS
  cd $(dirname "$0")/.. && ./cmp50-fma 200000 0  # well above 6.4 TFLOP/s
EOF

if [ "$REBOOT" = "1" ]; then
    echo "rebooting..."
    sync
    systemctl reboot
fi
