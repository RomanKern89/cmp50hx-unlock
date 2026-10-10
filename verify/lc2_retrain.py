#!/usr/bin/env python3
"""Retrain CMP 50HX to Gen2 via the BAR0 LNKCTL2 mirror.

Hypothesis under test: on hosts where the GPU's config-space LNKCTL2 write
is silently dropped, the BAR0 mirror at 0x880A8 (CMP50_PCIE_LINK_CTRL2,
logged as LC2 by the patched driver) may still accept the Target Link Speed
write — every other BAR0 register the policy function writes latches fine.
Sequence: policy window check -> LC2 mirror TLS=5GT -> LTSSM=6 poke ->
upstream TLS + Retrain Link -> poll.

Run as root on an idle GPU: sudo python3 lc2_retrain.py [BDF]
"""
import mmap
import os
import subprocess
import sys
import time

GPU_BDF = sys.argv[1] if len(sys.argv) > 1 else "0000:04:00.0"
BASE = "/sys/bus/pci/devices"

LC2_MIRROR = 0x880A8       # CMP50_PCIE_LINK_CTRL2
LNKSTA_MIRROR = 0x88088    # CMP50_PCIE_LINK_STATUS ([31:16] = LNKSTA)
LTSSM = 0x8872C            # CMP50_PCIE_LTSSM
PRIV_MISC_1 = 0x8841C      # CMP50_PCIE_PRIV_MISC_1
XP3G_PLM0 = 0x8E1B0        # must read 0xffffffff = unprotected
CYA0 = 0x8C2C0
LINK_CONFIG0 = 0x8C040
PL_LINK_RATE = 0x8C1C0
GEN2_EN = (1 << 11) | (1 << 13)
GEN2_VAL = (1 << 12) | (1 << 14)


def setpci(bdf, reg, val=None):
    spec = f"{reg}={val:04x}" if val is not None else reg
    out = subprocess.run(["setpci", "-s", bdf, spec],
                         capture_output=True, text=True, check=True)
    return int(out.stdout.strip() or "0", 16)


def speed_str(v):
    s = {1: "2.5GT/s", 2: "5GT/s", 3: "8GT/s"}.get(v, f"?{v:x}")
    return f"{s} width=x{((v >> 4) & 0x3F) + 1} raw=0x{v:04x}"


def main():
    if os.geteuid() != 0:
        raise SystemExit("must run as root")
    up = os.path.realpath(os.path.join(BASE, GPU_BDF)).rstrip("/").split("/")[-2]
    print(f"gpu={GPU_BDF} upstream={up}")
    print(f"config  : gpu {speed_str(setpci(GPU_BDF, 'CAP_EXP+12.W'))} "
          f"tls=0x{setpci(GPU_BDF, 'CAP_EXP+30.W'):04x} | "
          f"upstream tls=0x{setpci(up, 'CAP_EXP+30.W'):04x}")

    with open(os.path.join(BASE, GPU_BDF, "resource0"), "r+b", buffering=0) as f:
        bar0 = mmap.mmap(f.fileno(), 0x90000)

        def rd32(off):
            return int.from_bytes(bar0[off:off + 4], "little")

        def wr32(off, val):
            bar0[off:off + 4] = val.to_bytes(4, "little")

        plm = rd32(XP3G_PLM0)
        priv = rd32(PRIV_MISC_1)
        cya0 = rd32(CYA0)
        cfg = rd32(LINK_CONFIG0)
        pl = rd32(PL_LINK_RATE)
        print(f"policy  : plm=0x{plm:08x} priv=0x{priv:08x} cya0=0x{cya0:08x} "
              f"cfg=0x{cfg:08x} pl=0x{pl:08x}")
        print(f"mirror  : lc2=0x{rd32(LC2_MIRROR):08x} "
              f"stat=0x{rd32(LNKSTA_MIRROR):08x} ltssm=0x{rd32(LTSSM):08x}")

        if plm != 0xFFFFFFFF:
            print("policy  : XP3G PLM gate CLOSED; aborting (needs early GSP phase)")
            return 1
        if (cya0 & (1 << 2)) or ((cfg >> 18) & 3) != 2 or (pl & 0x60000) != 0x40000:
            print("policy  : Gen2 window NOT open; aborting")
            return 1

        lc2 = rd32(LC2_MIRROR)
        wr32(LC2_MIRROR, (lc2 & ~0xF) | 2)
        after = rd32(LC2_MIRROR)
        print(f"lc2     : 0x{lc2:08x} -> 0x{after:08x} "
              f"({'LATCHED' if (after & 0xF) == 2 else 'DID NOT LATCH'})")
        if (after & 0xF) != 2:
            return 1

        wr32(LTSSM, 6)
        print(f"ltssm   : -> 6 (readback {rd32(LTSSM)})")
        bar0.close()

    time.sleep(0.05)
    setpci(up, "CAP_EXP+30.W", (setpci(up, "CAP_EXP+30.W") & ~0xF) | 2)
    setpci(up, "CAP_EXP+10.W", setpci(up, "CAP_EXP+10.W") | 0x20)

    for attempt in range(40):
        time.sleep(0.1)
        sta = setpci(GPU_BDF, "CAP_EXP+12.W")
        if (sta & 0xF) >= 2:
            print(f"result  : GEN2 PASS attempt={attempt + 1} {speed_str(sta)}")
            return 0
    print(f"result  : still Gen1 {speed_str(setpci(GPU_BDF, 'CAP_EXP+12.W'))}")
    print("fallback: Retrain Link from the GPU side too")
    setpci(GPU_BDF, "CAP_EXP+10.W", setpci(GPU_BDF, "CAP_EXP+10.W") | 0x20)
    for attempt in range(40):
        time.sleep(0.1)
        sta = setpci(GPU_BDF, "CAP_EXP+12.W")
        if (sta & 0xF) >= 2:
            print(f"result  : GEN2 PASS(gpu-side) attempt={attempt + 1} {speed_str(sta)}")
            return 0
    print(f"result  : still Gen1 {speed_str(setpci(GPU_BDF, 'CAP_EXP+12.W'))}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
