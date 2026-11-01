# CMP 50HX unlock — full FP32 and INT8 speed on the NVIDIA CMP 50HX (TU102)

[Русская версия ниже](#кратко-по-русски) · [Detailed install guide](INSTALL.md) · [Measured results](userspace-unlock/RESULTS.md)

The CMP 50HX (`10de:1e09`, TU102) ships with a fuse-driven clamp on two
instruction paths: **FP32 FFMA** and **INT8 DP4A** run at roughly 1/32 of what
the silicon can do. This repository restores both, at every boot, on a normal
Linux system — and, for hosts where a driver-side patch cannot be loaded,
contains a **userspace** unlock that performs the same privileged register write
from before the driver binds the card.

Everything here was verified on real hardware: two CMP 50HX with 20 GiB
(memory-modified), Ubuntu 26.04, kernel 7.0.0-42, driver 610.43.03.

## What you get

| Metric | Clamped (stock) | Unlocked | Gain |
| --- | ---: | ---: | ---: |
| FP32 FFMA | 0.40 TFLOP/s | **12.9 TFLOP/s** | **~32×** |
| INT8 DP4A | 1.63 TIOP/s | **52.0 TIOP/s** | **~32×** |

`12.9 TFLOP/s` is 96% of the 13.55 TFLOP/s this part can do, and `52.0 TIOP/s`
is 96% of its 54.2 TIOP/s DP4A rate. The state is
**persistent** — it survives reboots and a full power cycle — and it is confirmed
by NVIDIA's own RM interface, not just by a benchmark:

```
NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER
stock:    v1 [5,5,1,5,5,5,5,5,5]   v2 [5,5,5,5,5,5,5,5]
unlocked: v1 [0,0,0,0,0,0,0,0,0]   v2 [0,0,0,0,0,0,0,0]      (0 = full speed)
```

FP16 half2, INT32, memory bandwidth and PCIe were never clamped, so they do not
change. Workloads limited by the SM's FP32/INT8 rate get the full 32×; LLM token
decode (memory bound) will not.

## Two ways to run it

**1. Patched driver (recommended, needs Secure Boot off or an enrolled MOK).**
The kernel module performs the privileged write inside its own boot, at the point
where nothing overwrites it. This is what produces the numbers above, and it is
what the project's own `install.sh` sets up.

```sh
git clone <this repo> cmp50hx-unlock && cd cmp50hx-unlock
sudo ./install.sh --card cmp50hx      # installs the 610.43.03 userland + patched modules
sudo reboot
cd verify && ./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json
```

**2. Userspace unlock, before the driver binds (Secure Boot can stay on).**
For hosts where loading a locally built module is impossible. `userspace-unlock/`
contains a complete port of the EFI exploit that talks to the card through
`vfio-pci` in no-IOMMU mode, with DMA buffers in hugepages, and writes the same
registers through the SEC2 booter.

```sh
cd userspace-unlock && make
sudo ./install.sh --fb 20g            # systemd unit, runs before nvidia
sudo reboot
```

> **Read this before choosing it.** The userspace unlock is proven at the
> register level (`NV_PFEAT` at `0x409668` goes from clamped to `0x00000000`),
> but the *stock* driver re-applies the fused selectors during GR init, so on its
> own it does not survive into the running system. Use it together with a driver
> that does not re-clamp. The full analysis, including every path that was tried
> and why each is closed, is in
> [userspace-unlock/RESULTS.md](userspace-unlock/RESULTS.md).

## Requirements

* NVIDIA CMP 50HX — `10de:1e09`. Nothing else is touched.
* Linux, kernel headers for the running kernel, `gcc`, `make` (the installer
  fetches what it needs on Ubuntu/Debian).
* The driver-side path needs either **Secure Boot off** (`OS Type = Other OS`) or
  the local signing key enrolled through MokManager.
* **20 GiB cards need `--fb 20g`.** The FB-range register still reports the stock
  10 GiB on memory-modified cards and the booter rejects the layout otherwise.
  Check with `nvidia-smi --query-gpu=memory.total`.

## Documentation map

| Document | What is in it |
| --- | --- |
| [INSTALL.md](INSTALL.md) | step-by-step installation, verification, rollback, troubleshooting |
| [userspace-unlock/README.md](userspace-unlock/README.md) | how the userspace unlock works and why it is built this way |
| [userspace-unlock/RESULTS.md](userspace-unlock/RESULTS.md) | every measurement and every negative result, with the register evidence |
| [userspace-unlock/BIOS-SETUP.md](userspace-unlock/BIOS-SETUP.md) | Secure Boot, serial console and PCIe link speed on an ASUS X99-E WS |
| [userspace-unlock/HANDOVER.md](userspace-unlock/HANDOVER.md) | state of a deployed host, what is proven, what is left |
| [docs/CMP50HX.md](docs/CMP50HX.md) | reverse-engineering notes: fuses, registers, GSP, RT cores |
| [docs/UPSTREAM-README.md](docs/UPSTREAM-README.md) | the original project README (patch-by-patch validation) |
| [efi-unlock/README.md](efi-unlock/README.md) | the pre-OS UEFI unlock and its risks |

## What is not solved here

* **PCIe Gen2.** The card is locked to Gen1 in its own link policy and the
  patch can only adopt Gen2 when the card comes up Gen2-ready at POST. Behind a
  PLX switch (as on the X99-E WS, where every x16 slot sits behind a PEX8747)
  that never happens. Measured, documented, and closed — see
  [userspace-unlock/BIOS-SETUP.md](userspace-unlock/BIOS-SETUP.md). Gen1 x16 is
  4 GB/s and bounds only host↔GPU transfers.
* **ReBAR / RT core count.** Separate patches exist in `patches/` and are
  documented in the upstream README; they are not part of the compute unlock.

## Safety

The unlock writes GPU registers from a signed SEC2 booter. A card that stops
responding mid-run stays that way until a power cycle. It does not flash
anything, does not modify the VBIOS, and is reversed by removing the patched
modules and rebooting ([INSTALL.md](INSTALL.md#rollback)).

## Credits and license

This is a fork of **[xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock)**
— the EFI unlock, the driver patches, the firmware extraction and the
reverse-engineering notes are that project's work. This fork adds the userspace
implementation (`userspace-unlock/`), the measurements above and the deployment
tooling, and keeps the original documentation verbatim in `docs/`.

Licensed under **copyleft-next 0.3.1**, the same license as upstream — see
[LICENSE](LICENSE) and [NOTICE](NOTICE). NVIDIA firmware remains under NVIDIA's
own terms.

---

## Кратко по-русски

Репозиторий снимает ограничитель **FP32 и INT8 (DP4A)** с NVIDIA CMP 50HX
(`10de:1e09`): было 0.40 TFLOP/s, стало **12.9 TFLOP/s** (≈32×), состояние
переживает перезагрузку и обесточивание, подтверждается интерфейсом самого
драйвера (`SM_ISSUE_RATE_MODIFIER` = все нули).

* Основной путь — патченный драйвер `610.43.03`: `sudo ./install.sh --card cmp50hx`, затем перезагрузка. Нужен выключенный Secure Boot либо зачисленный MOK-ключ.
* Альтернатива для хостов, где модуль загрузить нельзя — `userspace-unlock/`: анлок из userspace до загрузки драйвера (vfio + hugepages). Механизм доказан, но стоковый драйвер перезаписывает селекторы — подробности и все отрицательные результаты в `userspace-unlock/RESULTS.md`.
* Для карт на 20 ГБ обязателен `--fb 20g`.
* PCIe Gen2 на платах с PLX-коммутатором недостижим — это ограничение платы, на вычисления не влияет.

Подробная инструкция: [INSTALL.md](INSTALL.md).
