# Userspace unlock (Linux, Secure Boot on, no console)

`cmp50-unlock-us` runs the same hardware sequence as `../efi-unlock/unlock50x_v1.c`
from Linux userspace, before the stock `nvidia` driver binds the card. It exists
because this host has no console and keeps Secure Boot enabled — the EFI build
cannot be launched, and no self-built kernel module can be loaded.

## Why the platform layer looks like this

| Need | Obvious way | What actually happens here |
| --- | --- | --- |
| BAR0 access | `/sys/.../resource0` mmap | refused: lockdown `integrity` ("direct PCI access is restricted") |
| BAR0 access | `uio_pci_generic` + `/dev/uioN` | maps fine, **but** the driver never calls `pci_set_master()` and nvidia's remove path clears the bit, so the GPU cannot DMA its WPR meta → booter exits 0x91 |
| `PCI_COMMAND_MASTER` | pci-sysfs `config` write | refused with `EPERM` under lockdown |
| DMA buffers | `kmalloc`/`mmap` | userspace needs *physical* addresses; `/proc/self/pagemap` only exposes PFNs to `CAP_SYS_ADMIN`, and the booter only reads from above 4 GiB |

So the tool uses **vfio-pci in no-IOMMU mode**:

* vfio hands out BAR0 through its own region, not the pci-sysfs path lockdown blocks;
* `PCI_COMMAND` is writable through the **VFIO config region** — a different code
  path than the blocked sysfs file — which is how bus master gets enabled;
* this host has no IOMMU (`/sys/kernel/iommu_groups` is empty), so there is no
  translation and the addresses the firmware sees are plain physical addresses.

The DMA buffers live in 2 MiB hugepages (`MAP_HUGETLB`), whose physical
addresses come from pagemap. Every buffer is placed above 4 GiB: on a locked
card the booter's sysmem reads below 4 GiB are gated.

## Sequence

Ported 1:1 from `u50_unlock_card()`:

1. `probe` — geometry, chip id, GFW wait, PTIMER seed, arena/blob validation. No writes.
2. `fwsec` — ENGINE-reset the POST-time GFW, boot the 50HX FWSEC on GSP through
   the generic TU102 bootloader with the FRTS command, wait for WPR2.
3. `booter` — DIRECT_SEC2 port load of the TU102 booter (NS IMEM, secure IMEM,
   DMEM with the prod signature at +0x200), `BOOTVEC=0`, mailbox = WPR meta
   physical address, `STARTCPU`; the published V67 payload makes the booter do
   the privileged writes.
4. `unlock` — verify `SS0=0x88888888`, `SS1=0x8`, sanitize SEC2. The register
   that says whether the hardware *applied* the override is the readout at
   `0x00409668`: nine 3-bit issue-rate fields, **0 = full speed**. `PLM` staying
   at `0xffffff8f` is normal ("staged"), it is not a failure.
5. `flr` — PCIe FLR so the card comes back cold and WPR2-down, which is what
   the next driver expects; the selectors live in fuse-override and survive it.
6. `handoff` — unbind vfio-pci, clear `driver_override`, bind `nvidia` again.

> **Read `RESULTS.md` before trusting a "successful" run.** On this host the
> hardware does take the override (`RATE` goes to 0), and the stock 615 driver
> re-applies the fused selectors at GR init, so the clamp is back before any
> workload runs. The per-register proof and the remaining options are there.

## Framebuffer size must match the card

The WPR layout is anchored at the top of the framebuffer, so a wrong `fbSize`
puts every region in the middle of VRAM and the booter rejects the meta. The
FB-range register (`0x100ce0`) is read automatically, but a **memory-modified
20 GiB CMP 50HX still reports the stock 10 GiB geometry there**, so on those
cards it must be forced:

```sh
sudo ./cmp50-unlock-us --fb 20g
```

A 20 GiB card is correct when, after the `fwsec` stage, WPR2 reads
`04ffee00:04ffe000` (span `0xe00`) — that is the value `docs/20GB_RU.md` records
for this modification.

## Usage

```sh
make
sudo ./cmp50-unlock-us --blobs ../efi-unlock/blobs --fb 20g          # every 10de:1e09
sudo ./cmp50-unlock-us --bdf 0000:05:00.0 --fb 20g --stage fwsec     # stop after FWSEC
```

Stages: `probe`, `fwsec`, `booter`, `unlock`, `flr`, `handoff` (default).
Other flags: `--no-flr`, `--no-handoff`, `--allow-low`, `--fb auto|10g|20g`.

Run under `sudo` with `nr_hugepages` set (the tool needs 24 × 2 MiB pages;
`echo 32 > /proc/sys/vm/nr_hugepages`).

## Risk

If a card stops answering mid-run it stays that way until the machine is power
cycled — the same risk the EFI unlock carries. Every stage except `probe`
writes to the GPU; run them in order the first time.

## Files

* `cmp50-unlock-us.c` — the unlock.
* `cmp50-regs.c` — read-only register dump and the three-write probe.
* `usplat.c` — platform self-check (PFNs, hugepages, vfio master bit).
