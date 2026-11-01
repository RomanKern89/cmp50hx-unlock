# Userspace CMP 50HX unlock tools. Plain C, no external dependencies.
#
#   cmp50-unlock-us  the pre-driver unlock itself (vfio-pci + hugepages)
#   cmp50-regs       read-only register dump / direct-write probe
#   cmp50-fma        FP32 FFMA throughput through the NVIDIA OpenCL driver
#   cmp50-dp4a       INT8 DP4A throughput through the CUDA driver
#   usplat           platform self-check (pagemap PFNs, hugepages, vfio path)
#
# cmp50-unlock-us and cmp50-fma live in this tree; the firmware blobs come from
# ../efi-unlock/blobs (install-initramfs.sh copies them to /lib/firmware).
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS ?=

all: cmp50-unlock-us cmp50-regs cmp50-fma cmp50-dp4a usplat

cmp50-unlock-us: cmp50-unlock-us.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

cmp50-regs: cmp50-regs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

cmp50-fma: cmp50-fma.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldl

cmp50-dp4a: cmp50-dp4a.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldl

usplat: usplat.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

check: all
	sh tests/smoke.sh ./cmp50-unlock-us

install: cmp50-unlock-us cmp50-regs cmp50-fma cmp50-dp4a
	install -m 0755 cmp50-unlock-us /usr/local/sbin/cmp50-unlock-us
	install -m 0755 cmp50-regs /usr/local/sbin/cmp50-regs
	install -m 0755 cmp50-fma /usr/local/sbin/cmp50-fma
	install -m 0755 cmp50-dp4a /usr/local/sbin/cmp50-dp4a

clean:
	rm -f cmp50-unlock-us cmp50-regs cmp50-fma cmp50-dp4a usplat

.PHONY: all check install clean
