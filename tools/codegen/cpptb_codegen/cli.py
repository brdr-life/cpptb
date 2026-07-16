"""Public command-line interface for building and running cpptb projects."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Sequence

from cpptb_codegen.build import BuildError, build_project
from cpptb_codegen.design_ir import CodegenError
from cpptb_codegen.project import ProjectError, ProjectSpec, resolve_project
from cpptb_codegen.runner import RunnerError, discover_tests, run_tests


def _add_project_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--project",
        type=Path,
        help="project directory (default: current directory)",
    )
    parser.add_argument(
        "--source",
        action="append",
        type=Path,
        help="RTL source, directory, or glob; may be repeated",
    )
    parser.add_argument(
        "--testbench",
        action="append",
        type=Path,
        help="C++ testbench source, directory, or glob; may be repeated",
    )
    parser.add_argument("--top", help="select the DUT top module")
    parser.add_argument("--target", help="override the generated target name")
    parser.add_argument("--build-name", help=argparse.SUPPRESS)
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="build root, relative to the project unless absolute",
    )
    parser.add_argument(
        "--simulator",
        default=None,
        choices=("verilator",),
        help="simulator backend (default: verilator)",
    )
    parser.add_argument(
        "--framework-root",
        type=Path,
        help="cpptb checkout, installation prefix, or include directory",
    )
    parser.add_argument(
        "--rebuild", action="store_true", help="ignore the build cache"
    )
    parser.add_argument(
        "--verbose", action="store_true", help="show compiler commands"
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cpptb",
        description="Build, discover, and run C++ hardware testbenches.",
    )
    parser.add_argument("--version", action="version", version="cpptb 0.1.0")
    commands = parser.add_subparsers(dest="operation", required=True)

    build = commands.add_parser("build", help="generate and build the testbench")
    _add_project_options(build)
    build.add_argument(
        "--compare-frontend",
        choices=("verilator_json",),
        help=argparse.SUPPRESS,
    )

    list_tests = commands.add_parser("list", help="list compiled tests")
    _add_project_options(list_tests)
    list_tests.add_argument("--timeout", type=float)

    test = commands.add_parser("test", help="build and run tests")
    _add_project_options(test)
    test.add_argument("tests", nargs="*", help="test names (default: all)")
    test.add_argument("--timeout", type=float)
    test.add_argument(
        "--result-dir",
        type=Path,
        help="result directory (default: build/cpptb/TARGET/results)",
    )
    return parser


def _resolve(args: argparse.Namespace) -> ProjectSpec:
    return resolve_project(
        project=args.project,
        sources=args.source,
        testbenches=args.testbench,
        top=args.top,
        target=args.target,
        build_name=args.build_name,
        build_dir=args.build_dir,
        simulator=args.simulator,
        refresh_top=args.rebuild,
    )


def _build(args: argparse.Namespace, spec: ProjectSpec) -> Path:
    result = build_project(
        spec,
        rebuild=args.rebuild,
        framework_root=args.framework_root,
        verbose=args.verbose,
        compare_frontend=getattr(args, "compare_frontend", None),
    )
    state = "built" if result.rebuilt else "up to date"
    print(f"cpptb: {spec.target} {state}: {result.binary}")
    return result.binary


def _validate_timeout(timeout: float | None) -> None:
    if timeout is not None and timeout <= 0:
        raise RunnerError("--timeout must be greater than zero")


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        spec = _resolve(args)
        binary = _build(args, spec)
        if args.operation == "build":
            return 0

        _validate_timeout(args.timeout)
        catalog = discover_tests([str(binary)], args.timeout)
        if args.operation == "list":
            for test_name in catalog:
                print(test_name)
            return 0

        selected = list(args.tests) if args.tests else catalog
        unknown = [name for name in selected if name not in catalog]
        if unknown:
            available = ", ".join(catalog) if catalog else "<none>"
            raise RunnerError(
                f"unknown test {unknown[0]!r}; available tests: {available}"
            )
        if len(selected) != len(set(selected)):
            raise RunnerError("the requested test list contains duplicates")
        result_dir = (
            (
                args.result_dir.expanduser()
                if args.result_dir.is_absolute()
                else spec.root / args.result_dir.expanduser()
            ).resolve()
            if args.result_dir is not None
            else spec.result_dir
        )
        return run_tests([str(binary)], selected, result_dir, args.timeout)
    except (BuildError, CodegenError, ProjectError, RunnerError) as error:
        print(f"cpptb: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
