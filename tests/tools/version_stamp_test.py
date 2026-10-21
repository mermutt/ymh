#!/usr/bin/env python3
"""Spec 96 build-stamp tests (T1-T9, T11-T17).

Every test drives `cmake/version_stamp.cmake` in `cmake -P` script mode against
throwaway directories with `YMH_VERSION_DATE` pinned, so nothing depends on the
host clock, locale, or a configured project. T13/T14 configure tiny probe
projects; T10 is a gtest in the C++ suite.
"""

from __future__ import annotations

import argparse
import os
import re
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "cmake" / "version_stamp.cmake"
HEADER_TEMPLATE = ROOT / "cmake" / "build_version.hpp.in"
MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
STAMP_RE = re.compile(r"^([0-9]{2})-([A-Z][a-z]{2})-([0-9]{4}):#([0-9]+)$")


class VersionStampTest(unittest.TestCase):
    cmake: str = "cmake"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="ymh_version_")
        self.dir = Path(self._tmp.name)
        self._out_tmp = tempfile.TemporaryDirectory(prefix="ymh_version_out_")
        self.out = Path(self._out_tmp.name) / "display.txt"

    def tearDown(self) -> None:
        self._out_tmp.cleanup()
        self._tmp.cleanup()

    def run_stamp(self, date="20-10-2026", version_file=None, out_file=None):
        version_file = Path(version_file) if version_file else self.dir / ".version_info"
        out_file = Path(out_file) if out_file else self.out
        if out_file.exists():
            out_file.unlink()
        proc = subprocess.run(
            [
                self.cmake,
                f"-DYMH_VERSION_FILE={version_file}",
                f"-DYMH_VERSION_DATE={date}",
                f"-DYMH_VERSION_OUT={out_file}",
                "-P",
                str(SCRIPT),
            ],
            capture_output=True,
            text=True,
        )
        display = out_file.read_text(encoding="utf-8").strip() if out_file.exists() else ""
        return proc, display

    def assert_stamp(self, version_file, expected_line):
        self.assertEqual(version_file.read_text(encoding="utf-8").strip(), expected_line)

    def test_t1_creation(self):
        vf = self.dir / ".version_info"
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assert_stamp(vf, "20-10-2026:01")

    def test_t2_increment_three_runs(self):
        vf = self.dir / ".version_info"
        for expected in ("20-Oct-2026:#01", "20-Oct-2026:#02", "20-Oct-2026:#03"):
            proc, display = self.run_stamp()
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(display, expected)
        self.assert_stamp(vf, "20-10-2026:03")

    def test_t3_rollover(self):
        vf = self.dir / ".version_info"
        vf.write_text("19-10-2026:07\n", encoding="utf-8")
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assert_stamp(vf, "20-10-2026:01")

    def test_t4_malformed_and_empty(self):
        vf = self.dir / ".version_info"
        for body in ("", "garbage\n", "20-10-2026", ":5", "2026-10-20:05"):
            with self.subTest(body=body):
                vf.write_text(body, encoding="utf-8")
                proc, display = self.run_stamp()
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(display, "20-Oct-2026:#01")
                self.assert_stamp(vf, "20-10-2026:01")

    def test_t5_padding(self):
        vf = self.dir / ".version_info"
        vf.write_text("20-10-2026:9\n", encoding="utf-8")
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#10")
        self.assert_stamp(vf, "20-10-2026:10")

        vf.write_text("20-10-2026:99\n", encoding="utf-8")
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#100")
        self.assert_stamp(vf, "20-10-2026:100")

    def test_t6_lenient_parse(self):
        vf = self.dir / ".version_info"
        for body in ("20-10-2026:7", " 20-10-2026:07\n"):
            with self.subTest(body=body):
                vf.write_text(body, encoding="utf-8")
                proc, display = self.run_stamp()
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(display, "20-Oct-2026:#08")
                self.assert_stamp(vf, "20-10-2026:08")

    def test_t7_script_mode(self):
        self.assertFalse((self.dir / "CMakeLists.txt").exists())
        vf = self.dir / ".version_info"
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assertTrue(vf.is_file())
        self.assertNotIn("ymh: build version", proc.stdout)

    def test_t8_month_table(self):
        vf = self.dir / ".version_info"
        for index, month in enumerate(MONTHS, start=1):
            with self.subTest(month=month):
                if vf.exists():
                    vf.unlink()
                proc, display = self.run_stamp(date=f"20-{index:02d}-2026")
                self.assertEqual(proc.returncode, 0, proc.stderr)
                self.assertEqual(display, f"20-{month}-2026:#01")
                self.assert_stamp(vf, f"20-{index:02d}-2026:01")

    def test_t9_read_only_directory(self):
        if os.geteuid() == 0:
            self.skipTest("root ignores directory permissions")
        ro = self.dir / "ro"
        ro.mkdir()
        out = Path(self._out_tmp.name) / "readonly.txt"
        ro.chmod(0o555)
        try:
            proc, display = self.run_stamp(version_file=ro / ".version_info", out_file=out)
        finally:
            ro.chmod(0o755)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assertFalse((ro / ".version_info").exists())
        self.assertIn("ymh:", proc.stderr)

    def test_t11_single_version_source(self):
        sources = [ROOT / "CMakeLists.txt", *sorted((ROOT / "cmake").glob("*.cmake"))]
        for path in sources:
            text = path.read_text(encoding="utf-8")
            with self.subTest(path=str(path)):
                self.assertNotIn('YMH_VERSION="${PROJECT_VERSION}"', text)
                self.assertIsNone(
                    re.search(r"target_compile_definitions\([^)]*YMH_VERSION\s*=", text),
                    f"YMH_VERSION compile definition remains in {path}",
                )

    def test_t12_gitignore(self):
        probe = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--is-inside-work-tree"],
            capture_output=True,
            text=True,
        )
        if probe.returncode != 0 or probe.stdout.strip() != "true":
            self.skipTest("not inside a git work tree")
        for name in (".version_info", ".version_info.tmp-abc123"):
            proc = subprocess.run(
                ["git", "-C", str(ROOT), "check-ignore", name],
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, f"{name} is not gitignored")

    def _probe_project(self, body: str) -> tuple[Path, Path]:
        source = self.dir / "probe-src"
        build = self.dir / "probe-build"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(body, encoding="utf-8")
        return source, build

    def _configure_probe(self, source: Path, build: Path):
        return subprocess.run(
            [
                self.cmake,
                "-S",
                str(source),
                "-B",
                str(build),
                f"-DYMH_STAMP_MODULE={SCRIPT}",
                f"-DYMH_HEADER_TEMPLATE={HEADER_TEMPLATE}",
                "-DYMH_VERSION_DATE=20-10-2026",
            ],
            capture_output=True,
            text=True,
        )

    def test_t13_plain_build_does_not_bump(self):
        source, build = self._probe_project(
            'cmake_minimum_required(VERSION 3.22)\n'
            'include("${YMH_STAMP_MODULE}")\n'
            'ymh_stamp_version("${CMAKE_CURRENT_SOURCE_DIR}/.version_info" YMH_DISPLAY_VERSION)\n'
            "project(ymh_stamp_probe NONE)\n"
        )
        proc = self._configure_probe(source, build)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        stamp = source / ".version_info"
        before = stamp.read_text(encoding="utf-8")
        self.assertEqual(before.strip(), "20-10-2026:01")
        for _ in range(2):
            built = subprocess.run(
                [self.cmake, "--build", str(build)], capture_output=True, text=True
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(stamp.read_text(encoding="utf-8"), before)

    def test_t14_generated_header_matches_status(self):
        source, build = self._probe_project(
            'cmake_minimum_required(VERSION 3.22)\n'
            'include("${YMH_STAMP_MODULE}")\n'
            'ymh_stamp_version("${CMAKE_CURRENT_SOURCE_DIR}/.version_info" YMH_DISPLAY_VERSION)\n'
            "project(ymh_stamp_probe NONE)\n"
            'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/ymh")\n'
            'configure_file("${YMH_HEADER_TEMPLATE}"\n'
            '               "${CMAKE_BINARY_DIR}/generated/ymh/build_version.hpp" @ONLY)\n'
        )
        proc = self._configure_probe(source, build)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("ymh: build version 20-Oct-2026:#01 (from .version_info)", proc.stdout)
        header = build / "generated" / "ymh" / "build_version.hpp"
        self.assertTrue(header.is_file())
        match = re.search(r'#define YMH_VERSION "([^"]+)"', header.read_text(encoding="utf-8"))
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), "20-Oct-2026:#01")

    def test_t15_directory_at_stamp_path(self):
        vf = self.dir / ".version_info"
        vf.mkdir()
        proc, display = self.run_stamp()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assertIn("ymh:", proc.stderr)
        self.assertTrue(vf.is_dir())

    def test_t16_unreadable_file_still_replaced(self):
        if os.geteuid() == 0:
            self.skipTest("root can read a 000 file")
        vf = self.dir / ".version_info"
        vf.write_text("20-10-2026:41\n", encoding="utf-8")
        vf.chmod(0)
        try:
            proc, display = self.run_stamp()
        finally:
            vf.chmod(stat.S_IRUSR | stat.S_IWUSR)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(display, "20-Oct-2026:#01")
        self.assert_stamp(vf, "20-10-2026:01")

    def test_t17_malformed_override_ignored(self):
        vf = self.dir / ".version_info"
        proc, display = self.run_stamp(date="1-2-2026")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        local_date = time.strftime("%d-%m-%Y")
        expected_mon = MONTHS[int(local_date[3:5]) - 1]
        self.assertEqual(
            display, f"{local_date[0:2]}-{expected_mon}-{local_date[6:10]}:#01"
        )
        self.assert_stamp(vf, f"{local_date}:01")
        self.assertIn("YMH_VERSION_DATE", proc.stderr)
        self.assertIn("ymh:", proc.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake", help="path to the cmake executable")
    args, remaining = parser.parse_known_args()
    VersionStampTest.cmake = args.cmake
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(VersionStampTest)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
