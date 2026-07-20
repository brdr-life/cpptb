#!/usr/bin/env python3
"""Check that a built wheel contains both public C++ header packages."""

from __future__ import annotations

import sys
import zipfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_wheel.py WHEEL", file=sys.stderr)
        return 2

    wheel = Path(sys.argv[1])
    with zipfile.ZipFile(wheel) as archive:
        names = set(archive.namelist())

    required_suffixes = {
        "include/cpptb/cpptb.hpp",
        "include/cpptb/sv/cpptb_log_pkg.sv",
        "include/cpptb/sv/cpptb_log.svh",
        "include/cpptb/sv/cpptb_sv_log_bridge.cpp",
        "include/cpptb_vc/cpptb_vc.hpp",
        "include/cpptb_vc/apb.hpp",
    }
    missing = sorted(
        suffix
        for suffix in required_suffixes
        if not any(name.endswith(suffix) for name in names)
    )
    if missing:
        print(f"{wheel}: missing public headers: {', '.join(missing)}", file=sys.stderr)
        return 1

    print(f"{wheel}: core, SV logging, and verification-component assets present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
