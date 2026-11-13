#!/usr/bin/env python3
"""Verify a CMP 50HX trained its PCIe link to the generation it adopted.

This is a read-only, root-run check meant to run after the boot-time unlock
service (cmp50hx-gen2, which now requests Gen3 by default and falls back to
whatever the card advertises). It answers one question with a clear PASS/FAIL:
did the link actually reach the generation the card advertises as its maximum,
and is that generation at least Gen2 (i.e. the unlock took effect this boot)?

Two independent sources are read and cross-checked:
  - PCIe config space, at the Express capability's register offsets, and
  - the BAR0 register mirror of those same registers.
LNKCAP (max link speed) gives the card's advertised target generation,
LNKSTA (current link speed/width) gives what the link actually negotiated,
and LNKCAP2's Supported Link Speeds Vector tells whether anything beyond the
locked 2.5 GT/s was ever advertised this boot. A link sitting at 2.5 GT/s with
only the 2.5 GT/s bit advertised means the policy was NOT applied/adopted --
that is reported explicitly instead of silently passing.

Run as root on an idle GPU: sudo python3 gen3_retrain.py [BDF]
Exit code 0 on PASS, 1 on FAIL.
"""
import os
import sys
import mmap
from typing import Optional, Tuple

BASE = "/sys/bus/pci/devices"
DEFAULT_BDF = "0000:04:00.0"

# PCIe Express capability register offsets, relative to the capability base.
LNKCAP = 0x0C    # Link Capabilities (32-bit): max speed [3:0], max width [9:4]
LNKCTL = 0x10    # Link Control (16-bit) -- low half of the 32-bit word below
LNKSTA = 0x12    # Link Status (16-bit): cur speed [3:0], neg width [9:4]
LNKCAP2 = 0x2C   # Link Capabilities 2 (32-bit): SLS vector in bits [7:1]
LNKCTL2 = 0x30   # Link Control 2 (16-bit): target link speed [3:0]

# BAR0 mirror of the same Express registers. The mirror base is chosen so that
# LNKCAP lands at 0x88084 (verified against the regmap); the others follow by
# their capability-relative offsets.
BAR0_LNKCAP = 0x88084     # mirrors config LNKCAP   (cap+0x0c)
BAR0_LNKCTL_STA = 0x88088  # mirrors config LNKCTL/LNKSTA (cap+0x10): STA = high 16
BAR0_LNKCAP2 = 0x880A4    # mirrors config LNKCAP2  (cap+0x2c)
BAR0_LNKCTL2 = 0x880A8    # mirrors config LNKCTL2/STA2 (cap+0x30): CTL2 = low 16
BAR0_MAP_LEN = 0x90000

# Encoding shared by LNKCAP max-speed and LNKSTA current-speed fields.
_GEN_GTS = {1: "2.5GT/s", 2: "5.0GT/s", 3: "8.0GT/s", 4: "16.0GT/s", 5: "32.0GT/s"}


def gts_str(speed_field: int) -> str:
    """Human label for a 4-bit PCIe link-speed field value."""
    return _GEN_GTS.get(speed_field, f"?0x{speed_field:x}")


def upstream_of(bdf: str) -> Optional[str]:
    """Return the parent (upstream bridge) BDF from the sysfs realpath.

    Mirrors gen2_watch_train.py: .../<parent>/<dev> -> <parent>.
    """
    try:
        path = os.path.realpath(os.path.join(BASE, bdf))
        return path.rstrip("/").split("/")[-2]
    except Exception as exc:  # noqa: BLE001 - diagnostic, never crash
        print(f"note: could not resolve upstream of {bdf}: {exc}")
        return None


def cap_exp_off(fd: int) -> Optional[int]:
    """Walk the config-space capability list to find the PCI Express cap (0x10).

    Reads directly from the open config fd so it does not depend on lspci.
    """
    try:
        status = int.from_bytes(os.pread(fd, 2, 0x06), "little")
        if not (status & 0x10):  # Capabilities List bit
            print("note: device reports no capability list")
            return None
        ptr = os.pread(fd, 1, 0x34)[0] & 0xFC
        seen = 0
        while ptr and seen < 48:
            cap_id = os.pread(fd, 1, ptr)[0]
            if cap_id == 0x10:  # PCI Express
                return ptr
            ptr = os.pread(fd, 1, ptr + 1)[0] & 0xFC
            seen += 1
        print("note: PCI Express capability not found in config space")
        return None
    except Exception as exc:  # noqa: BLE001
        print(f"note: capability walk failed: {exc}")
        return None


def read_cfg32(fd: int, off: int) -> Optional[int]:
    """Read a 32-bit config-space register, or None on failure."""
    try:
        return int.from_bytes(os.pread(fd, 4, off), "little")
    except Exception as exc:  # noqa: BLE001
        print(f"note: config read at 0x{off:x} failed: {exc}")
        return None


def read_cfg16(fd: int, off: int) -> Optional[int]:
    """Read a 16-bit config-space register, or None on failure."""
    try:
        return int.from_bytes(os.pread(fd, 2, off), "little")
    except Exception as exc:  # noqa: BLE001
        print(f"note: config read at 0x{off:x} failed: {exc}")
        return None


def read_bar0(bdf: str) -> Optional[Tuple[int, int, int, int]]:
    """Return (lnkcap, lnkctl_sta, lnkcap2, lnkctl2) from the BAR0 mirror.

    Returns None if BAR0 cannot be mapped or read; each 32-bit word is the
    raw mirror value at its documented offset.
    """
    res0 = os.path.join(BASE, bdf, "resource0")
    try:
        with open(res0, "r+b", buffering=0) as f:
            bar0 = mmap.mmap(f.fileno(), BAR0_MAP_LEN)
            try:
                def rd(off: int) -> int:
                    return int.from_bytes(bar0[off:off + 4], "little")

                return (rd(BAR0_LNKCAP), rd(BAR0_LNKCTL_STA),
                        rd(BAR0_LNKCAP2), rd(BAR0_LNKCTL2))
            finally:
                bar0.close()
    except Exception as exc:  # noqa: BLE001
        print(f"note: BAR0 mirror unavailable ({res0}): {exc}")
        return None


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    if os.geteuid() != 0:
        print("FAIL: must run as root (needs config space + BAR0 access)")
        return 1

    bdf = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BDF
    up = upstream_of(bdf)
    print(f"gpu={bdf} upstream={up or '?'}")

    cfg_path = os.path.join(BASE, bdf, "config")
    try:
        fd = os.open(cfg_path, os.O_RDONLY)
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: cannot open {cfg_path}: {exc}")
        return 1

    try:
        cap = cap_exp_off(fd)
        if cap is None:
            print("FAIL: no PCIe Express capability; cannot read link state")
            return 1

        lnkcap = read_cfg32(fd, cap + LNKCAP)
        lnksta = read_cfg16(fd, cap + LNKSTA)
        lnkcap2 = read_cfg32(fd, cap + LNKCAP2)
        lnkctl2 = read_cfg16(fd, cap + LNKCTL2)
    finally:
        os.close(fd)

    if lnkcap is None or lnksta is None:
        print("FAIL: could not read LNKCAP/LNKSTA from config space")
        return 1

    # Config-space derived values.
    adv_gen = lnkcap & 0xF          # advertised max generation (Max Link Speed)
    cur_gen = lnksta & 0xF          # current negotiated generation
    cur_width = (lnksta >> 4) & 0x3F
    tgt_gen = (lnkctl2 & 0xF) if lnkctl2 is not None else None
    sls_vector = (lnkcap2 & 0xFE) if lnkcap2 is not None else None  # bits [7:1]

    # BAR0 mirror cross-check.
    mirror = read_bar0(bdf)
    if mirror is not None:
        m_cap, m_ctlsta, m_cap2, m_ctl2 = mirror
        m_adv = m_cap & 0xF
        m_sta = (m_ctlsta >> 16) & 0xFFFF
        m_cur = m_sta & 0xF
        m_width = (m_sta >> 4) & 0x3F
        m_tgt = m_ctl2 & 0xF
        m_sls = m_cap2 & 0xFE
    else:
        m_adv = m_cur = m_width = m_tgt = m_sls = None

    print("")
    print("source      maxLNKCAP   curLNKSTA              tgtLNKCTL2  SLSvector")
    print(f"config      Gen{adv_gen} {gts_str(adv_gen):<7}"
          f"  Gen{cur_gen} {gts_str(cur_gen):<7} x{cur_width:<3}"
          f"  {('Gen' + str(tgt_gen)) if tgt_gen is not None else '  ?'}"
          f"       {('0x%04x' % sls_vector) if sls_vector is not None else '?'}")
    if mirror is not None:
        print(f"bar0mirror  Gen{m_adv} {gts_str(m_adv):<7}"
              f"  Gen{m_cur} {gts_str(m_cur):<7} x{m_width:<3}"
              f"  {('Gen' + str(m_tgt))}"
              f"       0x{m_sls:04x}")
        if (m_adv, m_cur) != (adv_gen, cur_gen):
            print("note: config space and BAR0 mirror disagree on speed fields")
    print("")

    # NEGATIVE / sanity note: only 2.5 GT/s advertised => policy not adopted.
    if sls_vector is not None and sls_vector == 0x0002:
        print("FAIL: LNKCAP2 advertises only 2.5GT/s (SLS vector 0x0002) -- the "
              "unlock policy was NOT applied/adopted this boot.")
        return 1
    if sls_vector is None:
        print("note: LNKCAP2 unreadable; cannot confirm the advertised speed vector")

    # PASS when the link reached the card's advertised max and that is >= Gen2.
    if adv_gen < 2:
        print(f"FAIL: card advertises only Gen{adv_gen} ({gts_str(adv_gen)}); "
              "unlock did not raise the advertised maximum above Gen1.")
        return 1
    if cur_gen == adv_gen:
        print(f"PASS: link trained to its advertised Gen{adv_gen} "
              f"({gts_str(cur_gen)} x{cur_width}).")
        return 0
    print(f"FAIL: link at Gen{cur_gen} ({gts_str(cur_gen)} x{cur_width}) but "
          f"card advertises Gen{adv_gen} ({gts_str(adv_gen)}); retrain incomplete.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
