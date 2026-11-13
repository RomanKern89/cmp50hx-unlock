# CMP 50HX — PCIe Gen2 unlock in two steps

This turns a CMP 50HX (`10de:1e09`) from PCIe **Gen1** (2.5 GT/s) to **Gen2**
(5.0 GT/s), **including boards where the card sits behind a PLX switch** (e.g.
ASUS X99-E WS), where Gen2 used to be impossible. The Gen2 retrain is done by the
patched kernel module itself at boot — no manual step, no BIOS slot change.

> Gen2 is the hardware ceiling of these cards: the TU102 clamps the advertised
> link capability to Gen2 even when Gen3 is requested. See `docs/PCIE-GEN3.md`.

## Before you start (once)

- **OS:** Ubuntu or Debian (or a derivative).
- **Secure Boot: OFF** in firmware setup. The patched module is unsigned; with
  Secure Boot on it will not load (or sign it with your own MOK).
- Root access (`sudo`).

## Step 1 — run the installer

```bash
curl -fsSL https://raw.githubusercontent.com/RomanKern89/cmp50hx-unlock/master/install.sh | sudo bash
```

It auto-detects the card, installs the NVIDIA 610.43.03 userland, builds the
patched modules **for the running kernel**, installs them with a backup of the
old ones, and bakes them into the initramfs. It never reboots or unloads a live
driver. Wait for `PASS_CMP50HX_INSTALL`.

> Until the PLX-Gen2 fix is merged to `master`, install from the branch that has
> it by prefixing the command with its tarball URL:
> ```bash
> curl -fsSL https://raw.githubusercontent.com/RomanKern89/cmp50hx-unlock/feat/pcie-gen3-adaptive/install.sh \
>   | sudo CMP_UNLOCK_REPO_TARBALL=https://github.com/RomanKern89/cmp50hx-unlock/archive/refs/heads/feat/pcie-gen3-adaptive.tar.gz bash
> ```

## Step 2 — reboot

```bash
sudo reboot
```

The patched module loads at boot, adopts the Gen2 capability, and retrains each
card's upstream link to Gen2 before GSP comes up — so Gen2 sticks. That's it.

## Verify

```bash
# per card BDF (list them: lspci -Dnn | grep 1e09)
sudo python3 /opt/cmp50hx-unlock/verify/gen3_retrain.py 0000:05:00.0
nvidia-smi --query-gpu=pci.bus_id,pcie.link.gen.current,pcie.link.width.current --format=csv
cat /sys/bus/pci/devices/0000:05:00.0/current_link_speed      # -> 5.0 GT/s PCIe
sudo dmesg | grep CMP50_GEN2                                   # UPSTREAM_RETRAIN / RETRAIN_PASS
```

Expected: `pcie.link.gen.current = 2`, `current_link_speed = 5.0 GT/s`,
`dmesg` shows `CMP50_GEN2: UPSTREAM_RETRAIN ... cur_speed=2` and
`RETRAIN_PASS`, and no `Xid`/`AER` errors.

## Important: kernel updates

The module is built for **one exact kernel version**. If `apt` installs a newer
kernel and you reboot into it, there is no matching module and the GPUs come up
without the driver. Two options:

- **Re-run Step 1** after a kernel update (it rebuilds for the new running
  kernel), then reboot; **or**
- **Pin the kernel** you built for, e.g. keep booting `7.0.0-42-generic`:
  set it as the GRUB default (`Advanced options for Ubuntu` → that kernel) and
  `sudo apt-mark hold linux-image-generic linux-headers-generic` to stop
  automatic kernel upgrades.

## Rollback

```bash
# restore the modules the installer backed up (path printed in its summary):
sudo cp -a /opt/cmp50hx-unlock/backups/modules-*/lib/modules/$(uname -r)/updates/* \
           /lib/modules/$(uname -r)/updates/ 2>/dev/null
sudo depmod -a "$(uname -r)"
sudo update-initramfs -u -k "$(uname -r)"
sudo reboot
# NVIDIA userland: sh /opt/cmp50hx-unlock/cache/NVIDIA-Linux-x86_64-610.43.03.run --uninstall
```

## What each card ends up with

| | Gen1 (stock) | Gen2 (this unlock) |
|---|---|---|
| Link speed | 2.5 GT/s | **5.0 GT/s** |
| Host↔GPU bandwidth | ~1x | **~2x** |
| Compute (SM/Tensor) | full | full (unchanged; SM-bound) |

The link generation only bounds host↔GPU transfers; the compute unlock is
independent and unaffected.
