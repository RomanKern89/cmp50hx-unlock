#!/usr/bin/env python3
"""Make a private CMP50HX Vulkan pipeline-bind library copy."""

import argparse
import os
from pathlib import Path
import stat
import sys


PATTERN = bytes.fromhex("68 0e 01 20 f0 00 00 00")


def find_matches(data: bytes) -> list[int]:
    offsets = []
    start = 0
    while True:
        offset = data.find(PATTERN, start)
        if offset < 0:
            return offsets
        offsets.append(offset)
        start = offset + 1


def parse_argument(value: str) -> int:
    try:
        number = int(value, 0)
    except ValueError as exc:
        raise ValueError("ARGUMENT must be an integer (decimal or 0x-prefixed)") from exc
    if not 0 <= number <= 0xFFFFFFFF:
        raise ValueError("ARGUMENT must fit a DWORD (0..0xffffffff)")
    return number


def patch(input_path: Path, output_path: Path, argument: int) -> list[int]:
    if not input_path.is_file():
        raise ValueError(f"input is not a regular file: {input_path}")
    if not output_path.parent.is_dir():
        raise ValueError(f"output directory does not exist: {output_path.parent}")
    if os.path.lexists(output_path):
        raise ValueError(f"output already exists (refusing to replace): {output_path}")

    original = input_path.read_bytes()
    matches = find_matches(original)
    if len(matches) != 2:
        raise ValueError(
            f"expected exactly 2 CALL_MME_MACRO constants, found {len(matches)}"
        )

    replacement = PATTERN[:4] + argument.to_bytes(4, "little")
    result = bytearray(original)
    for offset in matches:
        result[offset : offset + len(PATTERN)] = replacement

    if find_matches(result):
        raise ValueError("original command remains after patch")

    try:
        with output_path.open("xb") as output:
            output.write(result)
    except FileExistsError as exc:
        raise ValueError(f"output appeared during patch (refusing to replace): {output_path}") from exc
    os.chmod(output_path, stat.S_IMODE(input_path.stat().st_mode))
    return matches


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Patch two NVIDIA libnvidia-eglcore MME loop arguments into a private copy"
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("argument", metavar="ARGUMENT")
    args = parser.parse_args(argv)
    try:
        argument = parse_argument(args.argument)
        matches = patch(args.input, args.output, argument)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    offsets = " ".join(f"0x{offset:x}" for offset in matches)
    print(f"patched argument 0xf0 -> 0x{argument:x} at file offsets {offsets}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
