#!/usr/bin/env bash
# Sign the patched CMP 50HX kernel modules with a local MOK key so they load
# on a Secure Boot host (Ubuntu/Debian).
#
# Secure Boot is enforced by the firmware, but the kernel accepts modules
# signed by any key in its *platform* keyring - and that keyring is fed by the
# machine-owner keys (MOK) that MokManager accepted at boot. So the modules
# need their own signature; an unsigned module is refused even though the
# driver itself is fine (see the install.sh Secure Boot warning).
#
#   sudo bash tools/sign-modules.sh
#       sign /lib/modules/$(uname -r)/updates/*.ko with the default key pair,
#       creating it on first use
#   sudo bash tools/sign-modules.sh --import
#       ... and queue the certificate for enrollment (mokutil --root-pw), so
#       the next boot shows MokManager and asks for the root password
#   sudo bash tools/sign-modules.sh module.ko [more.ko ...]
#   sudo bash tools/sign-modules.sh --key K --cert C module.ko
#
# Options:
#   --key PATH    signing key (default /var/lib/cmp50hx-mok/mok.key)
#   --cert PATH   matching certificate (default .../mok.crt)
#   --import      queue the certificate for MOK enrollment
#   --check       only report the signature state, change nothing
#   -h, --help    this text
#
# The key never leaves the host. Removing Secure Boot does not need it, but a
# host that keeps Secure Boot on does: re-sign after every module rebuild.
set -Eeuo pipefail

key_path=/var/lib/cmp50hx-mok/mok.key
cert_path=/var/lib/cmp50hx-mok/mok.crt
do_import=0
do_check=0
modules=()

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
trap 'die "failed at line ${LINENO}"' ERR

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --key)      key_path="${2:?--key needs a path}"; shift 2 ;;
        --cert)     cert_path="${2:?--cert needs a path}"; shift 2 ;;
        --import)   do_import=1; shift ;;
        --check)    do_check=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        -*)         die "unknown option: $1 (see --help)" ;;
        *)          modules+=("$1"); shift ;;
    esac
done

[[ ${EUID} -eq 0 ]] || die "run as root (sudo)"
krel="$(uname -r)"

# --- locate sign-file --------------------------------------------------------
find_sign_file() {
    local candidate
    for candidate in \
        "/lib/modules/${krel}/build/scripts/sign-file" \
        "/usr/src/linux-headers-${krel}/scripts/sign-file" \
        /usr/lib/linux-kbuild-*/scripts/sign-file \
        /usr/src/linux-headers-*/scripts/sign-file; do
        [[ -x "${candidate}" ]] && { printf '%s\n' "${candidate}"; return 0; }
    done
    return 1
}
sign_file="$(find_sign_file || true)"

# --- default module set ------------------------------------------------------
if [[ ${#modules[@]} -eq 0 ]]; then
    shopt -s nullglob
    modules=(/lib/modules/"${krel}"/updates/*.ko)
    shopt -u nullglob
    [[ ${#modules[@]} -gt 0 ]] \
        || die "no modules in /lib/modules/${krel}/updates/; run install.sh first"
fi

# --- report only -------------------------------------------------------------
signer_of() { modinfo -F signer "$1" 2>/dev/null || true; }

if [[ ${do_check} -eq 1 ]]; then
    wanted="$(openssl x509 -in "${cert_path}" -noout -subject 2>/dev/null | sed 's/.*CN *= *//' || true)"
    for m in "${modules[@]}"; do
        s="$(signer_of "$m")"
        if [[ -z "${s}" ]]; then
            printf 'UNSIGNED  %s\n' "$m"
        elif [[ -n "${wanted}" && "${s}" == "${wanted}" ]]; then
            printf 'OK        %s (signer %s)\n' "$m" "${s}"
        else
            printf 'OTHER_KEY %s (signer %s)\n' "$m" "${s}"
        fi
    done
    exit 0
fi

[[ -n "${sign_file}" ]] || die "sign-file not found; install linux-headers-${krel}"
command -v openssl >/dev/null || die "openssl not found; apt-get install openssl"

# --- key pair (created once per host) ---------------------------------------
if [[ ! -f "${key_path}" || ! -f "${cert_path}" ]]; then
    mkdir -p "$(dirname "${key_path}")"
    chmod 0700 "$(dirname "${key_path}")"
    log "generating MOK key pair in $(dirname "${key_path}")"
    openssl req -new -x509 -newkey rsa:4096 -nodes -days 3650 \
        -keyout "${key_path}" -out "${cert_path}" \
        -subj "/CN=CMP 50HX local module signing $(hostname)-$(date -u +%Y%m%d)/" \
        -addext "basicConstraints=critical,CA:FALSE" \
        -addext "keyUsage=digitalSignature" \
        -addext "extendedKeyUsage=codeSigning" >/dev/null 2>&1
    chmod 0400 "${key_path}"
    log "key:  ${key_path}"
    log "cert: ${cert_path}"
fi

cn="$(openssl x509 -in "${cert_path}" -noout -subject | sed 's/.*CN *= *//')"
log "signing with CN=${cn}"

# --- sign -------------------------------------------------------------------
for m in "${modules[@]}"; do
    [[ -f "${m}" ]] || die "module not found: ${m}"
    "${sign_file}" sha512 "${key_path}" "${cert_path}" "${m}"
    s="$(signer_of "$m")"
    [[ "${s}" == "${cn}" ]] || die "signature verification failed for ${m} (signer '${s}')"
    log "signed ${m}"
done

# --- optional enrollment ----------------------------------------------------
if [[ ${do_import} -eq 1 ]]; then
    command -v mokutil >/dev/null || die "mokutil not found; apt-get install mokutil"
    if mokutil --test-key "${cert_path}" 2>/dev/null | grep -q 'is already enrolled'; then
        log "certificate already enrolled - nothing queued"
    else
        # --root-pw: MokManager asks for the root password at the next boot
        # instead of a one-off password typed now.
        mokutil --import --root-pw "${cert_path}"
        log "certificate queued: on the next boot choose 'Enroll MOK' -> Continue,"
        log "enter the root password, then 'Reboot'"
    fi
fi

cat <<EOF

Signed ${#modules[@]} module(s) for kernel ${krel}.
  verify now:      sudo bash ${BASH_SOURCE[0]} --check
  load now:        sudo modprobe nvidia    (fails unless the MOK is enrolled)
  enroll the key:  sudo mokutil --import --root-pw ${cert_path}   then reboot

Rollback (Secure Boot off instead): the modules need no signature once
Secure Boot is disabled; deleting the MOK key pair changes nothing else.
EOF
