# Results: userspace unlock on the two 20 GiB CMP 50HX (host .214)

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
| Driver registry override `NVreg_RegistryDwords="RMOverrideSmSpeedSelect=0x88888888;RMOverrideSmSpeedSelect1=0x00000008"` | Key is accepted (`/proc/driver/nvidia/params` shows it) but every RM allocation then fails with `0x1b` — the documented "unverified" branch does not work on 615 |
| Switch to the project's validated driver generation | **`linux-modules-nvidia-610-7.0.0-42-generic` does not exist** in the Ubuntu archive, so no signed 610 module is available for this kernel |

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
