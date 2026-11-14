# NVLink on CMP 50HX — investigation and fuse-shadow probe (2026-11-14)

Question: is NVLink on the CMP 50HX blocked by software (driver, GSP policy, regkey) that a
patch could lift, or by the silicon itself?

Answer: **by the silicon.** The NVLink unit (IOCTRL) is not enumerated by the chip, the
mechanism matches the fused-off NVDEC, and the engine-disable fuse shadow refuses a write
even from the privileged booter window that does land other writes in the same run.
No driver or firmware patch available to the owner can enable NVLink on this SKU.
Multi-GPU traffic stays on PCIe P2P (patches 06/07, Gen2 ceiling).

## Host and cards

lmserver 192.168.1.215, kernel 7.0.0-43, open driver 610.43.03 (GSP), 2x CMP 50HX
(`10de:1e09`, subsystem `10de:1554`, VBIOS 90.02.60.00.1A, board PG150 SKU 100) at
`05:00.0` and `0a:00.0`.

## Evidence (read-only, live, both cards)

| Check | Result |
|---|---|
| `nvidia-smi nvlink -s/-c/-e/-R` | `Device does not have or support Nvlink` |
| NVML `nvmlDeviceGetNvLinkState`, links 0–5 | `NOT_SUPPORTED`; `FI_DEV_NVLINK_LINK_COUNT` `NOT_SUPPORTED` |
| `/proc/driver/nvidia/params` | `NvLinkDisable: 0`, `RegistryDwords: ""` — no regkey disables it |
| `nvidia-smi topo -m` / `-p2p n` / `-p2p r` | `PHB` / `NS` / `OK` (PCIe P2P works, no NVLink path) |
| PTOP device info (BAR0 `0x22700`, 64 words, Turing format) | only GR `0x00`, 5× LCE `0x13`, SEC2 `0x0d`, GSP `0x14`; **no IOCTRL `0x12`**, no NVDEC `0x10` |
| `NV_FUSE_OPT_NVDEC_DISABLE` (`0x21378`, `tu102/dev_fuse.h`) | `0x7` — all NVDEC fused; NVDEC is absent from PTOP exactly like IOCTRL |
| Positive control: RTX 3090 (NVLink IP present, no bridge), driver 595.99.02 | `Unable to retrieve Nvlink information as all links are inActive` — a different state |

Notes:
- NVLink on Volta/Turing is an on-die IOCTRL/MINION block; it never appears in PCI config
  space, so `lspci` cannot prove or disprove it (an earlier note relied on that — withdrawn).
- In the open RM, CPU-RM carries no physical NVLink code (`RMCFG_MODULE_NVLINK=0`); presence
  is decided by GSP-RM from IOCTRL discovery (`kernel_nvlinkstate.c` `knvlinkIsPresent_IMPL`,
  `kernel_nvlink.c` `knvlinkCopyNvlinkDeviceInfo_IMPL`). TU102 is NVLink-capable in the HAL
  (`g_kernel_nvlink_nvoc.c`); nothing in the tree is CMP- or `1e09`-specific for NVLink.
- The board is the RTX 2080 Ti FE PCB family; the owner reports the edge connector is present.
  Whether the lane components are populated was not checked.

## Fuse-shadow probe (pre-OS, one-shot)

Same harness as `GEN3-PROBE-RUN-2026-11-14.md` (`-DPCIE_GEN_EXPERIMENT`), two directed
writes in the single SEC2 booter run, extended with `fuse_ptop_status()` (fuse + PTOP dump)
and a `FUSE-VERDICT` line in `efi-unlock/unlock50x_v1.c`.

| item | value |
|---|---|
| build | `/data/build/efi-fuse/efi-unlock`, `EXTRA_CFLAGS='-DGEN3_SLOT2_ADDR=0x21378u -DGEN3_SLOT2_VALUE=0u'`, EFI md5 `fea81499644239e11326d86f9c241cb1` |
| build checks | preprocessor: slot #2 = `0x21378/0` with flags, stock `LINK_CAP2/0xE` without (negative control); object code `movl $0x21378,0xf9a4(%rbx)` (slot #2 address word); ReBAR step compiled out |
| deploy | `\EFI\CMP50\50HXFUSE.EFI`, `Boot0008` + `--return-to-grub`, `BootNext` only, `BootOrder` untouched |
| run | reboot 19:48:18 UTC, up 19:49:31 UTC; services stopped before, `active` after |

Log (`/data/build/efi-fuse/50hx_log.fuse.txt`, card `05:00.0`):

```
FUSE-PROBE pre:  NVDEC_DISABLE(0x21378)=0x00000007 PTOP engines=8 typemask=0x00182001 NVDEC=0 IOCTRL=0
GEN3-PROBE armed: #1 {0xffffffff -> 0x08e1b0} #2 {0x00000000 -> 0x021378}
FUSE-PROBE post: NVDEC_DISABLE(0x21378)=0x00000007 PTOP engines=8 typemask=0x00182001 NVDEC=0 IOCTRL=0
GEN3-VERDICT: CTRL 0x08e1b0 -> 0xffffffff (pre 0xffffff8f, want 0xffffffff) = CSB_WRITE_LANDED
GEN3-VERDICT: TEST 0x021378 -> 0x00000007 (pre 0x00000007, want 0x00000000) = REFUSED
FUSE-VERDICT: control path live; fuse shadow 0x021378 REFUSED the write (FUSE_SHADOW_LOCKED)
```

Independent OS-side re-read after boot: both cards `0x21378 = 0x7`, PTOP without `0x12`.
Health after the run: both cards Gen2, 0 Xid, all six services `active`.

Cleanup: `Boot0008` deleted, EFI moved to `/data/build/efi-fuse/esp-archive/`,
`/boot/efi/EFI/CMP50` removed; `BootOrder` = `0003,0004,0005,0001,0000` (the saved order
minus the BBS/CD entries the firmware itself dropped on that boot).

## Verdict

The privileged write path is alive in the same run (control landed) and the engine-disable
fuse shadow refuses the write; PTOP does not change. Together with run 7 of the Gen3 probe
(fuse-override page `0x823810` refuses writes), the owner's only privileged writer cannot
change fuse state. NVLink's IOCTRL is removed by the same class of fuse as NVDEC, so it
cannot be enabled by a driver patch, a regkey, or this pre-OS path.

Not verified: the exact NVLink fuse offset (not in any open header; inferred to share the
OPT window's behaviour), the board's NVLink lane components, and a VBIOS cross-flash route
(not researched).

## VBIOS cross-flash question (2026-11-14, evening)

Could a 2080 Ti (or other TU102) VBIOS bring NVLink back? Researched by image comparison and
a public-source sweep (English, Russian, Chinese), every cited source re-checked by an
independent fetch.

**Image comparison.** The live VBIOS was read through the BAR0 PROM window (read-only; both
cards byte-identical, md5 `a75051832e407633062ce210dc8ef701`, PCIR `10de:1e09`, 90.02.60.00.1A,
board string `1000G1500100` = PG150 SKU 100, empty DCB). References from the TechPowerUp
collection: RTX 2080 Ti FE 90.02.17.00.04 (md5 `77d59a02…`) and 90.02.30.00.05
(md5 `48dd27db…`), PCIR `10de:1e07`, PG150 SKU 30.

| | CMP 50HX | 2080 Ti FE 2018 | 2080 Ti FE 2019 |
|---|---|---|---|
| BIT `I` (NVINIT_PTRS) idx 14 "NVLink Configuration Data" | `0x7617` | `0x7436` | `0x7477` |
| 21-byte block | `01 15 00…00 ff 00 00 00 00` | identical | identical |
| LowPower NVLINK table (PERF_PTRS idx 39, 126 B) | — | identical | identical |

The 2080 Ti image carries no NVLink configuration the CMP image lacks. In the driver the VBIOS
NVLink data can only remove links (`discoveredLinks &= ~vbiosDisabledLinkMask`,
`kernel_nvlinkstate.c` `_knvlinkFilterDiscoveredLinks`), and it is applied after the presence
check that already fails on the missing IOCTRL.

**Public evidence (verified quotes).** No report exists of a 2080 Ti VBIOS running on a CMP 50HX.
Every analogous attempt failed: Turing nvflash refuses device-ID mismatch (TechPowerUp,
PCGamesHardware, nvflashk README); CMP 40HX + RTX 2070 image refused by both bypass flashers
("chipid mismatch", Win-Raid 2024); an EEPROM-programmer flash of a different-device-ID
Turing image left the card dead (igor'sLAB 2018); CMP 90HX foreign image → Code 43
(NVIDIA-patcher #45). No source reports any fused unit (NVLink, NVENC/NVDEC, display, SMs,
PCIe Gen3) restored by a VBIOS on any CMP/Turing card. The only public NVLink revival on a CMP
(170HX, GA100) needed voltage fault injection on the fuses plus board rework, not a VBIOS.

**Conclusion.** A VBIOS swap cannot enable NVLink on the CMP 50HX: the image is bound to
`1e09`, the reference image's NVLink data is identical, and the unit is removed by fuses before
the VBIOS matters. Expected outcome of forcing it: refusal, or a non-booting card that needs an
SPI reflash of the backup.
## PTOP writability — the last software lever (2026-11-14, evening)

The fuse shadow test above used `NV_FUSE_OPT_NVDEC_DISABLE` as a proxy: the real
NVLink fuse offset is not published (the TU102 fuse header exposes exactly two
OPT registers, `SECURE_GSP_DEBUG_DIS` and `NVDEC_DISABLE`). So the question was
narrowed to the mechanism that *decides* engine presence in the first place: the
PTOP device-info table, which is what RM/GSP read when they report "no IOCTRL".

Published headers disagree across chips, which is why this had to be measured on
our silicon rather than inferred:

| chip | `NV_PTOP_DEVICE_INFO2(i)` | `NV_PTOP_DEVICE_INFO_CFG` |
|---|---|---|
| GA100 (Ampere) | `0x22800 + i*4` — `R--4A` (read-only) | `0x224FC` — `R--4R` |
| GB100 (Blackwell) | `0x22800 + i*4` — `RW-4A` (read-write) | `0x224FC` — `RW-4R` |
| TU102 (Turing) | not published | not published |

Two independent writers were tested in **one** boot with the proven harness
(`-DPCIE_GEN_EXPERIMENT`, two CSB writes per run):

| writer | target | result |
|---|---|---|
| host MMIO | unused row `0x22748 <- 0xDEADBEEF` | readback `0x00000000` — **READ_ONLY** |
| host MMIO | `NV_PTOP_DEVICE_INFO_CFG 0x224FC` | reads **poison `0xbadf5040`**, write refused |
| CSB (falcon-privileged) | `0x22748 <- 0x00000001` | control first: XP3G `0xffffff8f -> 0xffffffff` **LANDED**; row readback `0x00000000` — **REFUSED** |
| CSB | fuse shadow `0x21378 <- 0` | third independent refusal (previously 2026-11-14 19:48) |

Full table dump taken in the same run (32 rows, both cards identical in shape):
`0x8006183e, 0x80000003, 0x00400205, 0xac028c3e, 0x8000004f, 0x0010407d,
0xb0030e3e, 0x8000004f, 0x04104085, 0xa103aa3e, 0x8000004f, 0x0810408d,
0xa5252c3e, 0x8000004f, 0x0c104095, 0xa945ae3e, 0x8000004f, 0x1010409d,
then zeros` — 8 entries, `typemask=0x00182001`, no `0x10` (NVDEC) and no `0x12`
(IOCTRL).

**Result.** The engine table on TU102 is ROM for every writer available to the
owner: the host PCIe path *and* the falcon-privileged CSB path that does land
writes in the same run. Engine presence — including NVLink's IOCTRL — is
decided by fuse/strap state below any software-visible register. A missing unit
cannot be re-added by faking the table, and a driver patch could at best lie to
the consumer while the hardware still reports no IOCTRL and no trainable link.

### What is left, and why it is not a software unlock

1. **eFuse state** — the only place IOCTRL presence is actually decided. Not
   published for TU102; not writable through any shadow exposed to software
   (three refusals above). The only public precedent for reviving NVLink on a CMP
   part is the 170HX (GA100), and it needed **voltage fault injection on the
   fuse** plus board rework — a lab attack with brick risk, not a patch.
2. **Board** — the owner reports the NVLink edge connector is present on this
   PG150 board (consistent with it being the 2080 Ti PCB family); whether the
   lane components are populated was never checked physically. Even if they are,
   without IOCTRL there is nothing to train the links.
3. **VBIOS** — closed above: the image's NVLink data is identical to the 2080 Ti
   reference and the part is device-ID locked.

Practical alternative for multi-GPU traffic remains PCIe P2P, whose measured
state is in `docs/P2P.md` (currently slower than host staging until the
documented ACS/IOMMU recipe is applied).

### Artefacts for this run

| item | value |
|---|---|
| build | `/data/build/efi-gen3/efi-unlock`, `EXTRA_CFLAGS='-DGEN3_SLOT2_ADDR=0x22748u -DGEN3_SLOT2_VALUE=1u'`, sha256 `e6218823…` |
| deploy | `\EFI\CMP50\50HXNVL.EFI`, `Boot0002` + `--return-to-grub`, `BootNext` only |
| log | `/data/build/efi-gen3/50hx_log.nvlink-ptop.txt`, sha256 `bd63c948…` |
| health after | both cards Gen2, 0 Xid, 7/7 services `active`, `BootOrder` restored, entry deleted |
