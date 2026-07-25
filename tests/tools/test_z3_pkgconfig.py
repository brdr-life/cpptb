"""Contract for tools/z3_pkgconfig.py.

The generated z3.pc has to satisfy two things that are easy to regress: the
version must compare correctly against the 4.15.5 floor, and Libs must carry an
rpath, or binaries link fine and then fail at run time.
"""

import importlib.util
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "cpptb_z3_pkgconfig", REPO / "tools" / "z3_pkgconfig.py"
)
z3_pkgconfig = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = z3_pkgconfig
_SPEC.loader.exec_module(z3_pkgconfig)


def _fake_wheel(root: Path, *, version: str, library: str) -> Path:
    """Lay out what 'uv pip install --target' produces, without a download."""
    package = root / "z3"
    (package / "include").mkdir(parents=True)
    (package / "lib").mkdir(parents=True)
    (package / "include" / "z3++.h").write_text("// stub\n", encoding="utf-8")
    (package / "lib" / library).write_bytes(b"")
    dist = root / f"z3_solver-{version}.dist-info"
    dist.mkdir()
    (dist / "METADATA").write_text(
        f"Metadata-Version: 2.1\nName: z3-solver\nVersion: {version}\n",
        encoding="utf-8",
    )
    (dist / "RECORD").write_text("", encoding="utf-8")
    return root


class RenderTests(unittest.TestCase):
    def test_libs_carry_an_rpath(self):
        text = z3_pkgconfig.render(Path("/somewhere/z3"), "5.0.0.0", "z3")
        libs = [line for line in text.splitlines() if line.startswith("Libs:")][0]
        self.assertIn("-lz3", libs)
        self.assertIn("-L${libdir}", libs)
        # Without this the binary links but cannot start.
        self.assertIn("-Wl,-rpath,${libdir}", libs)

    def test_declares_name_and_version(self):
        text = z3_pkgconfig.render(Path("/somewhere/z3"), "4.15.8.0", "z3")
        self.assertIn("Name: z3", text)
        self.assertIn("Version: 4.15.8.0", text)


class LibraryDetectionTests(unittest.TestCase):
    def test_accepts_linux_and_macos_layouts(self):
        import tempfile

        for library in ("libz3.so", "libz3.so.5.0", "libz3.dylib"):
            with self.subTest(library=library), tempfile.TemporaryDirectory() as tmp:
                lib_dir = Path(tmp)
                (lib_dir / library).write_bytes(b"")
                self.assertEqual(z3_pkgconfig.library_name(lib_dir), "z3")

    def test_rejects_a_directory_without_a_library(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(z3_pkgconfig.Z3WheelError):
                z3_pkgconfig.library_name(Path(tmp))


class _StubRelocationMixin:
    """Stub Mach-O relocation out of the rendering tests.

    _fake_wheel writes a zero-byte stub, which install_name_tool cannot
    process; relocation has dedicated cases in MacOsRelocationTests.
    """

    def setUp(self):
        from unittest import mock
        patcher = mock.patch.object(
            z3_pkgconfig, "relocate_for_macos", return_value=None
        )
        patcher.start()
        self.addCleanup(patcher.stop)


class GenerateTests(_StubRelocationMixin, unittest.TestCase):
    def test_generates_from_a_target_directory(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            site = _fake_wheel(root / "site", version="5.0.0.0", library="libz3.so")
            target = z3_pkgconfig.generate(root / "pkgconfig", site)

            self.assertTrue(target.is_file())
            text = target.read_text(encoding="utf-8")
            self.assertIn("Version: 5.0.0.0", text)
            self.assertIn(str((site / "z3").resolve()), text)

    def test_reports_a_missing_wheel_clearly(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(z3_pkgconfig.Z3WheelError):
                z3_pkgconfig.generate(root / "pkgconfig", root / "empty")


class MacOsRelocationTests(unittest.TestCase):
    """The dylib install name decides whether an rpath is consulted at all."""

    def test_is_a_no_op_away_from_macos(self):
        import tempfile
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            with mock.patch.object(z3_pkgconfig.platform, "system",
                                   return_value="Linux"):
                self.assertIsNone(z3_pkgconfig.relocate_for_macos(Path(tmp)))

    def test_rewrites_a_bare_install_name(self):
        import tempfile
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            lib_dir = Path(tmp)
            (lib_dir / "libz3.dylib").write_bytes(b"")
            calls = []

            def fake_run(command, **kwargs):
                calls.append(command)
                return mock.Mock(returncode=0, stdout="")

            with mock.patch.object(z3_pkgconfig.platform, "system",
                                   return_value="Darwin"), \
                 mock.patch.object(z3_pkgconfig, "_install_name",
                                   return_value="libz3.dylib"), \
                 mock.patch.object(z3_pkgconfig.shutil, "which",
                                   return_value="/usr/bin/tool"), \
                 mock.patch.object(z3_pkgconfig.subprocess, "run", fake_run):
                result = z3_pkgconfig.relocate_for_macos(lib_dir)

            self.assertEqual(result, "@rpath/libz3.dylib")
            self.assertIn("install_name_tool", calls[0][0])
            self.assertEqual(calls[0][1:3], ["-id", "@rpath/libz3.dylib"])
            # Editing load commands invalidates the signature on arm64.
            self.assertTrue(any("codesign" in c[0] for c in calls))

    def test_leaves_an_already_relocated_dylib_alone(self):
        import tempfile
        from unittest import mock

        with tempfile.TemporaryDirectory() as tmp:
            lib_dir = Path(tmp)
            (lib_dir / "libz3.dylib").write_bytes(b"")
            with mock.patch.object(z3_pkgconfig.platform, "system",
                                   return_value="Darwin"), \
                 mock.patch.object(z3_pkgconfig, "_install_name",
                                   return_value="@rpath/libz3.dylib"), \
                 mock.patch.object(z3_pkgconfig.subprocess, "run") as run:
                result = z3_pkgconfig.relocate_for_macos(lib_dir)
            self.assertEqual(result, "@rpath/libz3.dylib")
            run.assert_not_called()


@unittest.skipIf(shutil.which("pkg-config") is None, "pkg-config not installed")
class PkgConfigIntegrationTests(_StubRelocationMixin, unittest.TestCase):
    def test_pkg_config_accepts_the_file_and_the_version_gate(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            site = _fake_wheel(root / "site", version="5.0.0.0", library="libz3.so")
            target = z3_pkgconfig.generate(root / "pkgconfig", site)
            # Inherit PATH: pkg-config lives in Homebrew's prefix on macOS,
            # not /usr/bin. PKG_CONFIG_PATH is searched before the system
            # directories, so the stub still wins over any installed z3.
            env = dict(os.environ)
            env["PKG_CONFIG_PATH"] = str(target.parent)

            version = subprocess.run(
                ["pkg-config", "--modversion", "z3"],
                env=env, text=True, capture_output=True, check=True,
            )
            self.assertEqual(version.stdout.strip(), "5.0.0.0")

            # The same gate the Makefile and CMakeLists use.
            gate = subprocess.run(
                ["pkg-config", "--atleast-version=4.15.5", "z3"], env=env
            )
            self.assertEqual(gate.returncode, 0)

    def test_old_version_fails_the_gate(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            site = _fake_wheel(root / "site", version="4.8.12.0", library="libz3.so")
            target = z3_pkgconfig.generate(root / "pkgconfig", site)
            # Inherit PATH: pkg-config lives in Homebrew's prefix on macOS,
            # not /usr/bin. PKG_CONFIG_PATH is searched before the system
            # directories, so the stub still wins over any installed z3.
            env = dict(os.environ)
            env["PKG_CONFIG_PATH"] = str(target.parent)

            gate = subprocess.run(
                ["pkg-config", "--atleast-version=4.15.5", "z3"], env=env
            )
            self.assertNotEqual(gate.returncode, 0)


if __name__ == "__main__":
    unittest.main()
