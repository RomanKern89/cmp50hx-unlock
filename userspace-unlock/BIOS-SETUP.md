# ASUS X99-E WS: turning Secure Boot off and enabling a serial console

Board: ASUS X99-E WS, BIOS 3901 (AMI Aptio V with the ASUS UEFI front end).

**Prerequisite:** this board has no on-board video and the two CMP 50HX cards
have no outputs, so setup cannot be *seen* today. Any video card with an output
(even an old one) makes both tasks below a three-minute job; it can be removed
afterwards.

## 1. Enter setup

Power on, then tap `Del` repeatedly during POST (ASUS also accepts `F2`).
If the EZ Mode screen appears, press `F7` for Advanced Mode.

## 2. Turn Secure Boot off

`Boot` tab → **Secure Boot**:

| Setting | Value |
| --- | --- |
| `OS Type` | **`Other OS`** (was `Windows UEFI mode`) |

That is enough: the firmware drops the platform keys, enters Setup Mode and
stops enforcing signatures. The equivalent route is `Secure Boot` →
`Key Management` → `Clear Secure Boot keys` → `Yes`.

`F10` → **Save & Exit** → `Yes`.

Verify from Linux:

```sh
mokutil --sb-state            # SecureBoot disabled
cat /sys/kernel/security/lockdown   # none  (was: none [integrity] confidentiality)
```

To undo later: `Key Management` → `Restore Factory Keys` (or `Install default
Secure Boot keys`), then `OS Type` = `Windows UEFI mode`.

With Secure Boot off, `sig_enforce` is no longer forced, so a locally built
module loads without any enrolment — `tools/finish-mok.sh` detects this and skips
the whole MOK requirement.

## 3. Enable serial console redirection

`Advanced` tab → **Serial Port Console Redirection**:

| Setting | Value |
| --- | --- |
| `COM1 Console Redirection` | **`Enabled`** |
| `Terminal Type` | `VT-UTF8` (or `ANSI` if your client is old) |
| `Bits per second` | `115200` |
| `Data Bits` | `8` |
| `Parity` | `None` |
| `Stop Bits` | `1` |
| `Flow Control` | `None` |
| `VT-UTF8 Combo Key Support` | `Enabled` |
| `Recorder Mode` | `Disabled` |
| `Resolution 100x31` | `Enabled` |
| `Legacy OS Redirection Resolution` | `80x24` |
| `Putty KeyPad` | `VT100` |
| `Redirection After BIOS POST` | **`Always Enable`** |

Also check that the port itself is on: `Advanced` → `Serial Port 1
Configuration` (some BIOS revisions put it under `Onboard Devices
Configuration`) → `Serial Port` = `Enabled`.

`F10` → **Save & Exit**.

Hardware: the board exposes **COM1 as an internal 10-pin header**, not as a rear
connector, so a DB9 bracket (or a USB-TTL adapter wired to the header) is
needed. Wiring for a standard bracket: pin 1 = DCD, 2 = RXD, 3 = TXD, 4 = DTR,
5 = GND, 6 = DSR, 7 = RTS, 8 = CTS, 9 = RI, 10 = key.

`Always Enable` is what makes this useful: firmware setup, MokManager, GRUB and
the kernel all appear on the serial line, so the machine gets a real remote
console.

## 3a. PCIe Gen2 needs the slot set to Gen2 as well

The driver patch and `cmp50hx-gen2.service` only work when the card comes up
with its link policy in the Gen2-ready state. The tell is in `dmesg`:

```
CMP50_GEN2: POLICY_PASS phase=post-booter ... PL=00240032   # bit 20 clear -> Gen2 sticks
CMP50_GEN2: POLICY_PASS phase=post-booter ... PL=00140036   # bit 20 set   -> locked
```

In `Advanced` → `System Agent (SA) Configuration` → `PCIe Configuration`, set
the **link speed of the slot that holds the card to `Gen2`** (not `Auto`, not
`Gen1`). On this board the slots sit behind PLX PEX8747 switches, so if the
firmware exposes the switch ports separately, set those to Gen2 as well.

Then **switch the PSU off and on** — a warm reboot does not re-derive the
adoption, which is one-shot per power cycle.

Do not try to force it from the OS with `setpci` on the card: a manual
`Retrain Link` with the policy still locked drops the card off the bus (a reboot
brings it back).

### Result on this board: not attainable

All slots were set to Gen2 and the machine was fully powered off, and it still
does not come up:

```
CPU root port 00:02.0   cap gen2, runs gen2     <- the BIOS setting took
PLX upstream  08:00.0   cap gen3, runs gen2
PLX downstream 09:10.0  cap gen3, runs gen1
CMP 50HX      0a:00.0   cap gen1, runs gen1     <- the card only advertises 2.5 GT/s
PL_LINK_RATE at post-booter = 0x00140036 (bit 20 set, locked)
```

The card is two PLX hops away from the CPU, and it only advertises Gen2 after
its own policy is applied, which happens at POST and depends on the link it
trained on. Behind the switch it trains at Gen1 and the policy comes up locked.
There is no x16 slot wired directly to the CPU on this board, and the only
CPU-direct port (`00:01.0`) is x4 — 2 GB/s, worse than the 4 GB/s of Gen1 x16.

Practical consequence: keep Gen1 x16. It only bounds host↔GPU transfers; the
compute unlock is SM-bound and unaffected (12.9 TFLOP/s with the link at Gen1).
Leave `cmp50hx-gen2.service` disabled: with the policy locked it can only loop
for its full timeout, and its retrain attempts have no effect.

## 4. Make Linux use it

```sh
# /etc/default/grub
GRUB_TERMINAL="serial console"
GRUB_SERIAL_COMMAND="serial --speed=115200 --unit=0 --word=8 --parity=no --stop=1"
GRUB_CMDLINE_LINUX_DEFAULT="console=tty0 console=ttyS0,115200n8"

sudo update-grub
sudo systemctl enable --now serial-getty@ttyS0.service
```

Connect with `screen /dev/ttyUSB0 115200` (or PuTTY in serial mode).

## 5. After Secure Boot is off

```sh
sudo /home/lmserver/cmp50hx-unlock/userspace-unlock/tools/finish-mok.sh
sudo reboot
sudo /home/lmserver/cmp50hx-unlock/userspace-unlock/tools/finish-mok.sh --check
cd /home/lmserver/cmp50hx-unlock/verify && ./rm_issue_rate 0 > /tmp/rm.json && python3 verify.py /tmp/rm.json
```

Expected: the patched `610.43.03` driver loads, `verify.py` prints
`PASS_CMP50HX_ISSUE_RATE_AND_COUNTS`, and FP32 rises well above the ~6.4 TFLOP/s
measured in the clamped state.
