"""Single-binary entry point for the cpptb toolchain.

PyInstaller/Nuitka bundle this file together with the Python toolchain and
the framework tree (include/ and src/). When frozen, the bundle directory is
a complete framework root, so pointing CPPTB_ROOT at it makes
find_framework_include() and the host-loop search work unchanged.

The one binary carries every entry point: the default is the `cpptb`
project CLI, and the first argument selects a lower-level tool by name --
`codegen`, `run`, and `rggen` map to cpptb-codegen, cpptb-run, and
cpptb-rggen. None of those names collides with a `cpptb` subcommand.
"""

import hashlib
import os
import shutil
import sys


def _materialize_framework(bundle: str) -> str:
    """Copy the bundled framework tree to a stable, versioned cache.

    A onefile bundle extracts to a fresh temporary directory on every run.
    Build fingerprints include framework file paths, so pointing CPPTB_ROOT
    at the ephemeral extraction would miss the cache on every invocation.
    A directory keyed by the executable's content hash is stable for one
    release and automatically fresh for the next.
    """
    digest = hashlib.sha256()
    with open(sys.executable, "rb") as exe:
        for chunk in iter(lambda: exe.read(1 << 20), b""):
            digest.update(chunk)
    cache_root = os.environ.get("XDG_CACHE_HOME") or os.path.join(
        os.path.expanduser("~"), ".cache")
    target = os.path.join(cache_root, "cpptb", f"fw-{digest.hexdigest()[:16]}")
    marker = os.path.join(target, ".complete")
    if not os.path.exists(marker):
        staging = target + ".partial"
        shutil.rmtree(staging, ignore_errors=True)
        for tree in ("include", "src"):
            shutil.copytree(os.path.join(bundle, tree),
                            os.path.join(staging, tree))
        shutil.rmtree(target, ignore_errors=True)
        os.replace(staging, target)
        with open(marker, "w") as done:
            done.write("cpptb framework tree\n")
    return target


def main() -> int:
    if getattr(sys, "frozen", False):
        bundle = getattr(sys, "_MEIPASS", None) or os.path.dirname(
            os.path.abspath(sys.executable))
        os.environ.setdefault("CPPTB_ROOT", _materialize_framework(bundle))

    tool = sys.argv[1] if len(sys.argv) > 1 else ""
    if tool == "codegen":
        from cpptb_codegen.generate_dpi_bindings import main as tool_main
        sys.argv = [sys.argv[0] + " codegen", *sys.argv[2:]]
    elif tool == "run":
        from cpptb_codegen.runner import main as tool_main
        sys.argv = [sys.argv[0] + " run", *sys.argv[2:]]
    elif tool == "rggen":
        from cpptb_codegen.rggen_codegen import main as tool_main
        sys.argv = [sys.argv[0] + " rggen", *sys.argv[2:]]
    else:
        from cpptb_codegen.cli import main as tool_main
    result = tool_main()
    return int(result) if result is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
