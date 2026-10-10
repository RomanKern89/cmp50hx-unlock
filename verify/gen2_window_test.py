#!/usr/bin/env python3
"""Find when (if ever) the GPU's LNKCTL2.TLS becomes writable on this host.

Reloads the nvidia module and samples/writes TLS every ~20 ms through the
whole GSP bootstrap. Three possible outcomes, each diagnostic:
  - TLS latches early, stays       -> the retrain must move into the driver's
                                      early phase (patch fix), retry late fails
  - TLS latches then reverts       -> GSP-RM rewrites it; race must be won
  - TLS never latches              -> host/silicon refuses; not fixable here
On latch: poke BAR0, set TLS on both ends, retrain, report LNKSTA.

Run as root on an idle GPU: sudo python3 gen2_window_test.py [BDF] [seconds]
"""
import mmap
import os
import re
import subprocess
import sys
import time

GPU_BDF = sys.argv[1] if len(sys.argv) > 1 else "0000:04:00.0"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
BASE = "/sys/bus/pci/devices"
SAMPLE_EVERY = 0.02


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def upstream_of(bdf):
    return os.path.realpath(os.path.join(BASE, bdf)).rstrip("/").split("/")[-2]


def cap_exp_off(bdf):
    out = sh(["lspci", "-vv", "-s", bdf])
    m = re.search(r"Capabilities: \[([0-9a-f]+)(?: v\d+)?\] Express", out)
    if not m:
        raise SystemExit(f"{bdf}: cannot locate PCIe cap")
    return int(m.group(1), 16)


def lnksta_str(v):
    speed = {1: "2.5GT/s", 2: "5GT/s", 3: "8GT/s"}.get(v & 0xF, f"?{v & 0xF:x}")
    return f"speed={speed} width=x{((v >> 4) & 0x3F) + 1} raw=0x{v:04x}"


def main():
    if os.geteuid() != 0:
        raise SystemExit("must run as root")

    up = upstream_of(GPU_BDF)
    cfg_gpu = os.path.join(BASE, GPU_BDF, "config")
    fd = os.open(cfg_gpu, os.O_RDWR)
    cap = cap_exp_off(GPU_BDF)
    lnkctl2_off = cap + 0x30
    lnksta_off = cap + 0x12
    lnkctl_off = cap + 0x10
    print(f"gpu={GPU_BDF} upstream={up} cap=0x{cap:x} lnkctl2=0x{lnkctl2_off:x}")
    print(f"before: gpu tls=0x{os.pread(fd, 2, lnkctl2_off).hex()} "
          f"{lnksta_str(int.from_bytes(os.pread(fd, 2, lnksta_off), 'little'))}")

    print("reloading nvidia module ...")
    subprocess.run(["systemctl", "stop", "cmp-idle-governor"], capture_output=True)
    for m in ("nvidia_drm", "nvidia_modeset", "nvidia_peermem", "nvidia_uvm", "nvidia"):
        subprocess.run(["rmmod", m], capture_output=True)
    if "nvidia " in open("/proc/modules").read():
        raise SystemExit("rmmod failed; holders still present")

    bar0_res = os.path.join(BASE, GPU_BDF, "resource0")
    t0 = time.monotonic()
    subprocess.Popen(["modprobe", "nvidia"])

    latched_at = None
    transient = []
    while time.monotonic() - t0 < DURATION:
        os.pwrite(fd, b"\x02", lnkctl2_off)  # TLS bits = 5GT/s
        rd = int.from_bytes(os.pread(fd, 2, lnkctl2_off), "little")
        if (rd & 0xF) == 2:
            latched_at = time.monotonic() - t0
            break
        if (rd & 0xF) not in (1,):
            transient.append((round(time.monotonic() - t0, 2), hex(rd)))
        time.sleep(SAMPLE_EVERY)

    if latched_at is None:
        print(f"result: TLS NEVER latched in {DURATION:.0f}s "
              f"(transients: {transient[:10] or 'none'})")
        _ = subprocess.run(["systemctl", "start", "cmp-idle-governor"], capture_output=True)
        return 1

    print(f"result: TLS LATCHED at t+{latched_at:.2f}s after modprobe")
    with open(bar0_res, "r+b", buffering=0) as f:
        bar0 = mmap.mmap(f.fileno(), 0x90000)
        bar0[0x8872C:0x88730] = (6).to_bytes(4, "little")
        bar0.close()
    # upstream side: TLS 5GT/s + Retrain Link
    up_ctl2 = int(sh(["setpci", "-s", up, "CAP_EXP+30.W"]).strip(), 16)
    subprocess.run(["setpci", "-s", up, f"CAP_EXP+30.W={(up_ctl2 & ~0xF) | 2:04x}"], check=True)
    up_ctl = int(sh(["setpci", "-s", up, "CAP_EXP+10.W"]).strip(), 16)
    subprocess.run(["setpci", "-s", up, f"CAP_EXP+10.W={up_ctl | 0x20:04x}"], check=True)

    for attempt in range(40):
        time.sleep(0.1)
        sta = int.from_bytes(os.pread(fd, 2, lnksta_off), "little")
        tls = int.from_bytes(os.pread(fd, 2, lnkctl2_off), "little")
        if (sta & 0xF) >= 2 and (tls & 0xF) == 2:
            print(f"result: LINK AT GEN2 attempt={attempt + 1} {lnksta_str(sta)}")
            _ = subprocess.run(["systemctl", "start", "cmp-idle-governor"], capture_output=True)
            return 0
        if (tls & 0xF) != 2:
            print(f"result: TLS reverted to 0x{tls:04x} at t+{time.monotonic() - t0:.2f}s "
                  f"before link trained ({lnksta_str(sta)})")
            break
    print(f"result: no Gen2 ({lnksta_str(int.from_bytes(os.pread(fd, 2, lnksta_off), 'little'))})")
    _ = subprocess.run(["systemctl", "start", "cmp-idle-governor"], capture_output=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
