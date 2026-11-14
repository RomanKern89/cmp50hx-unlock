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

## Tested on

Verified live on the hardest topology — a card **behind a PLX switch**, where Gen2
used to be impossible:

- **Board:** ASUS X99-E WS (Intel X99). Both CMP 50HX sit **behind cascaded PLX
  PEX8747 switches** (card → PEX8747 → PEX8747 → CPU root port), not on a
  CPU-direct slot.
- **GPUs:** GPU0 `05:00.0` (x16) and GPU1 `0a:00.0` (x8), both `10de:1e09`.
- **OS:** Ubuntu, kernel `7.0.0-42-generic`; NVIDIA driver `610.43.03`.
- **Result (measured):** both cards `current_link_speed = 5.0 GT/s`,
  `pcie.link.gen.current = 2`, **0 Xid / 0 AER**, stable under continuous load.
  `dmesg` shows `CMP50_GEN2: UPSTREAM_RETRAIN ... cur_speed=2 attempt=0` followed by
  `ALREADY_GEN2 ... (not re-kicking)`.

## How it works (why Gen2 sticks behind a PLX)

On PLX boards earlier attempts lost Gen2 because the **signed GSP firmware reverts**
the link capability back to Gen1 shortly after the driver raises it. The fix acts in
the **post-booter window** — after the SEC2 booter runs but **before GSP-RM loads**:

1. it adopts the higher (Gen2) link capability on the card, and
2. **retrains the card's immediate upstream link (the PLX port) to Gen2 right there**,
   so the link is already physically trained to Gen2 before GSP comes up.

GSP does not fight a link that is already trained — it leaves the Gen2 link alone, so
Gen2 survives into the OS. (An OS-side retrain alone was too late: GSP had already
reverted and re-locked.) That is what makes Gen2 work behind the dual-PEX8747
topology of the X99-E WS. The code is the post-booter hook in
`patches/cmp50hx/01-cmp50-stockflow.patch` (`s_cmp50RetrainUpstreamGen2`).

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
