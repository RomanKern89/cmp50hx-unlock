# Results: userspace unlock on the two 20 GiB CMP 50HX (test host)

## Final result (Secure Boot off + patched driver)

| | |
| --- | --- |
| firmware | Secure Boot **disabled** in setup (`OS Type = Other OS`), `lockdown=none`, `sig_enforce=N` |
| driver | the project's patched **610.43.03**, loaded from `/lib/modules/<kver>/updates/`, userland 610.43.03 |
| RM acceptance (`verify/rm_issue_rate`) | `v1 = [0,0,0,0,0,0,0,0,0]`, `v2 = [0,…,0]` — all nine issue-rate fields at full speed |
| FP32 FFMA | **12.92–12.96 TFLOP/s** (96% of the 13.55 TFLOP/s the part can do) |
| INT8 DP4A | **51.8 TIOP/s** |
| 30 s sustained | 12.86 TFLOP/s, 55 °C, 95 W, no new Xid |
| project reference | unlocked 13.512 TFLOP/s / 47.984 TIOP/s, stock 0.427 / 1.689 → about **30×** |

The pre-driver userspace unlock (`cmp50-unlock-us`) is not used in this final
state and its service stays disabled: with Secure Boot off the patched driver
does the privileged write itself, at the point in its own boot where nothing
overwrites it.

## Reinstall and re-verification (fresh Ubuntu 26.04.1, 2026-11-01)

The machine was reinstalled from scratch and the unlock rebuilt from this
repository with `sudo ./install.sh --card cmp50hx` (commit `67b13c5`). One
CMP 50HX (20480 MiB) was present; a GT 220 drives the console.

| | |
| --- | --- |
| OS / kernel | Ubuntu 26.04.1 LTS, `7.0.0-42-generic` |
| firmware | Secure Boot **disabled**, `lockdown=[none]`, `sig_enforce=N` |
| driver | patched 610.43.03 from `/lib/modules/7.0.0-42-generic/updates/`, unsigned; userland 610.43.03 |
| RM acceptance | `v1 = [0,0,0,0,0,0,0,0,0]`, two runs of `v2` = all zeros; 3584 / 56 / 448 cores |
| FP32 FFMA | **12.85–12.97 TFLOP/s** (peak 13.55, i.e. 95–96%) |
| INT8 DP4A | **52.0 TIOP/s** (peak 54.2, i.e. 96%) |
| 30 s sustained | 47–50 °C, 82–96 W, SM 1890 MHz, mem 7000 MHz, 0 Xid, throttle reasons 0 |
| initramfs | rebuilt, module `srcversion` verified against `updates/` |
| BAR1 | 64 MiB — the ReBAR request cannot be placed (see below) |
| PCIe | `ADOPT_FAIL ret=0 cap=10453d01 tries=50 (locked set still in effect)` — Gen2 still closed, as documented |

Two tooling defects this run exposed and fixed:

* `cmp50-dp4a` reported **812 TIOP/s**, 16x above the part's peak. The kernel
  kept 16 accumulator chains but stored only the first, so ptxas deleted the
  other fifteen and the run charged for 16x the work it performed. Folding
  every chain into the stored value gives 52.0 TIOP/s, stable at 20k, 200k and
  2M iterations (0.0516 s / 0.5158 s / 5.1892 s — the runtime scales linearly,
  so the loop is real).
* `verify.py` exited non-zero on `CMP_SKU is 0` although all nine issue-rate
  fields read full speed. The patched driver clears the SKU selector by design,
  so 0 is now a note and any other value still fails.

ReBAR: this board enumerates all PCI memory below 4 GiB (the root port window
is 96 MiB and everything sits in `0xd2..0xfb`), so a 16 GiB BAR1 has nowhere to
go and BAR1 stays 64 MiB. Enabling `Above 4G Decoding` in firmware is what the
resize would need; the compute unlock does not depend on it.

## Re-verification on both cards (2026-11-05)

The run above had one card installed. With two cards, every check was repeated
per card on the same driver build (loaded module `srcversion` matches the module
built by `install.sh`, patch set before #74). Power limit 160 W (default 225 W).

| | GPU 0 | GPU 1 |
| --- | ---: | ---: |
| RM acceptance (`rm_issue_rate` + `verify.py`) | PASS | PASS |
| FP32 FFMA | **12.92 TFLOP/s** | **12.84 TFLOP/s** |
| INT8 DP4A | **54.48 TIOP/s** | **54.33 TIOP/s** |
| FP16 tensor cores, FP32 / FP16 accumulate (`verify/hgemm-bench.cu`) | 51.6 / 50.5 TFLOP/s | 50.0 / 48.5 TFLOP/s |
| same at the default 225 W limit | 76.4 / 80.6 TFLOP/s | 75.1 / 79.4 TFLOP/s |
| device memory (D2D copy, read + write) | 499 GB/s | 499 GB/s |
| Xid | 0 | 0 |

Everything below records how that conclusion was reached: what the userspace
port proves, why it alone is not enough with the stock driver, and which paths
were tried and closed on the way.

What was measured on the real hardware, what it proves, and where it stops.
Everything below was produced on 2026-11-01 on Ubuntu 26.04 / kernel
`7.0.0-42-generic` / Secure Boot on (`lockdown=integrity`, `sig_enforce=Y`) /
nvidia-open `615.71.09`, two CMP 50HX `10de:1e09`, 20480 MiB each,
VBIOS `90.02.60.00.1A`, PCIe Gen1 x16.

## 1. The unlock itself works, and the readout register proves it

`NV_PFEAT` at `0x00409668` is the *readout* of the nine applied SM issue-rate
fields (`IMLA0, FMLA16, DP, FMLA32, FFMA, IMLA1..IMLA4`); **0 means full speed**,
anything else is a reduction. Reading it through the BAR0 mapping that is alive
for the whole run:

```
entry:  RATE=0x016db6ed        <- clamped
booter: RATE=0x00000000        <- all nine fields at full speed
```

This is the first unambiguous proof in this project that the userspace port
performs the same state change as the EFI unlock: the privileged SEC2 chain
lands the override words, the hardware applies them, and every issue-rate field
reads 0.

`PLM` (`0x00409650`) stays at `0xffffff8f`. That is *not* a failure: it is the
documented "staged" value (`unlock50x_v1.c` calls it `VAL_PLM_STAGED`,
"SS0 still gated"), and it is what the EFI produces as well. Judging the unlock
by `PLM == 0xffffffff` is wrong; `RATE` is the value that matters.

The values also survive `close(vfio fd)` and 12 s of polling while the BAR0
mapping is kept — so the write is not transient.

## 2. The driver puts the clamp back

`verify/rm_issue_rate` (the project's own RM probe) reports, **after** a
successful boot-time unlock and after the driver bound the cards:

```
v1 [5, 5, 1, 5, 5, 5, 5, 5, 5]      v2 [5, 5, 5, 5, 5, 5, 5, 5]
```

Identical to a stock boot. `verify.py` therefore fails, and FP32/DP4A
throughput is unchanged (~6.4 TFLOP/s FFMA, ~26 TIOP/s DP4A — the FFMA number is
~48 % of the 13.4 TFLOP/s peak at the observed 1875 MHz and ~112 W, i.e. not
power-limited).

The sequence that produces this: the unlock is complete (`RATE=0`) when the tool
releases the card, and the nvidia GR/GSP initialisation programs the selectors
from the fuses again, restoring the clamped pattern before any user work runs.
So a pre-driver write cannot survive this driver.

## 3. What was tried to get past that, and why each path is closed

| Attempt | Result |
| --- | --- |
| Unlock at boot, before nvidia's first probe (`rd.driver.blacklist=` + `cmp50-unlock.service`) | Service succeeds, both cards "UNLOCKED", handed to nvidia — RM still reports the clamped values |
| Keep the card in D0 across the handoff (`power/control=on`, `vfio_pci.disable_idle_d3=Y`) | No change |
| Skip the FLR (`--no-flr`) | No change; the FLR is not what loses the state |
| Real GSP `.fwimage` in the radix pages instead of the zero dummy | Booter still succeeds identically; no change |
| Driver registry override `NVreg_RegistryDwords="RMOverrideSmSpeedSelect=0x88888888;RMOverrideSmSpeedSelect1=0x00000008"`, first on 615-open | Key is accepted (`/proc/driver/nvidia/params` shows it) but every RM allocation then fails with `0x1b`. The open 615 module does not contain the string `RMOverrideSmSpeedSelect` at all — only `subdeviceCtrlCmdKGrGetSmIssueRateModifier*`, i.e. the getter. The mechanism does not exist in that build. |
| Same registry override on the **proprietary, Canonical-signed 580** (its object files *do* contain `RMOverrideSmSpeedSelect`) | Driver loads (`580.178.04`, Canonical), key is live in `/proc/driver/nvidia/params`, cards enumerate — and the nine modifiers still read `[5,5,1,5,5,5,5,5,5]`. The documented-but-unverified registry branch is inert on this card, exactly as `docs/CMP50HX.md` hedged. |
| Switch to the project's validated driver generation | **`linux-modules-nvidia-610-7.0.0-42-generic` does not exist** in the Ubuntu archive; for kernel 7.0.0-38 the objects exist but the signatures package is published as `+2` while the module package pins `+1`, so no linked signed module can be produced |

What is left is the project's own mechanism for exactly this situation:
`patches/cmp50hx/01-cmp50-stockflow.patch` performs the same privileged write
*inside* the driver, right before GSP-RM starts, so the driver's own state and
the hardware agree and nothing re-latches afterwards. It requires a rebuilt,
unsigned kernel module, which this host cannot load: Secure Boot is on with
`sig_enforce=Y` and enrolling a MOK needs a keyboard at the next boot.

## 4. Consequences for the deployment

`install.sh` (systemd unit + blacklist, run before nvidia probes) is correct and
was executed end-to-end several times: two cards, ~5 s each, `mbox0=0`,
`RATE=0`, handoff to nvidia, no Xid. It is left **installed and disabled**,
because on this host it does not change what the driver ends up doing:

```sh
sudo ./install.sh --fb 20g      # enable the boot-time unlock
sudo ./install.sh --remove      # remove it
```

The value it has today is that the hardware path is proven and repeatable: if
the driver is ever replaced by one that does not re-apply the fused selectors
(a 610 build, or the patched module with Secure Boot off), the deployment above
is the piece that needs no further work.

## 5. Reproduce

```sh
# the unlock, with the readout register in the log
sudo ./cmp50-unlock-us --blobs ../efi-unlock/blobs --fb 20g --no-flr --hold 4
# ground truth, after the driver is up
cd ../verify && ./rm_issue_rate 0 | python3 verify.py /dev/stdin   # expects all zeros
```

Watch `RATE=0x00000000` in the unlock log: that is the hardware accepting the
override. Watch `rm_issue_rate`: that is what the working system actually uses.
The gap between the two is the driver re-latch described above.

## 5a. Two operational hazards found while getting there

- **A locally signed module in `updates/` is a boot hazard.** It shadows the
  signed distro module in `kernel/` *and* is copied into the initramfs, so the
  next boot comes up with no GPU driver at all — `nvidia-smi` gone, though SSH
  still works. Keep such modules out of the search path until their key is
  enrolled; `tools/rollback-patched.sh` moves them aside and `--check` reports
  which module would load.
- **The `.run` replaces the userland.** After it installs 610.43.03 libraries,
  putting the 615 kernel module back yields `Driver/library version mismatch`:
  the cards work, NVML/CUDA do not. `finish-mok.sh` compares the module and NVML
  versions and refuses to reboot while they disagree.
- **DKMS must not build these modules.** `nvidia-dkms-580`/`615` sign with the
  local MOK key, which is not enrolled, and land in `updates/` where they win
  over the signed distro build.

## 6. The one remaining path: the driver-side patch with an enrolled MOK

Everything that needs no console has been tried (section 3). The mechanism that
survives is the project's own `patches/cmp50hx/01-cmp50-stockflow.patch`: it
performs the same privileged write from inside the driver, before GSP-RM starts,
so nothing re-applies the fuses afterwards.

It needs a locally built module, which Secure Boot accepts only if the signing
key is enrolled. On the test host the key pair already existed —
`/var/lib/shim-signed/mok/MOK.{priv,der}`, created by a DKMS install, with a
host-specific subject — but `mokutil --list-enrolled` showed only Canonical's
key, so modules signed with it were rejected.

Enrolling it needs physical presence exactly once, for about two minutes:

```sh
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der   # sets a one-time password
sudo reboot
# at the blue MokManager screen: Enroll MOK -> Continue -> Yes -> password -> reboot
mokutil --list-enrolled | grep -c '^\[key'               # expect 2
```

After that, build and sign the patched module and the unlock becomes persistent:

```sh
sudo ./install.sh --card cmp50hx        # repository root: builds + installs the patched 610 set
# the DKMS hook signs with /var/lib/shim-signed/mok/MOK.priv automatically
```

Two things to check before that run: the patch targets 610.43.03 sources, and the
signed 610 module packages for this kernel are missing/skewed (section 3), so the
patched build has to be produced by DKMS from the 610.57.04 source package
(`nvidia-kernel-source-610`) rather than from `linux-modules-nvidia-610-*`.

