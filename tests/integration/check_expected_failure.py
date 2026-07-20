#!/usr/bin/env python3

import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a command and validate its expected failure output."
    )
    parser.add_argument(
        "--contains",
        action="append",
        default=[],
        help="required output fragment; may be repeated",
    )
    parser.add_argument("--result-json", type=Path)
    parser.add_argument("--result-status")
    parser.add_argument("--result-wait-status")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")

    if args.result_json:
        args.result_json.unlink(missing_ok=True)

    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr
    sys.stdout.write(output)

    if result.returncode == 0:
        print("expected command to fail, but it exited successfully", file=sys.stderr)
        return 1

    missing = [fragment for fragment in args.contains if fragment not in output]
    if missing:
        print(
            "expected failure output did not contain: " + ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    if args.result_json:
        try:
            payload = json.loads(args.result_json.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"cannot read expected result JSON: {error}", file=sys.stderr)
            return 1
        if args.result_status and payload.get("status") != args.result_status:
            print(
                f"expected result status {args.result_status!r}, got "
                f"{payload.get('status')!r}",
                file=sys.stderr,
            )
            return 1
        wait_graph = payload.get("wait_graph") or {}
        if (
            args.result_wait_status
            and wait_graph.get("status") != args.result_wait_status
        ):
            print(
                f"expected wait graph status {args.result_wait_status!r}, got "
                f"{wait_graph.get('status')!r}",
                file=sys.stderr,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
