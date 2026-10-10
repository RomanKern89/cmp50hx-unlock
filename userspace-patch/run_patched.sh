#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 PATCH_DIRECTORY PROGRAM [ARGUMENT ...]" >&2
    exit 2
fi

patch_dir=$1
program=$2
shift 2

if [[ ! -d "$patch_dir" ]]; then
    echo "error: patch directory does not exist: $patch_dir" >&2
    exit 1
fi
if [[ ! -f "$program" || ! -x "$program" ]]; then
    echo "error: program is not an executable file: $program" >&2
    exit 1
fi
if ! command -v readelf >/dev/null 2>&1; then
    echo "error: readelf is required to find the ELF interpreter" >&2
    exit 1
fi

interpreter=$(readelf -lW "$program" | awk '/Requesting program interpreter:/ {gsub(/[\[\]]/, "", $NF); print $NF; exit}')
if [[ -z "$interpreter" || ! -x "$interpreter" ]]; then
    echo "error: could not find an executable ELF interpreter for: $program" >&2
    exit 1
fi

exec "$interpreter" --library-path "$patch_dir" "$program" "$@"
