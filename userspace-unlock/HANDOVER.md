# Handover: state of the two CMP 50HX on this host

Everything below is verified on the machine; see `RESULTS.md` for the evidence
behind each claim and `README.md` for the userspace tool itself.

## Machine state (2026-11-01, after the work)

| | |
|---|---|
| kernel | `7.0.0-42-generic`, Secure Boot on, `lockdown=integrity` |
| driver | `615.71.09`, Canonical-signed, from `kernel/nvidia-615-open` |
| userland | `615.71.09` (NVML matches the module) |
| cards | both `10de:1e09`, 20480 MiB, `nvidia-smi` healthy |
| `/lib/modules/.../updates` | empty — nothing shadows the signed module |
| DKMS | no nvidia registration, no local builds |
| modprobe.d | `cmp-unlock.conf` only (`blacklist nouveau`) |
| `cmp50-unlock.service` | installed, **disabled** |
| staged patched driver | `/opt/cmp50hx/driver-610.43.03/` (signed, not installed) |
| pinned `.run` | `cache/NVIDIA-Linux-x86_64-610.43.03.run`, SHA-256 matches `install.sh` |
| MOK | one key enrolled (Canonical); the local key exists but is **not** enrolled |

## What is proven

* The userspace unlock performs the EFI's hardware change: the applied-rate
  readout at `0x00409668` goes from `0x016db6ed` to `0x00000000` (all nine SM
  issue-rate fields at full speed).
* It runs correctly before the driver from a systemd unit
  (`rd.driver.blacklist` keeps nvidia out of the initramfs), on both cards,
  in about five seconds each.
* The stock driver then re-applies the fused selectors during GR init, so the
  card is clamped again before any workload runs.

## What is exhausted (all tested, all negative)

pre-driver timing · D0/`disable_idle_d3` · no-FLR · real GSP `.fwimage` ·
`RMOverrideSmSpeedSelect` on 615-open (the string is absent from that build) ·
the same key on the signed proprietary 580 (string present, accepted, inert) ·
non-GSP init on 580 (`NVreg_EnableGpuFirmware=0`, clamp unchanged) ·
signed 610 module (does not exist for -42; -38 has a packaging skew) ·
`resource0` read/write (`EIO`) · clearing `sig_enforce` (`bool_enable_only`) ·
DKMS/local-key modules (key not enrolled).

## What remains: one physical step

The project's driver-side patch is the only mechanism that survives, and its
module has to be signed by an enrolled key.

```sh
sudo /opt/cmp50hx-unlock/userspace-unlock/tools/finish-mok.sh
```

installs the 610.43.03 userland and patched modules, signs them, queues the key
and rebuilds the initramfs (no network needed). Then:

```sh
sudo reboot
# MokManager: Enroll MOK -> Continue -> Yes -> root password -> Reboot
```

Verify:

```sh
sudo tools/finish-mok.sh --check
cd ../verify && ./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json
cd ../userspace-unlock && ./cmp50-fma 200000 0
```

Expected: `PASS_CMP50HX_ISSUE_RATE_AND_COUNTS`, FP32 well above the ~6.4 TFLOP/s
measured in the clamped state.

## Rollback

```sh
sudo userspace-unlock/tools/rollback-patched.sh   # stock module back
sudo reboot
```

Nothing in `kernel/` is touched, so the stock driver always remains available.

## If a future kernel update arrives

`linux-modules-nvidia-615-open-generic` is installed, so the signed module
follows the kernel automatically. If the patched driver is ever active, re-run
`tools/sign-modules.sh` after the update and rebuild the initramfs — an unsigned
rebuild is refused at load time.
