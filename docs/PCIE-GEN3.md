# CMP 50HX PCIe link unlock (Gen2 behind PLX; Gen3 status)

The CMP 50HX (TU102, `10de:1e09`) ships with its PCIe link fused to Gen1
(2.5 GT/s): its Link Capabilities 2 advertises only the 2.5 GT/s bit
(`LNKCAP2 = 0x0002`). The silicon is the same TU102 as an RTX 2080 Ti (Gen3 x16),
so Gen1 is a policy/fuse limit, not the PHY.

The driver already recovered the Gen2 **capability** by writing the card's PCIe
policy registers through BAR0 after the XP3G privilege gate opens, making the
card adopt the unlocked set (`s_cmp50ApplyGen2Policy`, patch 01). On a
CPU-direct slot that was enough. **Behind a PLX switch it was not**, and this
document records how that was fixed and where Gen3 stands.

## The PLX problem, and the fix (Gen2 now works behind PLX)

On a board where the card sits behind one or more PLX PEX8747 switches (e.g. the
ASUS X99-E WS, every x16 slot behind a switch), the card trains **Gen1** at POST
because at POST it still advertises only Gen1. The sequence seen in `dmesg`:

- `post-booter` (before GSP-RM boots): the policy write + LTSSM kick makes the
  card adopt Gen2 — `LNKCAP = …453d02`, `LNKCAP2 = 0x0006`.
- `gsp-ready` (after GSP-RM boots): on a link that is still **physically Gen1**,
  GSP reverts the capability back to Gen1 and locks it. Empirically GSP does
  **not** revert a link that is already **trained** at Gen2 ("it does not fight
  an already-unlocked block").

So the capability was won at `post-booter` but lost before the OS could use it,
and every OS-stage retrain afterwards hit an already-locked block → `ADOPT_FAIL`.

**The fix** (`s_cmp50RetrainUpstreamGen2`, patch 01): immediately after the
`post-booter` adoption, and **before GSP-RM boots**, retrain the card's immediate
upstream port (the PLX downstream port) to Gen2 — set its Link Control 2 target
to Gen2 and fire Retrain Link. The card↔PLX link then comes up physically
Gen2-trained, so when GSP boots it sees a trained Gen2 block and leaves it alone.
Gen2 survives into the OS. The retrain only fires when the card actually adopted
Gen2+ (`LNKCAP` SLS ≥ 2), so it never asks for a speed the card does not
advertise — the anti-brick guarantee.

The port is reached from the RM core with its own PCI accessors:
`pGpu->gpuClData.upstreamPort` + `osPciReadDword`/`osPciWriteDword` at
`PCIECapPtr + 0x30` (Link Control 2) and `+ 0x10` (Link Control, Retrain = bit 5).

### Hardware result — 2026-11-13, ASUS X99-E WS, 2× CMP 50HX behind dual PLX PEX8747

Both cards reached **Gen2** and held it:

- `UPSTREAM_RETRAIN phase=post-booter … cur_speed=2` — the card↔PLX link trained Gen2.
- `gsp-ready: ALREADY_GEN2 CAP=…453d02 (not re-kicking)` — GSP did not revert.
- `current_link_speed = 5.0 GT/s`, `nvidia-smi` `pcie.link.gen.current = 2`,
  `LNKCAP2 = 0x0006`, both cards; no Xid, no AER; stable (not flapping).
- Host↔GPU bandwidth roughly doubles vs Gen1 (the compute unlock is SM-bound and
  unaffected by the link generation).

This is the first time Gen2 held on this dual-PLX board, where it was previously
closed. No BIOS slot change was needed.

## Gen3 status (not yet achieved; under investigation)

The same code path was tried with a Gen3 request (policy fields set to `3`):

- The policy **was accepted**: `LINK_CONFIG_0` field = 3 (`CFG=…c5800`) and
  `PL_LINK_RATE` field = 3 (`PL=…16…`) both took.
- But the card **adopted only Gen2**: `LNKCAP = …453d02` (SLS = 2),
  `LNKCAP2 = 0x0006` — not the Gen3 vector `0x000e`. The retrain then correctly
  fell back to the adopted generation (Gen2); no regression, no Xid.

So in the path tested, the TU102 **clamps the regenerated `LNKCAP2` to Gen2**
even when the policy requests Gen3.

### Why: the Gen3 capability is fuse-clamped (direct-write test, 2026-11-13)

A read/write probe through the **XP3G-privileged XVE BAR0 window** — the same
window that raised the card from Gen1 to Gen2 — shows the Gen3 capability bits
are rejected by hardware:

| write (gate open, in the OS) | readback | result |
|---|---|---|
| `LINK_CAP2` (`0x880a4`) `\|= 0x8` (8.0 GT/s bit) | `0x00000006` | **rejected** (bit stays clear) |
| `LINK_CAP` (`0x88084`) max-speed field `= 3` | max-speed `= 2` | **rejected** (clamped to Gen2) |

The same window writes the Gen2 fields fine (that is how Gen2 is reached), so
this is not a window/privilege problem — the advertised max is **fuse-clamped at
Gen2** on this SKU. Two independent methods agree: the direct privileged write is
rejected, and the policy-driven LTSSM regen also clamps to Gen2. The GA100
`OPT_GEN23`/`OPT_GEN3` registers are unmapped on TU102 (read `0`), and the
pre-OS EFI path cannot help because the XP3G gate is closed before the OS.

**Conclusion:** Gen3 is not reachable by any validated path. The TU102 die
supports Gen3 (it is the RTX 2080 Ti chip), but NVIDIA fused this CMP 50HX SKU's
PCIe link capability to Gen2, and that clamp resists every write path available,
including the privileged one that unlocks Gen2. **Gen2 is the hardware ceiling.**

### No software override exists on TU102 (source audit, 2026-11-14)

A pass over the published `610.43.03` `open-gpu-kernel-modules` source confirms the
clamp sits below any software-reachable register on Turing:

- The driver's only PCIe-generation *force* knob is **Gen2** (`RmForceEnableGen2` →
  `PDB_PROP_CL_PCIE_FORCE_GEN2_ENABLE`, `chipset.c`). There is **no**
  `ForceEnableGen3` / `FORCE_GEN3` anywhere in the tree — NVIDIA's own RM never
  forces Gen3 on this family.
- There is **no `dev_nv_xp.h` for `turing/tu102`** at all. The XP/PL block — where
  `LINK_CONFIG_0 0x8c040`, `PL_LINK_RATE 0x8c1c0`, the CYA controls and the
  supported-speeds logic live — is published for Pascal/Maxwell/NVSwitch, but the
  Turing XP3G block is closed. The ceiling is enforced in an unpublished,
  PLM-locked hardware block.
- No `SUPPORTED_LINK_SPEEDS_VECTOR`, `OPT_PCIE_GEN`, or PCIe-gen fuse-override
  register is defined for **any** chip in the published regmaps. The Ampere
  `OPT_GEN23`/`OPT_GEN3` names used by the EFI experiment came from a community
  CMP 170HX/90HX patch, not NVIDIA headers, and read `0`/poison on TU102.
- A prior full IDA pass over `gsp_tu10x_610.43.03.elf` already found the GSP PCIe
  policy routine `RMPcieLinkSpeed @0x4cb25b8` writes only those policy registers
  and **never writes Link Cap/Cap2 nor reads a PCIe-gen fuse** — re-disassembling
  the GSP cannot locate a fuse the firmware does not read.

Five independent lines now agree the TU102 PCIe link is fused to Gen2 below the
software layer: (1) the direct privileged XVE write of the Gen3 capability bits is
rejected; (2) the policy-driven LTSSM regen clamps the regenerated `LNKCAP2` to
Gen2; (3) the GSP firmware does not touch the ceiling; (4) the published source
exposes no Gen3 force path and no supported-speeds / fuse-override register on
Turing; (5) the driver's own maximum force is Gen2.

The one write not yet tried is the same `LINK_CAP`/`LINK_CAP2` write routed through
the **falcon-privileged SEC2 booter CSB** (the primitive that lands the SM-speed
override, `efi-unlock/unlock50x_v1.c` `own_code_67`) instead of the MMIO window.
It is a *directed* test — known target (`0x88084`/`0x880a4`), known value (Gen3
bits) — but low odds: `LINK_CAP2` is a capability register driven combinationally
from the supported-speeds vector, so a falcon-priv write to its alias is unlikely
to differ from the already-rejected MMIO write, and no writable latch for it exists
in any regmap. **Gen2 remains the hardware ceiling; Gen3 is not reachable by any
validated or principled path.**

The generation request defaults to Gen2.

## How to select the generation

Boot service (`cmp50hx/cmp50hx-gen2.sh`, run by `cmp50hx-gen2.service`) and the
kernel module (`cmp50_target_gen`) both accept a generation; the card is trained
only as high as it actually adopts (so 3 is a ceiling, not a force). The service
and `CMP50_GEN2:` dmesg markers keep their historical names (install/verify
scripts grep them).

## Verifying on hardware

```
sudo python3 verify/gen3_retrain.py <BDF>        # e.g. 0000:05:00.0
dmesg | grep CMP50_GEN2                            # ADOPT / UPSTREAM_RETRAIN / RETRAIN_PASS
cat /sys/bus/pci/devices/<BDF>/current_link_speed  # 5.0 GT/s = Gen2
nvidia-smi --query-gpu=pci.bus_id,pcie.link.gen.current --format=csv
```

`gen3_retrain.py` passes only when the link reached the card's advertised maximum
and that is at least Gen2; it fails loudly if `LNKCAP2` still reads `0x0002`
(policy not adopted this boot).

See `GEN2-INSTALL.md` for the two-step install.
