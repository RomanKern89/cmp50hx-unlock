# CMP 50HX PCIe Gen3 unlock

The CMP 50HX (TU102, `10de:1e09`) ships with its PCIe link fused down to Gen1
(2.5 GT/s): its Link Capabilities 2 advertises only the 2.5 GT/s bit
(`LNKCAP2 = 0x0002`). The silicon itself is the same TU102 as an RTX 2080 Ti,
which runs Gen3 x16, so the limit is policy/fuse, not the PHY.

This project already recovered **Gen2** by writing the card's PCIe policy
registers directly through BAR0 (after the XP3G privilege gate opens), making
the card adopt the unlocked capability set, then retraining the upstream link.
That path is live-proved at Gen2 x4 (host↔GPU bandwidth doubled; see
`docs/CMP50HX.md`). This document covers the **Gen3** extension of the same
mechanism.

## What changed

The unlock is now *generation-adaptive*. It is the same register sequence; only
the two generation selector fields are written with the requested generation
instead of a hard-coded Gen2:

| BAR0 register | offset | field | Gen2 value | Gen3 value |
|---|---|---|---|---|
| `LINK_CONFIG_0` (CFG0) | `0x8c040` | bits [19:18], mask `0x000C0000` | `0x00080000` | `0x000C0000` |
| `PL_LINK_RATE`         | `0x8c1c0` | bits [18:17], mask `0x00060000` | `0x00040000` | `0x00060000` |

Both fields are 2-bit generation selectors where the value equals the PCIe
generation (`2` = Gen2, `3` = Gen3). Everything else in the policy write
(`PRIV_MISC_1` bits, `XP3G_VAL0/OVR0`, `CYA_0` bit 2, the `LTSSM` kick) is
byte-for-byte the proven Gen2 sequence. No new register is written, so the
GA100-only register guard in `build.sh` still passes.

After the card adopts a capability set, the retrain targets **only the
generation the card actually advertises** (`LNKCAP` Max Link Speed, mirrored by
the config-space target): the kernel path computes
`target_gen = min(requested, adopted)` and sets `LNKCTL2` target speed and the
retrain success threshold to `5.0 GT/s` (Gen2) or `8.0 GT/s` (Gen3)
accordingly; the boot service reads the advertised target with `setpci` and
retrains to that. **We never ask the link for a speed the card does not
advertise — that is the anti-brick guarantee.**

## Backward compatibility (the safety fallback)

With the generation set to `2`, the policy writes the exact values the proven
Gen2 unlock wrote (`0x00080000` / `0x00040000`). So:

- **Gen3 is attempted by default.** If the TU102 fuse accepts a Gen3 capability
  the link trains to Gen3; if it clamps to Gen2 you get today's proven Gen2
  result — no worse than before, no brick risk (the retrain only ever targets
  the adopted generation).
- **To force the known-good Gen2 path**, set the generation to `2` (see below).

## How to select the generation

Kernel module (patch `04-cmp50-pcie-gen2.patch`, adds `nvidia` module param):

```
# default is 3 (Gen3); force the proven Gen2 path with:
options nvidia cmp50_target_gen=2
# cmp50_target_gen is a ceiling (2 or 3); the card still trains only as high
# as it adopts. cmp50_gen2_adopt=0 still defers adoption to the boot service.
```

Boot service (`cmp50hx/cmp50hx-gen2.sh`, run via `cmp50hx-gen2.service`):

```
# default is 3; override with the environment variable:
CMP50_TARGET_GEN=2 /usr/local/sbin/cmp50hx-gen2
```

To change it under systemd, add to the unit (or a drop-in):

```
[Service]
Environment=CMP50_TARGET_GEN=2
```

The service/marker name stays `cmp50hx-gen2` and the dmesg/log markers stay
`CMP50_GEN2:` for historical continuity (the install and verify scripts grep
them); the "gen2" in the name is now just an identifier — the mechanism targets
Gen3 by default. The log lines now print the generation reached, e.g.
`CMP50_GEN2: ADOPT_PASS target=Gen3 adopted=Gen3 ...` and
`CMP50_GEN2: RETRAIN_PASS target=Gen3 status=... attempt=...`.

## Live hardware finding (2026-11-13, dual-PLX host)

Measured on a host with two CMP 50HX, each **two PLX PEX8747 hops** from the CPU
(card → PEX8747 → PEX8747 → Intel root port). This is the topology
`userspace-unlock/BIOS-SETUP.md` already flagged, and the live registers explain
exactly why the driver path cannot win here:

- At the GSP `post-booter` phase the card briefly advertises Gen2
  (`LNKCAP2 = 0x00000006`, `PL_LINK_RATE = 0x00140036`, lock bit clear).
- ~100 ms later, at `gsp-ready`, GSP reverts and **locks** it
  (`LNKCAP2 = 0x00000002`, `PL_LINK_RATE = 0x00350036`, **bit 20 set**); the card
  drops back to advertising Gen1 only.
- By the time the driver/boot service runs, the policy reads locked: 50 LTSSM
  kicks leave the registers unchanged (`after_ltssm == before_policy`) →
  `ADOPT_FAIL` on both cards.
- A direct userspace experiment that **clears the lock bit (bit 20) and re-applies
  a Gen3 policy in a tight loop** was overridden every time: bit 20 returned to 1
  within ~50 ms (`PL 0x00350036 → 0x00370036`), `LNKCAP` stayed Gen1, no adoption.
  The card was unharmed (no Xid/AER); the retrain never fired because the
  anti-brick gate held (no adoption → no retrain).

**Conclusion for a board like this:** GSP re-locks the PCIe policy faster than any
OS-level writer can hold it, so **neither Gen2 nor Gen3 is reachable from the
driver or the boot service** — independent of the requested generation. The only
route that can beat the revert is the **pre-OS EFI path** (`efi-unlock/`), which
applies the policy before GSP exists. On a host where the card is CPU-direct, or
where the BIOS trains the slot at Gen2/Gen3 so the policy comes up unlocked (bit
20 clear), the driver path does take — that is where this Gen3 code is expected
to reach Gen3.

## Verifying on hardware

The Gen2 case reproduces the validated register values bit-for-bit; the kernel
addition type-checks and the register math is unit-checked. On a host where the
policy comes up unlocked, confirm Gen3 on the target rig:

1. Build and install the patched modules for the running kernel
   (`build.sh --card cmp50hx` → install → reboot), or run the boot service.
2. In BIOS, set the slot holding the card (and any PLX switch ports the firmware
   exposes) to **Gen3** — the card only advertises the generation it trained its
   own link on at POST.
3. After boot, for each card:
   ```
   sudo python3 verify/gen3_retrain.py <BDF>      # e.g. 0000:04:00.0
   dmesg | grep CMP50_GEN2                          # ADOPT/RETRAIN result + generation
   cat /sys/bus/pci/devices/<BDF>/current_link_speed
   ```
   `gen3_retrain.py` PASSes only when the link reached the card's advertised
   maximum and that maximum is at least Gen2; it fails loudly (instead of a
   silent pass) if `LNKCAP2` still reads `0x0002`, which means the policy was not
   adopted this boot.
4. Measure the effect with the bandwidth probe (`verify/pcie-bench.cu`): Gen3 x16
   should roughly double Gen2's host↔GPU throughput. The compute unlock is
   SM-bound and unaffected by the link generation.

If a card does not reach Gen3, fall back to `cmp50_target_gen=2` /
`CMP50_TARGET_GEN=2`: you keep the proven Gen2 result with no change in risk.

## Scope and honesty

- **Proved in software here:** the Gen2 path is unchanged and byte-identical;
  the Gen3 register encoding is correct and within the field masks; the kernel
  addition compiles/type-checks; no GA100-only register is introduced.
- **Measured on a dual-PLX host:** the driver/boot-service path cannot adopt any
  generation there because GSP re-locks the policy before an OS-level writer can
  hold it (see the live finding above). Gen3 on a physical card is therefore
  still unproven, and proving it needs a host where the policy comes up unlocked
  (CPU-direct or a BIOS-trained slot) or the pre-OS EFI path.
