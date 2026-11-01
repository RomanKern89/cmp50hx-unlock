# Finishing the unlock after the MOK key is enrolled

This is the only path left on this host (see `../RESULTS.md` section 6): the
stock driver re-applies the fused speed selectors during GR initialisation, so a
pre-driver write cannot survive. The project's driver-side patch performs the
same privileged write from inside the driver, and the module that carries it has
to be locally signed — which Secure Boot accepts only if its key is enrolled.

Everything that can be prepared without physical access is prepared here.

## Prerequisites already in place

| | |
|---|---|
| key pair | `/var/lib/shim-signed/mok/MOK.priv` + `MOK.der`, subject `lmserver Secure Boot Module Signature key` |
| enrolled keys | only Canonical's — `mokutil --list-enrolled` shows one `[key]` |
| patched driver sources | `patches/cmp50hx/01-cmp50-stockflow.patch` against NVIDIA 610.43.03 |
| build inputs | kernel headers, gcc, make, dkms, ~200 GB free |

## Step 1 — enrol the key (about two minutes at the machine)

```sh
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der
# it asks for a one-time password; remember it
sudo reboot
```

At the blue **MokManager** screen: `Enroll MOK` → `Continue` → `Yes` → type the
password → `Reboot`.

Check it took effect:

```sh
mokutil --list-enrolled | grep -c '^\[key'   # 2
```

## Step 2 — build, sign and install the patched driver

```sh
sudo /home/lmserver/cmp50hx-unlock/userspace-unlock/tools/finish-mok.sh
```

The script refuses to do anything unless two keys are enrolled, signs every
built module with the local key, installs it into
`/lib/modules/$(uname -r)/updates/cmp50hx/` (which takes precedence over the
stock `kernel/` module), rebuilds the initramfs and reboots.

If the artifacts are missing it tells you to run, once:

```sh
cd /home/lmserver/cmp50hx-unlock
bash ./build.sh --card cmp50hx          # artifacts only, nothing is installed
```

## Step 3 — verify

```sh
# the driver must be the patched 610.43.03, not the stock 615
cat /sys/module/nvidia/version
modinfo -F signer nvidia                     # lmserver Secure Boot Module Signature key

# the acceptance test from the project itself: all nine modifiers must be 0
cd /home/lmserver/cmp50hx-unlock/verify
./rm_issue_rate 0 > /tmp/rm.json
python3 verify.py /tmp/rm.json               # PASS_CMP50HX_ISSUE_RATE_AND_COUNTS

# and the throughput
cd /home/lmserver/cmp50hx-unlock/userspace-unlock
./cmp50-fma 200000 0                         # expect well above 6.4 TFLOP/s
```

## Rollback

```sh
sudo rm -rf /lib/modules/$(uname -r)/updates/cmp50hx
sudo depmod -a
sudo update-initramfs -u
sudo reboot
```

The stock, Canonical-signed 615 module loads again; the cards return to the
locked state exactly as they are now.

## Known pitfalls recorded during the investigation

- **Userspace version must match the module.** A 610.x kernel module needs the
  610.x user libraries (NVML/CUDA refuse a mismatch). `nvidia-driver-610` is
  installed but its libraries were replaced by 615 at some point — check
  `nvidia-smi` after the first boot and, if it complains about a version
  mismatch, install the matching userspace from the same 610.43.03 run file.
- **The driver must actually be the patched build.** If `/sys/module/nvidia/version`
  still reports 615, the `updates/` copy did not win; check that the `.ko` name
  matches what `modinfo -n nvidia` resolves to.
- **Do not let the pre-driver service run as well.** `systemctl disable
  cmp50-unlock` — the patched driver does the unlock itself, and two writers on
  the same block only add risk.
- **A failed patched boot is recoverable over SSH only if the network comes up.**
  Keep the stock module available (this rollback) before rebooting.
