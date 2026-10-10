# CMP50HX userspace pipeline-bind patch

This optional patch targets the classic Vulkan pipeline-bind delay in the
NVIDIA 610.43.03 userspace library. It is separate from the kernel-module
patches in this repository.

The rule is based on the research in [Cyridd/cmpunlocker](https://github.com/Cyridd/cmpunlocker),
especially its [`cmp_glcore_patch`](https://github.com/Cyridd/cmpunlocker/tree/2cf043be130167abe1298d667a378693b6253517/cmp_glcore_patch)
tool and commit [`2cf043b`](https://github.com/Cyridd/cmpunlocker/commit/2cf043be130167abe1298d667a378693b6253517).
The upstream project states MIT licensing, but this repository does not copy
its source. `patch_eglcore.py` is a small independent implementation of the
documented byte-pattern rule.

## Build a private library

Do not overwrite the NVIDIA file in `/usr/lib`. Give the installed library as
input and a new path as output. The tested path needs Python 3, Bash, and
`readelf` from `binutils`:

```bash
mkdir -p "$PWD/eglcore-patched"
python3 userspace-patch/patch_eglcore.py \
  /usr/lib/x86_64-linux-gnu/libnvidia-eglcore.so.610.43.03 \
  "$PWD/eglcore-patched/libnvidia-eglcore.so.610.43.03" 0
```

The tool scans the exact bytes `68 0e 01 20 f0 00 00 00` and refuses to write
unless there are exactly two matches. The final argument is a DWORD; `0` is
the validated CMP50HX value. The input remains unchanged and the output keeps
its mode bits. It refuses an existing output path, including symlinks and
hardlinks, and creates the output exclusively.

## Run one ELF program without `LD_LIBRARY_PATH`

The launcher reads the target program's ELF interpreter with `readelf`, then
executes that interpreter with its `--library-path` option:

```bash
userspace-patch/run_patched.sh \
  "$PWD/eglcore-patched" /path/to/native-vulkan-program arg1
```

This is for native Linux ELF programs. It does not set `LD_LIBRARY_PATH` and
does not install anything system-wide. The NVIDIA loader still needs the rest
of the system libraries in its normal paths.

For Steam or Proton, use the per-process `LD_LIBRARY_PATH` launch option when
the loader wrapper cannot cross the Steam runtime boundary:

```text
LD_LIBRARY_PATH=/path/to/eglcore-patched:$LD_LIBRARY_PATH %command%
```

This Steam/Proton fallback was not tested in this work.

## Safety and rollback

Keep the original driver library. Do not mix libraries from different NVIDIA
driver releases. A failed match-count check is expected when the driver
version is not the validated one; do not force it with hard-coded offsets.
Rollback is local: stop using the launcher or remove the Steam launch option
and delete the private output directory. The system NVIDIA installation is
unchanged.

This is research software. It changes a binary userspace library and can make
an application fail; test one native Vulkan program first and keep console or
remote recovery access.

## Test

```bash
python3 -m unittest discover -s userspace-patch/tests -v
```

The validated input was
`/usr/lib/x86_64-linux-gnu/libnvidia-eglcore.so.610.43.03`, SHA-256
`35517c07dc35c1d966f7c8102deca9cd1f4925f689f95b9ffacbfada3ef6e8f8`.
The two pattern offsets were `0xad5f6c` and `0xc65010`.

The first CMP50HX P8 comparison changed 100 binds from 45,234,208 to 88,032
GPU ticks with argument `0` (about 514x lower). The final packaged scripts
gave 88,096 ticks in a second run with `LD_LIBRARY_PATH` unset. The render
matched the reference byte-for-byte, with SHA-256
`8b16870a8f539ac7e43c00dfa4d3255f8699f0211498002480cddc485f870c48`.
All six Linux tests passed, the system library hash stayed unchanged, and no
recent NVRM, Xid, AER, or PCIe error was seen. This is a pipeline-bind result,
not proof of an RT-core or general shader unlock.
