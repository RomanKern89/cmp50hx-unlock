#!/usr/bin/env python3
"""Replay the CMP 50HX PCIe Gen2 retrain sequence outside the driver.

The patched nvidia module runs this exact sequence during device bring-up
(nv_cmp50hx_retrain_gen2 in patches/cmp50hx/04-cmp50-pcie-gen2.patch):
BAR0 0x8872c = 6, LNKCTL2.TLS = 5GT/s on the GPU and its upstream bridge,
then Retrain Link on the upstream bridge. This script replays it on a live
system to tell a boot-time ordering problem from a host that refuses Gen2.
Config-space access goes through setpci (capability-relative addressing).

Run as root on an idle GPU: sudo python3 retrain_gen2.py [BDF]
"""
import mmap
import os
import struct
import subprocess
import sys
import time

GPU_BDF = sys.argv[1] if len(sys.argv) > 1 else "0000:04:00.0"
BASE = "/sys/bus/pci/devices"

LNKCTL = "CAP_EXP+10.W"   # Retrain Link bit
LNKSTA = "CAP_EXP+12.W"   # speed bits 3:0, width bits 9:4
LNKCTL2 = "CAP_EXP+30.W"  # Target Link Speed bits 3:0


def setpci(bdf, reg, val=None):
    spec = f"{reg}={val:04x}" if val is not None else reg
    out = subprocess.run(["setpci", "-s", bdf, spec],
                         capture_output=True, text=True, check=True)
    return int(out.stdout.strip() or "0", 16)


def upstream_of(bdf):
    """Parent PCI device from the sysfs path .../<parent>/<dev>."""
    path = os.path.realpath(os.path.join(BASE, bdf))
    return path.rstrip("/").split("/")[-2]


def lnksta(bdf):
    v = setpci(bdf, LNKSTA)
    speed = {1: "2.5GT/s", 2: "5GT/s", 3: "8GT/s"}.get(v & 0xF, f"?{v & 0xF:x}")
    return f"speed={speed} width=x{((v >> 4) & 0x3F) + 1} raw=0x{v:04x}"


def main():
    if os.geteuid() != 0:
        raise SystemExit("must run as root")

    up = upstream_of(GPU_BDF)
    print(f"gpu={GPU_BDF} upstream={up}")

    for tag, bdf in (("gpu", GPU_BDF), ("upstream", up)):
        ctl2 = setpci(bdf, LNKCTL2)
        print(f"{tag:9s} before: {lnksta(bdf)} lnkctl2=0x{ctl2:04x}")

    with open(os.path.join(BASE, GPU_BDF, "resource0"), "r+b", buffering=0) as f:
        bar0 = mmap.mmap(f.fileno(), 0x90000)

        def rd32(off):
            return int.from_bytes(bar0[off:off + 4], "little")

        def wr32(off, val):
            bar0[off:off + 4] = val.to_bytes(4, "little")

        cya0 = rd32(0x8C2C0)
        cfg = rd32(0x8C040)
        pl = rd32(0x8C1C0)
        print(f"policy   : cya0=0x{cya0:08x} cfg=0x{cfg:08x} pl=0x{pl:08x}")
        # same acceptance window as the driver's retrain check
        if (cya0 & (1 << 2)) or ((cfg >> 18) & 3) != 2 or (pl & 0x60000) != 0x40000:
            print("policy   : GSP Gen2 policy NOT open (driver would skip too); aborting")
            return 1

        wr32(0x8872C, 6)
        print(f"poke     : bar0+0x8872c -> 6 (readback {rd32(0x8872C)})")
        bar0.close()

    time.sleep(0.05)
    for bdf in (GPU_BDF, up):
        setpci(bdf, LNKCTL2, (setpci(bdf, LNKCTL2) & ~0xF) | 2)
        after = setpci(bdf, LNKCTL2)
        print(f"tls      : {bdf} lnkctl2=0x{after:04x} "
              f"({'stuck' if (after & 0xF) == 2 else 'DID NOT STICK'})")
    setpci(up, LNKCTL, setpci(up, LNKCTL) | 0x20)

    for attempt in range(25):
        time.sleep(0.1)
        if (setpci(GPU_BDF, LNKSTA) & 0xF) >= 2:
            print(f"result   : RETRAIN_PASS attempt={attempt + 1} {lnksta(GPU_BDF)}")
            print(f"upstream : {lnksta(up)}")
            return 0
    print(f"result   : RETRAIN_FAIL {lnksta(GPU_BDF)}")
    print(f"upstream : {lnksta(up)}")

    # beyond what the driver does: also trigger retrain from the GPU side
    print("fallback : setting Retrain Link on the GPU side too")
    setpci(GPU_BDF, LNKCTL, setpci(GPU_BDF, LNKCTL) | 0x20)
    for attempt in range(25):
        time.sleep(0.1)
        if (setpci(GPU_BDF, LNKSTA) & 0xF) >= 2:
            print(f"result   : RETRAIN_PASS(gpu-side) attempt={attempt + 1} {lnksta(GPU_BDF)}")
            print(f"upstream : {lnksta(up)}")
            return 0
    print(f"result   : RETRAIN_FAIL(gpu-side) {lnksta(GPU_BDF)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
