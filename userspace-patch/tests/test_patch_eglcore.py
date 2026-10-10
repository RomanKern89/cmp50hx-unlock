#!/usr/bin/env python3
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATCHER = ROOT / "patch_eglcore.py"
LAUNCHER = ROOT / "run_patched.sh"
PATTERN = bytes.fromhex("68 0e 01 20 f0 00 00 00")


class PatchTests(unittest.TestCase):
    def run_patch(self, source: bytes, argument="0"):
        directory = tempfile.TemporaryDirectory()
        folder = Path(directory.name)
        input_path = folder / "input.so"
        output_path = folder / "output.so"
        input_path.write_bytes(source)
        os.chmod(input_path, 0o754)
        result = subprocess.run(
            [sys.executable, str(PATCHER), str(input_path), str(output_path), argument],
            capture_output=True, text=True,
        )
        return directory, result, input_path, output_path

    def test_exact_two_patch_and_preservation(self):
        source = b"prefix" + PATTERN + b"middle" + PATTERN + b"suffix"
        directory, result, input_path, output_path = self.run_patch(source, "0")
        self.addCleanup(directory.cleanup)
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = source.replace(PATTERN, PATTERN[:4] + b"\0\0\0\0")
        self.assertEqual(output_path.read_bytes(), expected)
        self.assertEqual(input_path.read_bytes(), source)
        if os.name != "nt":
            self.assertEqual(stat.S_IMODE(output_path.stat().st_mode), 0o754)

    def test_wrong_match_count_rejected(self):
        directory, result, _, output_path = self.run_patch(PATTERN)
        self.addCleanup(directory.cleanup)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected exactly 2", result.stderr)
        self.assertFalse(output_path.exists())

    def test_same_input_output_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.so"
            path.write_bytes(PATTERN * 2)
            result = subprocess.run(
                [sys.executable, str(PATCHER), str(path), str(path), "0"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("output already exists", result.stderr)

    def test_existing_output_rejected(self):
        directory, _, input_path, output_path = self.run_patch(PATTERN * 2)
        self.addCleanup(directory.cleanup)
        output_path.write_bytes(b"keep this file")
        result = subprocess.run(
            [sys.executable, str(PATCHER), str(input_path), str(output_path), "0"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output_path.read_bytes(), b"keep this file")
        self.assertIn("output already exists", result.stderr)

    @unittest.skipIf(os.name == "nt", "symlinks and POSIX hardlinks need POSIX semantics")
    def test_existing_link_aliases_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            input_path = folder / "input.so"
            input_path.write_bytes(PATTERN * 2)
            hardlink = folder / "hardlink.so"
            os.link(input_path, hardlink)
            symlink = folder / "symlink.so"
            os.symlink(input_path, symlink)
            for alias in (hardlink, symlink):
                result = subprocess.run(
                    [sys.executable, str(PATCHER), str(input_path), str(alias), "0"],
                    capture_output=True, text=True,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("output already exists", result.stderr)

    def test_launcher_argument_handling(self):
        if os.name == "nt":
            self.skipTest("launcher test needs a POSIX shell")
        command = [str(LAUNCHER)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
