#!/usr/bin/env python3
"""Watch link registers at ms resolution while forcing retrains.

Answers: does the link attempt a higher speed and fall back? We sample the
GPU config LNKSTA, upstream LNKSTA, the BAR0 status mirror (0x88088) and the
LTSSM register (0x8872c) while setting Target Link Speed on the root port to
5GT/s and then 8GT/s and firing Retrain Link. Distinct states are printed
with their first-seen offsets; an attempt+fallback would show up as a
transient speed > 2.5GT/s or LTSSM flapping.

Run as root on an idle GPU: sudo python3 gen2_watch_train.py [BDF]
"""
import mmap
import os
import sys
import time

GPU_BDF = sys.argv[1] if len(sys.argv) > 1 else "0000:04:00.0"
BASE = "/sys/bus/pci/devices"
LNKCTL = 0x10
LNKSTA = 0x12
LNKCTL2 = 0x30

up = os.path.realpath(os.path.join(BASE, GPU_BDF)).rstrip("/").split("/")[-2]
gpu_fd = os.open(os.path.join(BASE, GPU_BDF, "config"), os.O_RDWR)
up_fd = os.open(os.path.join(BASE, up, "config"), os.O_RDWR)


def cap_off(bdf):
    import re
    import subprocess
    out = subprocess.run(["lspci", "-vv", "-s", bdf],
                         capture_output=True, text=True).stdout
    m = re.search(r"Capabilities: \[([0-9a-f]+)(?: v\d+)?\] Express", out)
    if not m:
        raise SystemExit(f"{bdf}: cannot locate PCIe cap")
    return int(m.group(1), 16)


GPU_CAP = cap_off(GPU_BDF)
UP_CAP = cap_off(up)
res0 = open(os.path.join(BASE, GPU_BDF, "resource0"), "r+b", buffering=0)
bar0 = mmap.mmap(res0.fileno(), 0x90000)


def rd16(fd, off):
    return int.from_bytes(os.pread(fd, 2, off), "little")


def wr16(fd, off, val):
    os.pwrite(fd, val.to_bytes(2, "little"), off)


def rd32(mmoff):
    return int.from_bytes(bar0[mmoff:mmoff + 4], "little")


def sample():
    g = rd16(gpu_fd, GPU_CAP + LNKSTA)
    u = rd16(up_fd, UP_CAP + LNKSTA)
    m = rd32(0x88088)
    lt = rd32(0x8872C)
    return (g, u, m, lt)


def watch(seconds, label):
    seen = {}
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        s = sample()
        if s not in seen:
            seen[s] = time.monotonic() - t0
    print(f"-- distinct states during {label} ({len(seen)}):")
    for s, t in sorted(seen.items(), key=lambda kv: kv[1]):
        g, u, m, lt = s
        print(f"   t={t:6.3f}s gpu_sta=0x{g:04x} (sp={g & 0xF} w={((g >> 4) & 0x3F) + 1}) "
              f"up_sta=0x{u:04x} (sp={u & 0xF}) mirror=0x{m:08x} ltssm=0x{lt:08x}")


def set_target_and_retrain(tls):
    wr16(up_fd, UP_CAP + LNKCTL2, (rd16(up_fd, UP_CAP + LNKCTL2) & ~0xF) | tls)
    wr16(up_fd, UP_CAP + LNKCTL, rd16(up_fd, UP_CAP + LNKCTL) | 0x20)


print(f"gpu={GPU_BDF} (cap 0x{GPU_CAP:x}) upstream={up} (cap 0x{UP_CAP:x})")
print(f"start: gpu_tls=0x{rd16(gpu_fd, GPU_CAP + LNKCTL2):04x} "
      f"up_tls=0x{rd16(up_fd, UP_CAP + LNKCTL2):04x} ltssm=0x{rd32(0x8872C):08x}")

watch(0.5, "baseline")
set_target_and_retrain(2)   # Gen2 target, like the driver
watch(3.0, "Gen2 target retrain")
set_target_and_retrain(3)   # Gen3 target
watch(3.0, "Gen3 target retrain")
# leave the root port where the driver leaves it
wr16(up_fd, UP_CAP + LNKCTL2, (rd16(up_fd, UP_CAP + LNKCTL2) & ~0xF) | 2)
print(f"end: gpu_sta=0x{rd16(gpu_fd, GPU_CAP + LNKSTA):04x} "
      f"up_tls=0x{rd16(up_fd, UP_CAP + LNKCTL2):04x} ltssm=0x{rd32(0x8872C):08x}")
bar0.close()
