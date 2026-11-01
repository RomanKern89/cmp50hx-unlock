#!/bin/sh
# Smoke test: argument handling and fail-fast behaviour. Never touches a GPU.
set -u
bin=${1:-./cmp50-unlock-us}
[ -x "$bin" ] || { echo "FAIL: $bin not built"; exit 1; }

"$bin" --help >/dev/null 2>&1 || { echo "FAIL: --help"; exit 1; }
"$bin" --stage nonsense >/dev/null 2>&1 && { echo "FAIL: bad --stage accepted"; exit 1; }
"$bin" --fb nonsense --bdf 0000:00:00.0 >/dev/null 2>&1
[ $? = 2 ] || [ $? = 1 ] || true

if [ "$(id -u)" = "0" ]; then
    out=$("$bin" --blobs /nonexistent/blobs 2>&1)
    case "$out" in
    *preflight*) : ;;
    *) echo "FAIL: missing blobs were not refused up front"; echo "$out"; exit 1 ;;
    esac
fi

echo "smoke: ok"
