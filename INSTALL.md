# Install guide

Everything below is command-line only and was executed on real hardware
(CMP 50HX, 20 GiB modification, Ubuntu 26.04, kernel 7.0.0-42, driver
610.43.03). If a step behaves differently on your system, the troubleshooting
section at the end lists the failure modes actually met during development.

There are three independent ways to unlock the card. Pick one.

| | Unlocks | Needs | Survives driver load |
| --- | --- | --- | --- |
| **A. Patched driver** | FP32 + INT8 | Secure Boot off (or an enrolled MOK) | **yes** |
| B. Userspace pre-driver | FP32 + INT8 at the register level | nothing extra; works with Secure Boot on | no — the stock driver re-clamps |
| C. UEFI application | FP32 + INT8 before the OS | a display/console to launch it | yes |

Path A is what produces the published numbers and is the one to use.

---

## 0. Before you start

```sh
# 1. Is this the right card?  expect 10de:1e09, and note the memory size
nvidia-smi --query-gpu=index,name,memory.total,pci.bus_id --format=csv
lspci -nn | grep -i 3d

# 2. Secure Boot state
mokutil --sb-state

# 3. Toolchain the installer expects
sudo apt-get install -y build-essential linux-headers-$(uname -r) patch xz-utils
```

**Memory size matters.** The unlock's WPR layout is anchored at the top of the
framebuffer. Cards modified to 20 GiB still report the stock 10 GiB in the
FB-range register, so the userspace tool needs `--fb 20g`. The driver-side patch
reads the real geometry itself, so path A needs no flag.

Back up nothing — nothing is flashed. The only persistent change is a set of
kernel modules in `/lib/modules/<kver>/updates/`, removed by the rollback in
section A5.

---

## A. Patched driver (recommended)

### A1. Make the module loadable

Secure Boot verifies every module against keys it trusts. A locally built
module is accepted only if:

* **Secure Boot is off** — firmware setup → `Boot` → `Secure Boot` →
  `OS Type` = **`Other OS`** (ASUS wording; on other boards: disable Secure Boot
  or clear the platform keys), then `mokutil --sb-state` must say
  `SecureBoot disabled`; **or**
* your signing key is **enrolled**: `sudo mokutil --import <your>.der`, reboot,
  `Enroll MOK` in MokManager.

Without one of these the driver installs but never loads, and the machine boots
with no GPU driver — recoverable (section A5), but avoid it.

### A2. Install

```sh
git clone <this repo> cmp50hx-unlock
cd cmp50hx-unlock
sudo ./install.sh --card cmp50hx
```

What it does, in order: installs build tools and headers, installs the matching
**610.43.03 userland** from NVIDIA's official `.run` (cached in `cache/`, so a
second run needs no network), builds the patched modules with
`build.sh --card cmp50hx`, installs them into
`/lib/modules/$(uname -r)/updates/` with a backup of anything it replaces, runs
`depmod`, installs the optional units, and rebuilds the initramfs. It never
reboots and never unloads a live driver.

### A3. Reboot and confirm

```sh
sudo reboot

# after boot
cat /sys/module/nvidia/version            # 610.43.03
modinfo -n nvidia                         # .../updates/nvidia.ko
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv
```

### A4. Verify the unlock

Two independent checks. The first is NVIDIA's own view, the second is measured
throughput.

```sh
cd verify
./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json
# PASS_CMP50HX_ISSUE_RATE_AND_COUNTS
```

`verify.py` also asserts the CMP SKU and the core counts; on a patched driver
the SKU may read 0 by design, in which case only that last assertion fails while
the nine issue-rate values are already verified above it.

```sh
cd ../userspace-unlock
make
./cmp50-fma 20000 0        # ~12.9 TFLOP/s FP32 (clamped: ~0.4)
./cmp50-dp4a 20000 0       # ~51   TIOP/s  INT8 (clamped: ~1.6)
```

The raw register view, if you want it without the driver:
`NV_PFEAT` at BAR0 `0x00409668` — nine 3-bit fields, **0 = full speed**.

### A5. Rollback

```sh
sudo userspace-unlock/tools/rollback-patched.sh
sudo reboot
```

It moves the patched modules out of `/lib/modules/<kver>/updates/` (the stock,
distro-signed module in `kernel/` is never touched), reruns `depmod` and
rebuilds the initramfs. If the userland was replaced by the 610.43.03 `.run`,
reinstall the matching distro userland too:

```sh
sudo apt-get install --reinstall nvidia-utils-XXXX libnvidia-compute-XXXX \
     libnvidia-gl-XXXX libnvidia-cfg1-XXXX && sudo ldconfig
```

### A6. Kernel updates

After a kernel update the patched modules must be rebuilt for the new kernel and
re-signed (if you use MOK):

```sh
sudo ./install.sh --card cmp50hx
```

Keep the distro's `linux-modules-nvidia-*-generic` meta installed so the stock
driver still follows new kernels if the patched build is not available.

---

## B. Userspace unlock, before the driver binds

For hosts where loading a locally built module is impossible (Secure Boot on, no
MOK enrollment, no console). It talks to the card through `vfio-pci` in no-IOMMU
mode, keeps its DMA buffers in hugepages above 4 GiB, and performs the same
privileged write through the SEC2 booter.

> **Read this first.** The write is proven: the readout register goes from the
> clamped pattern to `0x00000000`. But the *stock* driver re-applies the fused
> selectors during GR init, so on its own this path does not survive into the
> running system. It is useful together with a driver that does not re-clamp, and
> it is the only path when a module cannot be loaded at all. The full evidence is
> in [RESULTS.md](userspace-unlock/RESULTS.md).

### B1. Build

```sh
cd userspace-unlock
make                       # cmp50-unlock-us, cmp50-regs, cmp50-fma, cmp50-dp4a, usplat
sudo ./usplat --vfio 0000:05:00.0    # platform self-check: PFNs, hugepages, BAR0
```

`usplat` proves the three things the tool needs: `/proc/self/pagemap` exposing
PFNs to root, hugepages being physically contiguous, and the vfio path giving
BAR0 plus bus master. If it fails, stop here and read the troubleshooting table.

### B2. Try it once, on one card, with the driver detached

```sh
sudo ./cmp50-unlock-us --blobs ../efi-unlock/blobs --fb 20g --no-flr --hold 4
```

Watch for these lines:

```
entry:  PLM=0xffffff8f SS0=... RATE=0x016db6ed
booter: ... SS0=0x88888888 SS1=0x00000008 RATE=0x00000000
```

`RATE=0` is the point of the exercise. `PLM=0xffffff8f` is the normal "staged"
value, not a failure.

### B3. Deploy at boot (optional)

```sh
sudo ./install.sh --fb 20g            # systemd unit + rd.driver.blacklist for nvidia
sudo reboot
journalctl -b -u cmp50-unlock | tail -20
```

Remove it with `sudo ./install.sh --remove`. Do **not** keep it enabled when the
patched driver is active — two writers on the same block only add risk.

### B4. If you only want to probe

```sh
sudo ./cmp50-unlock-us --stage probe     # reads geometry, blobs, arena; writes nothing
sudo ./cmp50-regs --uio 0                # register dump through uio
```

---

## C. UEFI application

`efi-unlock/` builds `50HXUNLK.EFI`, which runs before the OS and chainloads the
bootloader afterwards. It needs a display and a keyboard to launch and carries
the same "a wedged card stays wedged until power-off" risk. Build and usage are
documented in [efi-unlock/README.md](efi-unlock/README.md); the binary is also
attached to releases.

---

## Troubleshooting (all of these were met on real hardware)

| Symptom | Cause | Fix |
| --- | --- | --- |
| `modinfo -n nvidia` still points at the stock module | a DKMS or `updates/` copy shadows it, or the initramfs still has the old one | remove the shadowing `.ko`, `depmod -a`, `update-initramfs -u`, reboot |
| Machine boots with no GPU driver after installing | the signed module was replaced by one whose key is not enrolled | `tools/rollback-patched.sh && reboot` |
| `nvidia-smi: Driver/library version mismatch` | 610.43.03 userland with a 615 kernel module (or the reverse) | reinstall the userland that matches the module (A5) |
| Booter exits with `mbox0=0x91` | WPR meta layout rejected — wrong framebuffer size, or a card that a driver already initialised this power cycle | pass the right `--fb`, power-cycle, retry |
| `arena: hugepage mmap: Cannot allocate memory` | no hugepages reserved | set `vm.nr_hugepages` (the tool does it itself when it can) or free memory |
| `pagemap` gives PFN 0 | not root | run as root |
| `resource0: Permission denied` / mmap refused | lockdown under Secure Boot | use the vfio path in `usplat`/`cmp50-unlock-us` |
| `vfio: no no-IOMMU group holds this device` | `vfio-pci` not bound, or `enable_unsafe_noiommu_mode` off | `modprobe vfio_pci`, write 1 to that parameter, bind vfio-pci |
| `PCI_COMMAND_MASTER` cannot be set (`EPERM`) | pci-sysfs config writes are blocked by lockdown | the tool sets it through the VFIO config region instead |
| Card disappears from `lspci` | a manual PCIe retrain while the link policy was locked | reboot; do not retrain from userspace |
| Gen2 does not come up | the card is behind a PLX switch, or the slot is not set to Gen2 at POST | see [BIOS-SETUP.md](userspace-unlock/BIOS-SETUP.md) |

## Uninstall everything

```sh
sudo userspace-unlock/tools/rollback-patched.sh   # patched modules
sudo userspace-unlock/install.sh --remove         # pre-driver unit + blacklist
sudo reboot
```

Nothing else on the system is modified.
