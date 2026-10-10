# CMP50HX PCIe Link Disable live audit

Test host: `192.168.1.224`, Xeon E5-2670 v2 / X79, Linux
`6.8.0-138-generic`, NVIDIA `610.43.03`, CMP50HX at `0000:04:00.0`.
This is not an Alder Lake host, so it cannot prove an Alder Lake fix.

The delayed `cmp50hx-gen2.service` was disabled before each boot. Systemd units,
timers, cron, `rc.local`, and initramfs were checked for another retrain trigger.
There was none.

- A Link Disable cycle after `rm_init_adapter()` reached Gen2, then BAR0 and PCI
  reads returned `ffffffff`. The kernel logged Xid 79 and Xid 154, and
  `nvidia-smi` lost the GPU.
- A Link Disable cycle before `rm_init_adapter()` restored PCI config access,
  but RM initialization did not complete after repeated attempts. The GPU stayed
  at Gen1 and `nvidia-smi` found no device.
- Restoring the original modules and the delayed service restored a healthy GPU
  at Gen2 x4 with no Xid in the recovery boot.
- The final diagnostics-only module (`d23e376c...`) was tested in boot
  `88f88dcc-7031-413d-afb8-9d46710c7b74`. The service was disabled and inactive,
  had no boot messages, and had no helper process. The kernel retrain failed at
  Gen1 as expected, while `nvidia-smi` stayed healthy and no Xid or fatal AER was
  logged. Starting the service by hand was the only action that changed the link
  to Gen2 x4; the GPU stayed healthy.

Conclusion: do not run a Link Disable cycle from this CMP50HX kernel path. Keep
the post-RM diagnostic logging and the normal retrain. Alder Lake needs a test on
real Alder Lake hardware and a different safe initialization boundary.
