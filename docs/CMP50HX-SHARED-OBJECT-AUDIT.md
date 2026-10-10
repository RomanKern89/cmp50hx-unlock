# NVIDIA 610.43.03 shared-object audit and RT conclusion

Date: 2026-08-30.

This document is the short, complete map of the NVIDIA user-space libraries
checked during the CMP 50HX unlock work. It keeps static IDA facts, live GPU
facts, mock tests, load-only checks, compile-only tests, and open ideas
separate.

The result is not good for physical RT execution. We found and removed two
real performance limits, opened all host RT exposure gates, and mapped the
host compiler path through the final-cubin packaging boundary. We did not find
a CMP50-specific user-space branch that turns RT instructions off. The final
SM75 SASS emitter is still not exposed. The first real ray instruction is
still rejected by the SM decoder as `INVALID_OPCODE`.

## Evidence classes

- **Static:** exact file hash, IDA xrefs, disassembly, types, comments, and
  saved IDB readback. This proves code and data flow, not live card behavior.
- **Live:** a result measured on CMP 50HX `10de:1e09` with driver `610.43.03`.
- **Mock-only:** helper code tested with a fake ELF library. No NVIDIA process
  and no GPU work were used.
- **Load-only:** the exact NVIDIA DSO was loaded and process memory was read,
  but no NVIDIA export, GPU object, command buffer, or GPU work was used.
- **Compile-only:** a Vulkan pipeline and cache were created, but no command
  buffer was created or submitted.
- **Open:** a test idea or an indirect edge that has not been proved.

All addresses below are image-relative and valid only for the listed hashes.

## Exact library set

| File | Bytes | SHA-256 | Saved IDB bytes |
|---|---:|---|---:|
| `libnvidia-eglcore.so.610.43.03` | `39,091,248` | `35517c07dc35c1d966f7c8102deca9cd1f4925f689f95b9ffacbfada3ef6e8f8` | `277,671,043` |
| `libnvidia-glcore.so.610.43.03` | `41,760,584` | `6304dff2ee2e1ced2febf1e8b720f16c3cfdb698d4c8cc20f340bd1f09556687` | `297,295,573` |
| `libnvidia-glvkspirv.so.610.43.03` | `10,399,824` | `ebe38ab3d407ce6a71237006d6d2a9fc0420374700d0a0070a00ee828db5e899` | `94,618,314` |
| `libnvidia-rtcore.so.610.43.03` | `44,877,472` | `df846603db891087ff12ae412a1698fdd16094683bf93ec4a139f8d087e82b7d` | `284,192,113` |
| `libnvidia-gpucomp.so.610.43.03` | `110,902,328` | `4c16539192951d9b37c7db7e276560ad4f0a26d9d6c62e2004f0f563a418a8ec` | `898,683,228` |

Every saved IDB has evidence-based names, types, or comments. The main RTCORE
and GPUCOMP changes were force-recompiled, read back, and saved. Local and
remote RTCORE/GPUCOMP file hashes matched before the analysis.

## Mapped host-side flow

```text
Vulkan/EGL state and capability work
    libnvidia-eglcore.so
        |
        +-- NVVM IR -> NVuc container
        |      libnvidia-glvkspirv.so
        |
        +-- RTCORE export table
               libnvidia-rtcore.so
                    |
                    +-- mode 0: built-in RTCORE compiler
                    |
                    +-- mode 1: RTCORECP v3
                           libnvidia-gpucomp.so
                               -> final.cubin stage

OpenGL command work is a separate path in libnvidia-glcore.so.
```

The flow reaches RT-specific compile, lower, serialization, and final-cubin
work. It is not a map of the physical SM decoder.

## `libnvidia-eglcore.so.610.43.03`

### Real pipeline-bind throttle

Only two instructions emit `NVC597_CALL_MME_MACRO(52)` with argument `0xf0`:

| Instruction | Argument DWORD | Result |
|---:|---:|---|
| `0xad5f6a` | `0xad5f6c` | capability-gated path; not reached by the live probe |
| `0xc6500e` | `0xc65010` | reached from `update_pipeline_state_and_emit_mme` at `0xc67040`; live Vulkan bind throttle |

The private-copy patch changes both arguments to `0`. On CMP50HX P8, 100
binds changed from `45,234,208` GPU ticks to `88,032` ticks, about `514x`
less. The final no-`LD_LIBRARY_PATH` package measured `88,096` ticks. The
render matched the control byte-for-byte, the system library stayed unchanged,
and no new NVRM, Xid, AER, or PCIe error appeared.

This is a completed user-space performance unlock. It is not an RT-core
unlock. The other checked `0x2001xxxx` writes are normal state setup, masks,
or count limits. No other MME slot, `WAIT_FOR_IDLE`, or `PIPE_NOP` throttle was
proved.

### RT exposure and compiler gates

The RT extension and feature records use the reported RT-core count together
with profile `0x0e3595`. After the host count override, all live CMP50
conditions are true. The related compiler-input bit defaults to on. Therefore
the user-space Vulkan gate is open; it is not the reason for the decoder fault.

`VK_NV_ray_tracing_validation` is controlled by a separate off-by-default
field and `NV_ALLOW_RAYTRACING_VALIDATION=1`. That field adds validation work;
it does not enable RT hardware. RT task thresholds `4096` and `8192` select
CPU scheduling bands, not RT instruction support.

Result: EGLCORE had one real bind delay and several already-open RT gates. It
has no proved CMP50 RT reject branch.

## `libnvidia-glcore.so.610.43.03`

The OpenGL-side file also has two literal `0x20010e68, 0xf0` pairs, at
instruction starts `0xb2085b` and `0xdcbb6e`. Earlier live A/B tests changed
both arguments and did not change TU102 bind behavior. This closes a direct
copy of the EGLCORE patch as a useful GL fix.

The active path also builds typed 24-byte `GraphicsCommandDescriptor` records
and sends method words through indirect tables. BVH helpers and RT-related
data are positive work, not a product deny branch. The literal pair does not
describe the full dynamic path.

Result: no second useful throttle and no supported CMP/RT reject were found.
Open edges remain in indirect descriptor producers and dispatch tables, but
there is no evidence that changing them would make the SM accept RT opcodes.

## `libnvidia-glvkspirv.so.610.43.03`

`compileNvvmIrToNvucContainer_inferred` at `0xa1800` owns the exact phases
`NVVMIRToUCode`, `GLV Convert NVVM IR To Ucode`, and
`Container Serialization`. Its only code caller is exported `_nv002nvvm` at
`0xa2800`.

The saved IDA pass added evidence comments and read back the two internal
guards: `0xa1817` compares `pContext +0x200` with `3`, which is an internal
mode/state dispatch, and `0xa1872` checks `pState +0x115` before an early
output-fill/clear path. Neither is proved to be a GPU target, CMP, or RT
legality test.

The mapped writer group is:

| Address | Saved IDB name | Role |
|---:|---|---|
| `0x6a1730` | `createNvucContainerWriter_inferred` | allocate writer and index |
| `0x6a10b0` | `NvucContainerWriter_ctor_inferred` | initialize writer and vtable |
| `0x6a4b10` | `NvucContainerWriter_serialize_inferred` | return serialized pointer and size |
| `0x6a0b20` | `NvucContainerWriter_dtor_inferred` | release owned state |
| `0x6a0cb0` | `NvucContainerWriter_deletingDtor_inferred` | destroy and free |
| `0x9588c0` | `g_NvucContainerWriterVtable_inferred` | typed vtable |

CUDA `nvdisasm -b SM75` rejects the captured NVuc blocks because they are
compiler ucode containers, not final SASS. They cannot be put directly at the
live SM fault address.

Result: this file proves the NVVM-to-NVuc boundary. It does not contain a
proved CMP50 reject, SM75-vs-SM80 selector, RT-opcode legality test, or SASS
encoder, and it does not expose final SM75 RT instruction bytes. The next
software boundary is downstream GPUCOMP/RTCORE after NVuc serialization.

## `libnvidia-rtcore.so.610.43.03`

### Positive RT work

The RTCORE export table is `720` bytes with `89` function pointers. The mapped
work includes compile slot `+0x60`, serialization slot `+0x1c0`, state-3 RT
lowering, state-4 final package work, and exact closest-hit/any-hit dispatch
module names.

The `nv.rt.*` registry, type bits `36/37`, TTU type `37`, and internal builder
IDs `0x1499`, `0x149b`, and `0x149c` keep RT work alive. They are internal IR
values, not final SASS opcodes. Patching them out would remove required RT
input.

The global RT data map found `447` RT-prefix blocks and `174` referenced
functions. The direct state-3 group and selected consumers were checked. No
mapped branch used CMP, mining, PCI ID `0x1e09`, product name, or RT fuse to
drop the RT work.

### SM75 target profile

SM75 and SM80 have the same kind, LTO, variant `a/f` flags, and all checked
profile values except `+0x68/+0x6c`: SM75 has `16/32`, SM80 has `32/64`. The
direct profile consumers use names, suffixes, compute-profile links, and
compatibility sets. No direct RT consumer of the two different fields was
found. They are not a supported RT-unlock patch.

### Internal or external compiler selector

The backend selector is the 32-bit `current_value` field at `+0x54` in the
`0x60`-byte option descriptor at `0x2ad9f80`; its exact address is
`0x2ad9fd4`. The ELF loader first zero-fills `.bss`, but constructor `0xc0e80`
then supplies `-1` to descriptor builder `0x1242a0`. The builder copies that
value to `default_value +0x50` and `current_value +0x54`. A safe process which
only called `dlopen` on the exact RTCORE library confirmed `0xffffffff` after
constructors. Eleven sites read the field. There is no direct address write;
the constructor write is indirect through the descriptor pointer.

- `-1`: automatic choice; use external only when config `+0xbc` is `1`.
- `0`: always use the built-in compiler.
- `1`: always use external GPUCOMP RTCORECP.
- other: the machine code uses the same config-selected rule as `-1`.

This selector is process-wide and controls a family of compile and
serialization paths. Changing one branch would be wrong.

Result: RTCORE keeps RT work and has a real alternate compiler route. It has
no proved CMP50 reject. The later live test proved that forced mode `1` emits
the same saved cache and metadata as automatic mode for the tested ray query.

## `libnvidia-gpucomp.so.610.43.03`

`nvGetCompilerInterface` accepts RTCORECP id `0x5254434f52454350`, version
`{3,0}`. The corrected interface has `23` non-null slots through `+0xb0` and is
`184` bytes. Data at `+0xc8` is another object, not slot 23.

External compile slot `+0x18` reaches:

```text
rtcoreCompileWithInterfaceV3_inferred
  -> runGpucompRtcoreCompilePipeline_inferred
  -> initializeGpucompRtcoreCompileState_inferred
  -> runGpucompRtcoreCompileStateMachine_inferred
  -> runGpucompRtcoreCompileStateStep_inferred
```

The state machine advances through setup, entry-function work, the large state
3 compiler stage, and state 4. State 4 owns exact strings `beforePIC.ll`,
`afterPIC.ll`, `afterPICCleanup.ll`, `final.cubin`, and `rtx`. This proves that
the external route is real, not a stub.

The mapped route contains no direct CMP, PCI, product, mining, or fuse query.

Result: GPUCOMP is a real compiler route, but forcing it produced no saved
pipeline-cache or metadata difference. The cache-based test is closed; a final
SASS difference after the NVuc cache boundary is still not observable here.

### GPUCOMP target-profile and register-layout pass

A second bounded IDA pass followed the target-dependent code reached by
GPUCOMP state 3. It found a generic profile-name mapper and a generic register
layout builder, but no device-policy branch:

| Address | Saved IDB name | Evidence | Result |
|---:|---|---|---|
| `0x495b80` | `initGpuTargetProfileNameTable_inferred` | Static table contains `sm_20` through modern `sm_121f` variants, including `sm_75`. | Data registration only; no allow/deny branch. |
| `0x17d4d60` | `mapTargetCodeToSmProfile_inferred` | Target code `0x4b` maps to `sm_75`; nearby codes map to other generic `sm_*` variants. | Profile naming only; no CMP, PCI, product, fuse, or RT-opcode test. |
| `0x17d50c0` | `selectTargetProfileName_inferred` | Virtual target metadata selects the generic profile mapper for code `0xbe`. | Selector/formatter only; no proved hardware gate. |
| `0x17d46e0` | `mapElfMachineToTargetProfile_inferred` | ELF machine/profile identifiers map to generic ELF and GPU profile names. | ELF/profile translation only. |
| `0x659260` | `buildGpucompTargetRegisterLayout_inferred` | State 3 calls this builder. `targetKind` cases `0..7` and feature words `0x160`, `0x180`, `0x1a0`, `0x1c0`, `352`, `368`, and `448` derive layout values such as `34`, `36`, and `40`. | Numeric layout arithmetic; no CMP/product/fuse query. |
| `0x65b4f0` / `0x6545d0` | `deriveTargetRegisterCount_inferred` / `lookupTargetRegisterOffset_inferred` | The helper pair performs the same target-kind and feature-word switch work. | Register/layout lookup only; no RT instruction-legality branch. |

The state-3 call sites pass compiler target kind, feature data, profile/context
objects, and size limits into the layout builder. The two `+0x8` calls
(`0x6c9f36`, `0x6cae39`) resolve through
`g_gpucompTemporaryObjectVtable_inferred` to
`freeGpucompTemporaryObject_inferred`, which only calls `free`. The `+0x10`
call at `0x6ca156` resolves through
`g_gpucompBufferContextVtable_inferred` to `0x623e80`, a generic context/buffer
method that performs IR bookkeeping and an optional callback. None of these
edges is a final SASS emitter. This pass therefore closes another tempting theory:
the presence of `sm_75` and target-specific layout numbers is not evidence of a
hidden CMP50HX RT disable switch. The first target-specific machine operation
and final SM75 encoder remain beyond the mapped NVuc/final-cubin boundary. The
state-3 object initializes the callback-enable byte checked by `0x623e80` to
zero at both constructions, so that optional callback is not taken on the
mapped path; other generic contexts remain outside this bound.

### State 4: final-cubin package boundary

The exact state-4 reference to `final.cubin` is at `0x6bee6c` and has one data
xref in the mapped function. The nearby call to `0x6ab200`
(`constructStringFromLiteral_inferred`) only builds the C++ filename string.
That string then reaches `0x623a80` (`writeFileBuffer_inferred`), which calls
`fopen("wb")`, `fwrite`, and `fclose`. The surrounding `0x65e8a0`
(`assembleGpucompOutputMetadata_inferred`) assembles metadata and delegates to
`0x621670`; `0x790af0` is a generic IR/deque/text-processing helper. This is a
real output/package path, but it is not proof that final SM75 SASS is emitted in
this mapped slice. The first target-specific machine operation and final encoder
remain outside the present evidence.

The deeper RTCORE state-4 readback makes this boundary more precise. The emitter
`0x164620` calls `0x12d7e0` with generated buffers, relocation/context objects,
and callbacks. Its nearby calls at `0x165334` and `0x165359` are section helpers;
the named helper `0x226440` (`rewriteNvConstantSectionWithNumericSuffix_inferred`)
walks `.nv.constant`, parses a decimal suffix, updates section metadata, and
formats the suffix back. The later call at `0x1666a8` is output/package handling
after the `final.cubin` label. This is concrete ELF/Mercury metadata and
container work, not a fixed RT instruction template or a proved SM75 encoder.
The entire RTCORE DSO also has no exact match for the four generated SM75
ray-query words or their recorded 64-byte sequence.

### GPUCOMP finalizer: real error surface, but no CMP switch

The final-cubin state calls a large ELF/Mercury finalizer core at
`0x2563fd0`, now named `runGpucompElfFinalizerCore_inferred`. Two ELF wrappers
(`0x2555e60` and `0x2556320`) pass a source machine value, a requested target,
options, and the input ELF to this core. The core has a real architecture
compatibility helper at `0x2558350`, named
`checkFinalizerFastpathCompatibility_inferred`.

The most important negative result is also the most tempting false lead. When
the source and requested target satisfy the helper's capability rules, the
off-target fastpath at `0x2564bf2` prints
`[Finalizer] fastpath optimization applied for off-target %u -> %u
finalization`, copies the input ELF, and changes only its ELF machine field.
That is an ELF retag/copy path. It does not show instruction translation and
cannot make a SM75 decoder accept an SM80-only RT instruction.

The finalizer has generic failure codes for `unsupported instruction` (14),
`SASS generation failed` (16), and `unsupported SM version` (22). The mapper
at `0x479d520` (`mapMercuryFinalizerErrorCode_inferred`) turns those codes into
diagnostic strings. The `.nv.merc*` section parser at `0x479d850`
(`buildFinalizerElfSectionMetadata_inferred`) only builds ELF/Mercury section,
symbol, and relocation metadata. No mapped finalizer function reads PCI ID,
product name, mining state, RT fuse, or CMP identity. The `CAN_FINALIZE_DEBUG`
reader at `0x2556f10` is called at `0x2558364`, but its return value is
discarded in this build; it is not a device policy switch.

One deeper finalizer candidate is the constructor at `0x255ed80`, now named
`initGpucompFinalizerHashMaps_inferred`. It creates two independent hash maps
in the finalizer object (`+0x08` and `+0x30`) and populates them with `284` and
`295` entries. The keys are four-byte values. The insertion path at `0x255ead0`
is a generic FNV-1a table insert; `0x255ed50` only returns the newly allocated
value slot. The constructor is called from the finalizer core at `0x2565f88`
and from the generic runtime setup at `0x310e605`. Some values are constants and
some are pointers into code or exception metadata, but no consumer proof yet
identifies them as SASS opcodes. No CMP, PCI, product, mining, fuse, target-SM,
or RT-legality decision is present in this constructor or its generic insert
helper. It is therefore recorded as a registry/codec-table candidate, not as
an unlock switch or an RT encoder.

This makes the software picture worse, not better: a genuine finalizer and
generic SM/instruction error surface exist, but the visible off-target route
is a header retag and the actual final SASS encoder is still outside the
proved slice. A universal CMP50HX patch cannot be justified from this code.

## Process-local backend selector helper

`experiments/cmp50-rt-unlock-candidate-audit/backend-selector-preload` now has
a strict `LD_PRELOAD` helper. It needs no `LD_LIBRARY_PATH` and changes no
system file. The runner checks the exact RTCORE SHA-256. Loaded code checks
the exact SONAME, file size, GNU build ID, file device/inode, writable ELF
segment, exact old value `0xffffffff` (`-1`), atomic write to `1`, and
readback.

On host `192.168.1.224`, kernel `6.8.0-138-generic`, GCC `13.3.0`, the
production helper built as a `28,960`-byte `.so` with SHA-256
`6eace68fcdd0469704be50f559b0d9f322f2be3da4c986beeb288c72196513f3`.
The test result was:

```text
PASS: no opt-in, exact -1 -> 1 patch, old-value refusal, inode refusal, build-id refusal, shell syntax
```

The first load-only build still required old value `0`. It correctly observed
the real constructor-set value and refused:

```text
CMP50_RTCORE_BACKEND status=REFUSED reason=old_value offset=0x2ad9fd4 old=4294967295 new=4294967295
```

After changing only the required old value to `-1`, the exact load-only process
reported:

```text
CMP50_RTCORE_BACKEND status=PATCHED reason=ok offset=0x2ad9fd4 old=4294967295 new=1 size=44877472 build_id=0c67b62bd9e204afb9f878cf953d10c2d0889b30
```

It returned `0`, added zero kernel-log lines, and called no NVIDIA export. It
created no Vulkan object, command buffer, or GPU work.

The same helper then ran two forced-external compile-only pipeline builds,
interleaved with two automatic-mode builds. All four runs returned `0`, added
zero kernel-log lines, and created no command buffer. Their complete
`pipeline-info.json` files were byte-identical, SHA-256
`6ba6371070fa765c540270520824dcee1c89d978d0ff7bf70e342a91a4e0111c`.
Their complete 10,099-byte pipeline caches were byte-identical, SHA-256
`45a6fcc782126503e040925451b6bcdce71a7f44189e3aa3a67b63e2e62dcfb7`.
The compiler metadata stayed at 33 registers and 6,144 binary bytes.

No uprobe or debugger trace of GPUCOMP `0x61d870` was taken. Static control
flow proves mode `1` selects the external interface, and successful pipeline
creation is consistent with that route. The equal cache bytes cannot tell
whether automatic mode already used GPUCOMP or a difference exists only after
the NVuc cache boundary. The test therefore gives strong negative evidence,
not proof that final SM75 SASS is equal.

## Cross-library result

The five files do not show a common CMP50 branch that removes RT instructions.
Instead they show a complete positive RT path:

1. EGLCORE exposes RT after the host count gate.
2. GLVKSPIRV keeps NVVM input and writes NVuc containers.
3. RTCORE registers and lowers RT intrinsics.
4. RTCORE can use either its built-in compiler or GPUCOMP.
5. GPUCOMP reaches a real `final.cubin` stage.
6. Its finalizer reports generic unsupported-SM/instruction/SASS errors, but
   the visible off-target fastpath only retags an ELF header; it is not a code
   translator or RT unlock.

The live card still stops at the first ray-query machine-code group with
SM `WARP_ESR=9`, `INVALID_OPCODE`. The faulting address repeats across reboot.
The later FECS timeout and Xid 109 are effects of that first decoder fault, not
its cause. Hiding the Xid cannot make the instruction valid.

Host RT count, EGLCORE extension gates, compiler option bits, task scheduling,
RTCORE intrinsic registries, target-profile records, and the mapped GSP/FECS/
GPCCS/netlist state have all failed to give a writable RT enable switch.

## Candid conclusion

The good result is real but limited:

- full SM and Tensor issue rate is unlocked;
- the EGLCORE Vulkan bind delay is removed;
- 16 GiB BAR1 and PCIe Gen2 are working;
- the host reports 56 RT cores and creates RT objects.

The main RT goal is not reached. No real RT instruction executes.

The best present explanation is a physical RT floorsweep fuse or decoder state
sampled below every writable software path we reached. This is not direct
electrical proof, but it fits the repeated decode fault and every negative
software result better than a hidden `.so` product branch.

The last narrow user-space test is now complete. Automatic mode `-1` and forced
external mode `1` produced the same full cache and metadata bytes twice each.
The forced writes were logged and read back. This closes the observable
cache-based comparison and gives no evidence that this selector is an unlock.
It does not close a hidden difference at the still-unmapped final SASS encoder.

The finalizer map does not improve this outlook. It confirms that the DSO can
reject an unsupported instruction or SM in a generic compile error, while the
only visible off-target success path copies and retags the ELF. There is no
supported evidence that changing this path would emit legal SM75 RT code.

More broad searching in the same five libraries is now a poor use of time
without new evidence. We mapped positive RT routing, two real performance
limits, the host count gate, the internal and external compiler routes, and the
final-cubin package boundary. We found no product branch that explains the
decoder fault. Stronger next evidence would need one of:

- the same capture from an RTX 2080/2080 Ti on exact driver `610.43.03`;
- direct and safe physical fuse-state evidence;
- a second CMP50HX sample with a different fuse state;
- a final SASS difference tied to the faulting instruction group;
- direct proof that the physical fuse or reset-latched decoder state differs.

A single universal RT patch for all CMP cards is not supported by this work.
Software throttles can share patterns; physical floorsweep state is per die and
per product. Software RT emulation on CUDA cores may make an application work,
but it is not an unlock of the physical RT cores.

## Software RT-unlock stop decision — 2026-08-30

This investigation is now closed for software changes on the exact
CMP50HX/TU102 card and NVIDIA `610.43.03` build. The five copied NVIDIA DSOs,
their saved IDBs, the GSP/FECS/netlist path, and the safe compile-only backend
experiment were checked. They show positive RT routing and generic compiler
machinery, but no supported CMP/product/fuse/RT-decoder switch and no proven
translator that can turn the failing code into legal SM75 RT instructions.

The remaining uncertainty is outside the proved software boundary: the final
SASS selector/encoder, upload or relocation details, and the physical decoder
or fuse state. That uncertainty is not a reason to keep patching these DSOs.
Further work should resume only with new external evidence such as a final-SASS
difference, a second card, or direct fuse/decoder evidence. No more broad
`.so` searches, speculative byte patches, or RT dispatch attempts are part of
this research record.

## Research closure matrix

| Area | What the evidence established | Status |
|---|---|---|
| SM/Tensor issue rate | The stockflow path reaches full issue rate and Tensor work. | Achieved. |
| EGLCORE Vulkan path | The two `NVC597_CALL_MME_MACRO(52)` argument writes were the real bind delay; the private copy removes it with byte-identical rendering. | Achieved performance fix. |
| BAR1 and PCIe | The 16 GiB BAR1 path and the board-scoped PCIe Gen2 path survive reboot and pass their checks. | Achieved. |
| RT API exposure | The host can report 56 RT cores and create RT objects, BLAS/TLAS, and a pipeline. | API only. |
| GSP, FECS, netlist, Booter, VBIOS | The RT fuse read is count-only; the checked firmware, golden-context, signed-Booter, and VBIOS paths expose no decoder enable. | No software gate found. |
| Five NVIDIA DSOs | `eglcore`, `glcore`, `glvkspirv`, `rtcore`, and `gpucomp` retain positive RT/compiler work but no mapped CMP/PCI/product/mining/fuse reject. | User-space reject theory not supported. |
| RTCORE backend selector | Automatic `-1` and forced external `1` produce identical compile-only cache and metadata twice each. | Observable candidate closed. |
| GPUCOMP finalizer | Generic unsupported-instruction/SM errors exist; the visible off-target success path only retags an ELF header. | Not an unlock or translator. |
| GPUCOMP finalizer hash maps | `0x255ed80` builds two generic four-byte-key maps with `579` entries through FNV-1a insertion helpers. Their consumer semantics are unresolved, with no target or CMP decision in the constructor. | Candidate registry only; not an opcode proof. |
| Physical RT execution | The first ray-query machine-code group still faults at decode with `WARP_ESR=9 INVALID_OPCODE`, repeating across reboot. | Main goal not achieved. |

## Detailed records

- [`CMP50HX.md`](CMP50HX.md) — full GSP, register, live, and package record.
- `experiments/cmp50-eglcore-capability-map` — EGLCORE class and RT gate map.
- `experiments/cmp50-rt-unlock-candidate-audit/README.md` — full candidate
  matrix and per-library IDA work.
- `experiments/cmp50-rt-unlock-candidate-audit/RESEARCH_LOG.md` — RTCORE and
  GPUCOMP backend record, completed experiment, and stop rules.
- `experiments/cmp50-rt-unlock-candidate-audit/backend-selector-preload/LOAD_ONLY_RESULT.md`
  — first refusal and corrected exact `-1 -> 1` load-only record.
- `experiments/cmp50-rt-unlock-candidate-audit/backend-selector-preload/BACKEND_COMPARISON_RESULT.md`
  — exact four-run compile-only byte comparison.
- `experiments/cmp50-rt-unlock-candidate-audit/runs/20260830T163146Z-backend-selector-compile-only`
  — all automatic and forced-external caches, metadata, logs, and snapshots.
- `experiments/cmp50-rt-unlock-candidate-audit/rtcore-static-map.json` —
  machine-readable RTCORE map.
- `experiments/cmp50-rt-unlock-candidate-audit/gpucomp-static-map.json` —
  machine-readable GPUCOMP map.
