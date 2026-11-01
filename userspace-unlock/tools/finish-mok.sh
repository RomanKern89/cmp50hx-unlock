#!/bin/bash
# Install the patched driver and set up Secure Boot for it.
#
# One pass does everything that does not need a person:
#   1. installs the matching 610.43.03 userland and the patched modules
#   2. signs the modules with the repository's MOK key
#   3. queues that key for enrolment (MokManager will ask for the root password)
#   4. rebuilds the initramfs with the signed modules
#
# Then exactly one reboot needs a human: at MokManager choose
# `Enroll MOK` -> `Continue` -> `Yes` -> the root password -> `Reboot`.
#
#   sudo ./finish-mok.sh            # install, sign, queue enrolment
#   sudo ./finish-mok.sh --reboot   # ... and reboot immediately
#   sudo ./finish-mok.sh --check    # report the current state, change nothing
#   sudo ./rollback-patched.sh      # back to the stock signed driver
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
K=$(uname -r)
UPDATES=/lib/modules/$K/updates
CERT=/var/lib/cmp50hx-mok/mok.crt
MODE=install
REBOOT=0

for a in "$@"; do
    case "$a" in
        --reboot) REBOOT=1 ;;
        --check)  MODE=check ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" = "0" ] || { echo "run as root" >&2; exit 1; }

key_enrolled() {
    [ -r "$CERT" ] || return 1
    mokutil --test-key "$CERT" 2>/dev/null | grep -q 'is already enrolled'
}

if [ "$MODE" = check ]; then
    echo "kernel:            $K"
    echo "module in use:     $(modinfo -n nvidia 2>/dev/null || echo none)"
    echo "module version:    $(modinfo -F version nvidia 2>/dev/null || echo none)"
    echo "module signer:     $(modinfo -F signer "$UPDATES/nvidia.ko" 2>/dev/null || modinfo -F signer nvidia 2>/dev/null || echo none)"
    echo "updates/ modules:  $(ls "$UPDATES"/nvidia*.ko 2>/dev/null | wc -l)"
    echo "MOK cert present:  $([ -r "$CERT" ] && echo yes || echo no)"
    echo "MOK enrolled:      $(key_enrolled && echo yes || echo no)"
    echo "enrolled keys:     $(mokutil --list-enrolled 2>/dev/null | grep -c '^\[key' || true)"
    exit 0
fi

echo "== 1/4 installing the patched driver and the 610.43.03 userland =="
# The .run is pinned and already cached in $REPO/cache, so this needs no network.
( cd "$REPO" && bash ./install.sh --card cmp50hx )

echo "== 2/4 signing the installed modules and queuing the key =="
( cd "$REPO" && bash ./tools/sign-modules.sh --import )

echo "== 3/4 rebuilding the initramfs with the signed modules =="
update-initramfs -u

echo "== 4/4 verifying the signature state and the version match =="
( cd "$REPO" && bash ./tools/sign-modules.sh --check )

# The .run installs its own userland; a mismatch between it and the module that
# will load leaves nvidia-smi/CUDA broken even though the unlock worked.
mod_ver=$(modinfo -F version "$UPDATES/nvidia.ko" 2>/dev/null || echo "?")
lib_link=$(readlink -f /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1 2>/dev/null || echo "")
lib_ver=$(printf '%s' "$lib_link" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+$' || echo "?")
echo "module version:   $mod_ver"
echo "userspace (NVML): $lib_ver"
if [ "$mod_ver" != "$lib_ver" ]; then
    cat >&2 <<EOF

STOP: the module that will load ($mod_ver) and the NVML userland ($lib_ver)
differ. Rebooting like this gives a working unlock but a broken nvidia-smi.
Fix the userland before rebooting - install.sh normally does this itself:

  sudo sh $REPO/cache/NVIDIA-Linux-x86_64-610.43.03.run --silent \\
      --no-kernel-modules --no-dkms --no-backup

EOF
    exit 1
fi


cat <<EOF

Everything is prepared. The last step needs a person, once:

  sudo reboot
  # at the blue MokManager screen:
  #   Enroll MOK -> Continue -> Yes -> root password -> Reboot

After that the patched driver loads and the cards are unlocked. Check with:

  $(dirname "$0")/finish-mok.sh --check
  cd $REPO/verify && ./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json

If MokManager is skipped or the driver does not come up, recovery over SSH is:

  sudo $(dirname "$0")/rollback-patched.sh && sudo reboot
EOF

if [ "$REBOOT" = "1" ]; then
    echo "rebooting..."
    sync
    systemctl reboot
fi
