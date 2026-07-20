"""Minimal out-of-process launcher for already-built cpptb simulations."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


TEST_PREFIX = "CPPTB_TEST "
RESULT_SCHEMA_VERSIONS = {1, 2, 3, 4, 5}
RESULT_STATUSES = {
    "passed",
    "failed",
    "error",
    "skipped",
    "expected_failure",
    "unexpected_pass",
    "timed_out",
}
SUCCESSFUL_STATUSES = {"passed", "skipped", "expected_failure"}


class RunnerError(RuntimeError):
    """A simulator invocation or result contract failed."""


@dataclass(frozen=True)
class Invocation:
    returncode: int
    stdout: str
    stderr: str


def _run_command(
    command: Sequence[str],
    environment: dict[str, str],
    timeout: float | None,
) -> Invocation:
    try:
        completed = subprocess.run(
            list(command),
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise RunnerError(
            f"simulator exceeded the {timeout:g} second wall-time timeout"
        ) from error
    except OSError as error:
        raise RunnerError(f"cannot launch simulator: {error}") from error
    return Invocation(completed.returncode, completed.stdout, completed.stderr)


def discover_tests(command: Sequence[str], timeout: float | None) -> list[str]:
    environment = os.environ.copy()
    environment["CPPTB_LIST_TESTS"] = "1"
    environment.pop("CPPTB_TEST", None)
    environment.pop("CPPTB_RESULT_FILE", None)
    invocation = _run_command(command, environment, timeout)
    tests = [
        line[len(TEST_PREFIX) :]
        for line in invocation.stdout.splitlines()
        if line.startswith(TEST_PREFIX)
    ]
    if invocation.returncode != 0:
        detail = invocation.stderr.strip() or invocation.stdout.strip()
        raise RunnerError(
            f"test discovery exited with status {invocation.returncode}: {detail}"
        )
    if len(tests) != len(set(tests)):
        raise RunnerError("compiled test catalog contains duplicate names")
    return tests


def _result_stem(test_name: str) -> str:
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", test_name).strip("._")
    if stem == test_name:
        return stem
    digest = hashlib.sha256(test_name.encode("utf-8")).hexdigest()[:8]
    return f"{stem or 'unnamed_test'}-{digest}"


def _read_result(path: Path, expected_test: str) -> dict[str, object]:
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RunnerError("simulator did not produce a result file") from error
    except (OSError, json.JSONDecodeError) as error:
        raise RunnerError(f"cannot read result file: {error}") from error
    if result.get("schema_version") not in RESULT_SCHEMA_VERSIONS:
        raise RunnerError("result file has an unsupported schema version")
    if result.get("test_name") != expected_test:
        raise RunnerError(
            f"result names test {result.get('test_name')!r}, expected {expected_test!r}"
        )
    if result.get("status") not in RESULT_STATUSES:
        raise RunnerError(f"result has invalid status {result.get('status')!r}")
    return result


def run_tests(
    command: Sequence[str],
    tests: Sequence[str],
    result_dir: Path,
    timeout: float | None,
    seed: int | None = None,
) -> int:
    result_dir.mkdir(parents=True, exist_ok=True)
    passed = 0
    failed = 0
    errored = 0

    for test_name in tests:
        stem = _result_stem(test_name)
        result_path = result_dir / f"{stem}.json"
        log_path = result_dir / f"{stem}.log"
        result_path.unlink(missing_ok=True)
        environment = os.environ.copy()
        environment.pop("CPPTB_LIST_TESTS", None)
        environment["CPPTB_TEST"] = test_name
        environment["CPPTB_RESULT_FILE"] = str(result_path.resolve())
        if seed is not None:
            environment["CPPTB_RANDOM_SEED"] = str(seed)

        try:
            invocation = _run_command(command, environment, timeout)
            log_path.write_text(
                invocation.stdout + invocation.stderr, encoding="utf-8"
            )
            result = _read_result(result_path, test_name)
            status = str(result["status"])
            successful = (
                status in SUCCESSFUL_STATUSES and invocation.returncode == 0
            )
            if successful:
                passed += 1
                label = {
                    "passed": "PASS",
                    "skipped": "SKIP",
                    "expected_failure": "XFAIL",
                }[status]
            elif status in {"failed", "unexpected_pass", "timed_out"}:
                failed += 1
                label = {
                    "failed": "FAIL",
                    "unexpected_pass": "XPASS",
                    "timed_out": "TIMEOUT",
                }[status]
            else:
                errored += 1
                label = "ERROR"
            checks = result.get("checks", 0)
            wall_ms = int(result.get("wall_time_ns", 0)) / 1_000_000.0
            random_seed = result.get("random_seed")
            seed_text = f" seed={random_seed}" if random_seed is not None else ""
            print(
                f"{label:<5} {test_name} checks={checks}{seed_text} "
                f"wall_ms={wall_ms:.3f}"
            )
        except RunnerError as error:
            errored += 1
            print(f"ERROR {test_name}: {error}", file=sys.stderr)

    total = len(tests)
    print(
        f"cpptb: {total} tests: {passed} passed, {failed} failed, "
        f"{errored} errors"
    )
    return 0 if failed == 0 and errored == 0 else 1


def _split_command(arguments: Sequence[str]) -> tuple[list[str], list[str]]:
    try:
        separator = arguments.index("--")
    except ValueError as error:
        raise RunnerError(
            "separate cpptb-run options from the simulator command with --"
        ) from error
    options = list(arguments[:separator])
    command = list(arguments[separator + 1 :])
    if not command:
        raise RunnerError("simulator command is empty")
    return options, command


def _parse_seed(value: str) -> int:
    if value.startswith(("0x", "0X")):
        digits = value[2:]
        valid = bool(re.fullmatch(r"[0-9A-Fa-f]+", digits))
        base = 16
    else:
        digits = value
        valid = bool(re.fullmatch(r"[0-9]+", digits))
        base = 10
    if not valid:
        raise RunnerError(
            "--seed must be a decimal or 0x-prefixed unsigned 64-bit integer"
        )
    seed = int(digits, base)
    if seed > (1 << 64) - 1:
        raise RunnerError("--seed must fit in an unsigned 64-bit integer")
    return seed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="cpptb-run",
        description="List or run tests in an already-built cpptb simulation.",
    )
    subparsers = parser.add_subparsers(dest="operation", required=True)

    list_parser = subparsers.add_parser("list", help="list compiled tests")
    list_parser.add_argument("--timeout", type=float)

    run_parser = subparsers.add_parser("run", help="run selected tests serially")
    run_parser.add_argument("tests", nargs="*")
    run_parser.add_argument("--all", action="store_true")
    run_parser.add_argument("--timeout", type=float)
    run_parser.add_argument(
        "--seed",
        help="master random seed (decimal or 0x-prefixed)",
    )
    run_parser.add_argument("--result-dir", type=Path, default=Path("cpptb-results"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    try:
        options, command = _split_command(arguments)
        parsed = _parser().parse_args(options)
        if parsed.timeout is not None and parsed.timeout <= 0:
            raise RunnerError("--timeout must be greater than zero")
        seed = _parse_seed(parsed.seed) if getattr(parsed, "seed", None) else None

        if parsed.operation == "list":
            for test_name in discover_tests(command, parsed.timeout):
                print(test_name)
            return 0

        if parsed.all and parsed.tests:
            raise RunnerError("choose either --all or explicit test names")
        if parsed.all:
            tests = discover_tests(command, parsed.timeout)
        else:
            tests = list(parsed.tests)
        if not tests:
            raise RunnerError("no tests selected; provide names or --all")
        if len(tests) != len(set(tests)):
            raise RunnerError("the requested test list contains duplicates")
        return run_tests(
            command, tests, parsed.result_dir, parsed.timeout, seed
        )
    except RunnerError as error:
        print(f"cpptb-run: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
