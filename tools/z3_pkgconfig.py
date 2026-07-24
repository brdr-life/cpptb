#!/usr/bin/env python3
"""Expose the PyPI ``z3-solver`` wheel to pkg-config.

Distribution Z3 packages are frequently older than the 4.15.5 that
``cpptb/z3_random_backend.hpp`` needs, and Homebrew and apt disagree about
where Z3 lives. The ``z3-solver`` wheel ships ``z3++.h`` and the shared library
for macOS and Linux on both arm64 and x86_64, so installing it through uv gives
one portable path.

The wheel has no ``z3.pc``, which is the only reason the existing pkg-config
detection in the Makefile and CMakeLists cannot see it. Generating one keeps
that detection unchanged.

    make z3-toolchain

The wheel is installed into its own directory rather than the project virtual
environment on purpose: ``uv sync --frozen`` prunes anything outside the
lockfile's default groups, which would delete the library out from under
already-linked binaries and break them at run time with a dangling rpath.

The emitted ``Libs`` carries an rpath so binaries find the wheel's library at
run time without the caller exporting LD_LIBRARY_PATH or DYLD_LIBRARY_PATH.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
from importlib import metadata
from pathlib import Path

DEFAULT_OUTPUT = Path("build") / "pkgconfig"


class Z3WheelError(RuntimeError):
    """The z3-solver wheel is not importable from this interpreter."""


def locate_wheel(site_dir: Path | None) -> Path:
    """Return the wheel's ``z3`` package directory (holds include/ and lib/)."""
    if site_dir is not None:
        candidate = site_dir / "z3"
        if not candidate.is_dir():
            raise Z3WheelError(
                f"{site_dir} has no z3/ package; install it with "
                f"'uv pip install --target {site_dir} z3-solver'"
            )
        return candidate
    spec = importlib.util.find_spec("z3")
    if spec is None or not spec.submodule_search_locations:
        raise Z3WheelError(
            "cannot import the 'z3' package; run 'make z3-toolchain' or pass "
            "--site-dir"
        )
    return Path(list(spec.submodule_search_locations)[0])


def wheel_version(site_dir: Path | None) -> str:
    if site_dir is not None:
        # --target installs a normal .dist-info, so read it in place rather
        # than importing, which would need the directory on sys.path.
        found = [
            dist.version
            for dist in metadata.distributions(path=[str(site_dir)])
            if (dist.metadata["Name"] or "").lower() == "z3-solver"
        ]
        if not found:
            raise Z3WheelError(f"no z3-solver distribution metadata under {site_dir}")
        return found[0]
    try:
        return metadata.version("z3-solver")
    except metadata.PackageNotFoundError as error:  # pragma: no cover - defensive
        raise Z3WheelError("z3-solver is not installed") from error


def library_name(lib_dir: Path) -> str:
    """Confirm a linkable library exists; both platforms link with -lz3."""
    for pattern in ("libz3.so*", "libz3.dylib", "libz3.*.dylib"):
        if any(lib_dir.glob(pattern)):
            return "z3"
    raise Z3WheelError(f"no libz3 shared library under {lib_dir}")


def render(prefix: Path, version: str, lib: str) -> str:
    return (
        f"prefix={prefix}\n"
        "includedir=${prefix}/include\n"
        "libdir=${prefix}/lib\n"
        "\n"
        "Name: z3\n"
        "Description: Z3 theorem prover (PyPI z3-solver wheel)\n"
        f"Version: {version}\n"
        "Cflags: -I${includedir}\n"
        # The rpath keeps the wheel's library resolvable at run time on both
        # Linux (DT_RUNPATH) and macOS (LC_RPATH).
        f"Libs: -L${{libdir}} -l{lib} -Wl,-rpath,${{libdir}}\n"
    )


def generate(output_dir: Path, site_dir: Path | None = None) -> Path:
    prefix = locate_wheel(site_dir).resolve()
    include_dir = prefix / "include"
    lib_dir = prefix / "lib"
    if not (include_dir / "z3++.h").is_file():
        raise Z3WheelError(f"{include_dir} has no z3++.h")
    lib = library_name(lib_dir)

    output_dir.mkdir(parents=True, exist_ok=True)
    target = output_dir / "z3.pc"
    target.write_text(
        render(prefix, wheel_version(site_dir), lib), encoding="utf-8"
    )
    return target


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"directory to write z3.pc into (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--site-dir",
        type=Path,
        default=None,
        help="directory that 'uv pip install --target' populated; omit to use "
             "the importable z3 package",
    )
    parser.add_argument(
        "--print-path",
        action="store_true",
        help="print only the directory, for use in PKG_CONFIG_PATH",
    )
    args = parser.parse_args(argv)

    try:
        target = generate(args.output_dir, args.site_dir)
        version = wheel_version(args.site_dir)
    except Z3WheelError as error:
        print(f"z3_pkgconfig: {error}", file=sys.stderr)
        return 1

    if args.print_path:
        print(target.parent)
        return 0

    print(f"wrote {target} (z3 {version})")
    print(f"export PKG_CONFIG_PATH={target.parent.resolve()}:$PKG_CONFIG_PATH")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
