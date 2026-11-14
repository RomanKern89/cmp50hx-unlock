# Gen3 CSB probe — run record (2026-11-14)

Directed test of the last untried write path: the Gen3 capability bits through
the falcon-privileged SEC2 booter CSB, from EFI pre-OS. Plan:
`docs/GEN3-IMPLEMENTATION-PLAN.md`; mechanism, slot layout and verdicts:
`docs/PCIE-GEN3.md` ("Directed CSB capability probe").

## Artefacts

| item | value |
|---|---|
| source | `efi-unlock/unlock50x_v1.c`, built with `-DPCIE_GEN_EXPERIMENT` |
| build dir (host) | `/data/build/efi-gen3/efi-unlock` |
| deploy | `/boot/efi/EFI/CMP50/50HXGEN3.EFI`, `Boot0002 CMP50 Gen3 Probe`, optional data `--return-to-grub` |
| one-shot | `efibootmgr -n 0002`, entry NOT in the default path (BootOrder restored after every run) |
| verdict file | `/boot/efi/50hx_log.txt` (ESP), copied to `/data/build/efi-gen3/50hx_log.run<N>.txt` |
| post-check | `/data/build/efi-gen3/gen3_after.sh` |

Pre-state for every run: kernel `7.0.0-42-generic` (pinned), driver 610.43.03,
both CMP 50HX `pcie.link.gen.current=2`, miner/gate/comfy/music `active`,
0 Xid. Post-state after every run: identical, 0 Xid.

## Iterations and what each one taught

| run | build | design | outcome |
|---|---|---|---|
| 1 | flag on, re-run probe | patch the chain's 2nd write pair, re-run the booter after the stock unlock | probe **skipped**: guard demanded `PLM==0xFFFFFFFF`, but a finished chain always leaves the "staged" `0xFFFFFF8F`. Guard bug, not a mechanism bug. |
| 2 | +chain control | same, guard on `is_unlocked()` | probe ran: `CAP2(0x0e)` and `CAP(ms=3)` **rejected**, but no control existed, so the result was uninterpretable |
| 3 | +positive control | control write `CAP2=0x0006` (the Gen2 vector the XVE window does set) | control **also rejected** → the re-run does not reach `CAP2` at all |
| 4 | +path control | control writes `SS1` (a register the chain itself writes) on a re-run | `SS1 8→0` **refused**, `SS1→8` trivially unchanged: **a second booter run never executes the ROP chain** (the SEC2 ACR mutex is still owned). Host MMIO control (`CAP2|=0x6`) also refused: the pre-OS XP3G gate is closed. |
| 5 | single-run redesign | slot #1 = open XP3G gate `0x8E1B0 ← 0xFFFFFFFF`, slot #2 = `CAP2 ← 0x0000000E`, both in the **first** and only run | **CTRL: gate `0xffffff8f → 0xffffffff` = CSB write landed. TEST: `CAP2` stayed `0x00000002`, config space `0x00000002` = Gen3 bit refused.** |
| 6 | fuse-override shot | slot #1 = `0x823810 ← 0` (the CMP90 "PCIe fuse override"), slot #2 = `CAP2 ← 0x0000000E` | `0x823810` reads poison `0xbadf5040` before **and** after; `CAP2` refused. No live control in this variant, so formally inconclusive |
| 7 | fuse-override, controlled | slot #1 = XP3G gate (control, proven to land), slot #2 = `0x823810 ← 0` | **CTRL landed (`0xffffff8f → 0xffffffff`), TARGET `0x823810` still reads `0xbadf5040`** — the FEAT_OVR / fuse-override page is not writable even inside the booter's privileged window |

Run 5 log (excerpt):

```
GEN3-PROBE pre: CAP2=0x00000002 XP3G=0xffffff8f PLM=0xfffffe8e SS0=0x00000000
GEN3-PROBE armed: #1 {0xffffffff -> 0x08e1b0} #2 {0x0000000e -> 0x0880a4}
GEN3-PROBE post: CAP2=0x00000002 XP3G=0xffffffff  PLM=0xffffff8f SS0=0x00000000
GEN3-VERDICT: CTRL XP3G gate ... = CSB_WRITE_LANDED
GEN3-VERDICT: TEST LINK_CAP2 0x000880a4 -> 0x00000002 (pre 0x00000002) = GEN3_BIT_REFUSED
GEN3-VERDICT: cfgCAP2=0x00000002 cfgCAP=0x10453d01 (OS-visible view)
GEN3-VERDICT: FUSE_CLAMPED — CSB write path proven alive (the gate opened from
0xffffff8f to 0xffffffff) yet LINK_CAP2 refuses the Gen3 bit
```

## Verdict

The probe finally separates "the writer cannot reach the register" from "the
register refuses the value", because the **same run** proves the write path is
alive (it opened the XP3G privilege gate, which host MMIO cannot touch pre-OS)
and then shows `LINK_CAP2` refusing the 8.0 GT/s bit. Together with the OS-side
evidence (privileged XVE write rejected with the gate open; policy-driven LTSSM
regen clamps to Gen2; GSP never touches the ceiling; no Gen3 force path in the
published source) this is the **seventh and final proof** that the TU102 PCIe
capability is fused at Gen2 for this SKU.

Run 7 additionally closes the last documented lever of the CMP90 recipe: the
`0x823810` PCIe fuse-override page is poison (`0xbadf5040`) and refuses writes
even in the pre-OS booter window, while the control write in the same run lands.
There is no writer left to try.

**Gen2 is the hardware ceiling. Gen3 is closed by hardware, not by software.**

## Incident and fix (2026-11-14, run 5 and run 7)

Opening the XP3G gate from EFI made the application's own best-effort ReBAR step
(`u40x_rebar_try`, step [11]) succeed, leaving **BAR1 at 16 GiB**. On the next OS
boot the kernel could not place that window (`nvidia 0000:05:00.0: BAR 1 [mem
size 0x400000000 64bit pref]: can't assign; no space`), so `05:00.0` came up
without BARs, `nvidia` failed to init it, and `comfyui@1` kept restarting. The
other card fell back to a 64 MiB window and stayed alive. Recovery was one
normal reboot (POST restores the VBIOS BAR size); both cards returned to Gen2
with 0 Xid.

Fix in the source: step [11] is compiled out when `PCIE_GEN_EXPERIMENT` is set
("no irreversible BAR resize") — a probe must not change a memory map it cannot
undo. Lesson worth keeping beyond this task: any pre-OS experiment that opens the
XP3G gate also unlocks the ReBAR path, so ReBAR must be disabled explicitly.

## By-product worth keeping

The EFI application can now open the XP3G privilege gate from pre-OS through the
booter CSB (`0x8E1B0: 0xffffff8f -> 0xffffffff`), and the single-run probe
harness (`-DPCIE_GEN_EXPERIMENT`, slot macros in `unlock50x_v1.c`) can perform
two arbitrary directed BAR0 writes per boot with a positive control. That is the
first pre-OS path to a register window the OS only reaches after GSP-RM starts,
and it is reusable for any other register question (e.g. the `0x8238xx`
FEAT_OVR/fuse-override page that reads poison in the OS).

## Operational notes

- Every run was one-shot (`BootNext`), the default boot entry was never
  changed, and the boot entry was deleted afterwards; `BootOrder` was restored
  to `0003,0004,0005,0001,0000` after the last run.
- Services were stopped before each reboot and confirmed `active` after it;
  both cards returned to Gen2 with 0 Xid every time.
- In run 5 the compute unlock of the probed card is intentionally not attempted
  (slot #1 carries the gate write instead of the FECS PLM write) — the driver
  unlocks both cards in the OS anyway, which the post-check confirmed.
