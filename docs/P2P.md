# P2P between CMP 50HX cards (and RTX 20 series) — verified recipe

Issue #14 (avb80) proved working peer-to-peer CUDA access between an
RTX 2080 Ti and 3× CMP 50HX on Proxmox 7.0.14, driver 610.43.03 built
from open-gpu-kernel-modules with the cmp50hx-unlock patches plus the
aikitoria P2P patch applied on top.

The local build now applies the same P2P path for 610.43.03. It includes
the TU102 BAR1 sizing patch for an RTX 2080 Ti peer, the aikitoria P2P
patch, and the Turing BAR1 P2P fixes from issue #14. The CMP 50HX BAR1
size stays controlled by cmp50_rebar_size: selector 8 gives 16 GiB and
selector 9 gives 32 GiB when the card supports it. The RTX 2080 Ti
patch sets tu102_bar1_size to 9 by default.

The upstream issue report is hardware-verified. On 2026-10-06, this repo's
build was installed on Ubuntu 24.04.4 with kernel 6.8.0-139-generic and
two 20 GiB CMP 50HX cards. nvidia-smi topo -p2p r and -p2p w reported
OK for both GPU directions.

This checks the driver's P2P capability report; no CUDA transfer benchmark
was run. On this host, booting with intel_iommu=on iommu=pt made GSP startup
fail. The prior mitigations=off boot line was restored.

Kernel command line used:

```
intel_iommu=on iommu=pt video=efifb:off pci=realloc,hpmemsize=8G,hpmmiosize=256M disable_acs_redir=<bdf-of-each-switch-port>
```

Notes:

- `pci=realloc,hpmemsize=8G` makes Linux grow the prefetchable bridge
  windows at boot — the same effect the pre-OS ReBAR activation needs
  when the firmware-assigned window is too small.
- Each GPU needs ReBAR enabled: the kernel-side patch
  `patches/cmp50hx/03-cmp50-rebar.patch` (selector 8 = 16 GiB; #47 also
  proved selector 9 = 32 GiB on 20/22 GB mods) or the pre-OS activation
  in the unlock EFI (v1.1.13+).
- Verification: `p2pBandwidthLatencyTest` (CUDA samples) must print
  `CAN Access Peer Device` for every pair.

Full thread with lspci/ReBAR output and numbers:
https://github.com/xrip/cmp50hx-unlock/issues/14
### Measured on lmserver, 2026-11-14 (CUDA benchmark — the gap this doc flagged)

First actual CUDA transfer measurement of the pair (kernel `7.0.0-43-generic`,
driver 610.43.03, both cards at Gen2, 256 MiB buffers, 20 iterations, best of
run, mining active):

| path | bandwidth |
|---|---|
| `cudaMemcpyPeer` GPU0 -> GPU1 | **0.42 GB/s** |
| `cudaMemcpyPeer` GPU1 -> GPU0 | **0.40 GB/s** |
| staged via host (D2H + H2D) | **4.57 GB/s** |

`nvidia-smi topo -p2p r` reports `OK` and `cudaDeviceCanAccessPeer` returns 1 in
both directions, so peer access is *functional* — but it is an order of
magnitude slower than staging through the host, i.e. the direct path is not in
use. The cause is visible in the topology and the ACS state:

- the two cards are on **different** PEX8747 switches
  (`00:03.0 -> 03:00.0 -> 04:10.0 -> 05:00.0` and
  `00:02.0 -> 07:00.0 -> 08:11.0 -> 0a:00.0`), so peer traffic has to cross the
  root complex instead of staying inside one switch;
- every PLX downstream port still advertises
  `ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd+`, and the boot
  line carries **none** of the recipe above (`intel_iommu=on iommu=pt`,
  `pci=realloc,hpmemsize=...`, `disable_acs_redir=<ports>`).

So on this host P2P is currently slower than host staging and should not be
used. To revisit: apply the boot recipe to the ports in the chains listed here,
re-run this measurement, and only then decide. Same-switch P2P (both cards
behind one PEX8747) is the configuration where the switch shortcut pays off.
