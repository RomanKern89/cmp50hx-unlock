# Finishing the unlock: one pass, one reboot, one human step

The stock driver re-applies the fused speed selectors during GR initialisation,
so a pre-driver write cannot survive (see `../RESULTS.md`). The project's
driver-side patch performs the same privileged write from inside the driver and
does survive — but its module must be locally signed, and Secure Boot only
accepts a locally signed module once that key is enrolled.

Everything except the enrolment is automated.

## What is already in place

| | |
|---|---|
| patched driver | `artifacts/610.43.03-<kver>/` — already built for this kernel |
| userland | `cache/NVIDIA-Linux-x86_64-610.43.03.run`, 461 MB, SHA-256 matches the value pinned in `install.sh` |
| key pair | created by `tools/sign-modules.sh` at `/var/lib/cmp50hx-mok/` |
| enrolled keys | only Canonical's — enrolment has not happened yet |

## Step 1 — everything that can be automated (no person needed)

```sh
sudo /opt/cmp50hx-unlock/userspace-unlock/tools/finish-mok.sh
```

It installs the 610.43.03 userland and the patched modules, signs the modules
with the repository's MOK key, queues the certificate for enrolment, rebuilds
the initramfs and verifies the signature state. It needs no network.

## Step 2 — the one human step (about two minutes, at the machine)

```sh
sudo reboot
```

At the blue **MokManager** screen:

```
Enroll MOK -> Continue -> Yes -> <root password> -> Reboot
```

`tools/sign-modules.sh` queues the certificate with `mokutil --import
--root-pw`, so MokManager asks for the **root password**, not a one-time one.

## Step 3 — verify

```sh
sudo ./finish-mok.sh --check          # module version, signer, enrolment
cat /sys/module/nvidia/version        # 610.43.03
cd /opt/cmp50hx-unlock/verify
./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json
cd /opt/cmp50hx-unlock/userspace-unlock && ./cmp50-fma 200000 0
```

Success looks like `PASS_CMP50HX_ISSUE_RATE_AND_COUNTS` and a large jump in
FP32 throughput (the locked state measures ~6.4 TFLOP/s here).

## Recovery

If MokManager is skipped or the patched driver does not come up, SSH still
works and the stock module is untouched:

```sh
sudo /opt/cmp50hx-unlock/userspace-unlock/tools/rollback-patched.sh
sudo reboot
```

`rollback-patched.sh` only moves `/lib/modules/<kver>/updates/nvidia*.ko` aside;
the Canonical-signed 615 module in `kernel/` is never touched, so the cards
return to their current state.

## Pitfalls found during the investigation

- **The `.run` replaces the userland with 610.43.03.** If the patched module is
  then removed (rollback, or a failed enrolment), the machine is left with a 615
  kernel module and a 610.43 userland: the cards work, but `nvidia-smi` reports
  `Driver/library version mismatch`. `finish-mok.sh` now compares the module and
  NVML versions before letting you reboot, and the fix is to reinstall the
  matching userland:

  ```sh
  sudo apt-get install --reinstall nvidia-utils-615 libnvidia-compute-615 \
       libnvidia-gl-615 libnvidia-cfg1-615 libnvidia-encode-615 \
       libnvidia-decode-615 libnvidia-extra-615 libnvidia-fbc1-615 && sudo ldconfig
  ```

- **An unsigned module in `updates/` is a boot hazard.** It shadows the signed
  distro module *and* gets copied into the initramfs, so the next boot comes up
  without a GPU driver. Always keep the patched modules out of
  `/lib/modules/<kver>/updates/` until the key is enrolled; `rollback-patched.sh`
  does that, and `--check` shows which module would load.
- **The pre-driver service must stay off.** `systemctl disable cmp50-unlock` —
  the patched driver performs the unlock itself.
- **A rebuilt module needs re-signing.** After any kernel update, run
  `tools/sign-modules.sh` again (it signs everything in `updates/`) and rebuild
  the initramfs; an unsigned rebuild is refused at load time.
- **Do not let DKMS shadow the module.** A DKMS-built `nvidia.ko` lands in
  `updates/` and is signed with a key that may not be enrolled; the signed
  distro module in `kernel/` then stops being used.
