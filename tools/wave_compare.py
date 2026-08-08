#!/usr/bin/env python3
"""Compare two VCD waves for per-cycle DUT-state equivalence.

Both testbenches instantiate the same RTL module, so under a chosen scope
the signal trees are identical. The meaningful equivalence between a cpptb
run and its pure-SV twin is the design's cycle trajectory: the value of
every signal in that subtree, sampled after each rising clock edge's
updates have applied. Sub-cycle offsets on testbench drives (a twin's
`#1ps`, a deferred flush) are deliberately invisible at that sampling
grid, exactly as they are invisible to the design.

Standard library only. VCD is the interchange format because both dump
paths can produce it (Verilator `--trace` + `$dumpvars` on the twin,
`cpptb test --wave vcd` on the framework side) and it parses in a page
of code.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Wave:
    # id code -> every full dotted name sharing that code. Verilator reuses
    # one identifier for aliased nets (a DUT port and the wrapper net bound
    # to it), so a single name per id would hide whichever alias is looked
    # up second -- typically the whole port list under the DUT instance.
    names: dict[str, list[str]] = field(default_factory=dict)
    # ordered (time, id, value) changes
    changes: list[tuple[int, str, str]] = field(default_factory=list)
    timescale: str = ""


def parse_vcd(path: Path) -> Wave:
    wave = Wave()
    scope: list[str] = []
    time = 0
    with path.open("r", encoding="utf-8", errors="replace") as source:
        tokens = source.read().split("\n")
    index = 0
    in_definitions = True
    while index < len(tokens):
        line = tokens[index].strip()
        index += 1
        if not line:
            continue
        if in_definitions:
            if line.startswith("$scope"):
                parts = line.split()
                scope.append(parts[2] if len(parts) > 2 else "?")
            elif line.startswith("$upscope"):
                if scope:
                    scope.pop()
            elif line.startswith("$var"):
                # $var <type> <width> <id> <name> [range] $end
                parts = line.split()
                identifier = parts[3]
                name = parts[4]
                full = ".".join([*scope, name])
                wave.names.setdefault(identifier, []).append(full)
            elif line.startswith("$timescale"):
                wave.timescale = line.removeprefix("$timescale").removesuffix(
                    "$end"
                ).strip()
            elif line.startswith("$enddefinitions"):
                in_definitions = False
            continue
        if line[0] == "#":
            time = int(line[1:])
        elif line[0] in "01xzXZ":
            wave.changes.append((time, line[1:], line[0].lower()))
        elif line[0] in "bB":
            value, _, identifier = line[1:].partition(" ")
            wave.changes.append((time, identifier, value.lower()))
        elif line[0] in "rR":
            value, _, identifier = line[1:].partition(" ")
            wave.changes.append((time, identifier, value))
        # $dumpvars / $end markers inside the change section carry no state.
    return wave


def normalize(value: str) -> str:
    # Binary values minus leading zeros so 4-bit and 32-bit dumps of the
    # same number compare equal; x/z kept as-is.
    stripped = value.lstrip("0")
    return stripped if stripped else "0"


def cycle_states(
    wave: Wave, scope_prefix: str, clock_name: str
) -> tuple[list[int], dict[str, list[str]], list[str]]:
    """Sample every in-scope signal after each rising clock edge."""

    prefix = scope_prefix.rstrip(".") + "."
    in_scope: dict[str, str] = {}
    for identifier, aliases in wave.names.items():
        for full in aliases:
            if full.startswith(prefix):
                in_scope[identifier] = full[len(prefix):]
                break
    clock_ids = {
        identifier
        for identifier, aliases in wave.names.items()
        if any(
            full == clock_name or full.endswith("." + clock_name)
            for full in aliases
        )
    }
    if not clock_ids:
        raise SystemExit(f"clock {clock_name!r} not found in the dump")
    if not in_scope:
        raise SystemExit(f"no signals under scope {scope_prefix!r}")

    current: dict[str, str] = {}
    clock_value = "x"
    edge_times: list[int] = []
    samples: dict[str, list[str]] = {name: [] for name in in_scope.values()}

    changes = wave.changes
    total = len(changes)
    position = 0
    while position < total:
        time = changes[position][0]
        rising = False
        # apply every change at this timestamp, watching the clock
        while position < total and changes[position][0] == time:
            _, identifier, value = changes[position]
            if identifier in clock_ids:
                if clock_value in ("0", "x") and value == "1":
                    rising = True
                clock_value = value
            if identifier in in_scope:
                current[identifier] = value
            position += 1
        if rising:
            edge_times.append(time)
            for identifier, name in in_scope.items():
                samples[name].append(normalize(current.get(identifier, "x")))
    return edge_times, samples, sorted(in_scope.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", type=Path, required=True, help="first VCD")
    parser.add_argument("--a-scope", required=True)
    parser.add_argument("--b", type=Path, required=True, help="second VCD")
    parser.add_argument("--b-scope", required=True)
    parser.add_argument(
        "--clock-signal", required=True,
        help="clock signal name (suffix match on the full dotted path)",
    )
    parser.add_argument(
        "--min-cycles", type=int, default=1,
        help="fail if fewer common cycles than this were compared",
    )
    args = parser.parse_args()

    a_edges, a_samples, a_names = cycle_states(
        parse_vcd(args.a), args.a_scope, args.clock_signal
    )
    b_edges, b_samples, b_names = cycle_states(
        parse_vcd(args.b), args.b_scope, args.clock_signal
    )

    common = sorted(set(a_names) & set(b_names))
    only_a = sorted(set(a_names) - set(b_names))
    only_b = sorted(set(b_names) - set(a_names))
    if not common:
        print("wave-compare: no common signals under the given scopes",
              file=sys.stderr)
        return 1
    for name in only_a:
        print(f"wave-compare: only in {args.a}: {name}", file=sys.stderr)
    for name in only_b:
        print(f"wave-compare: only in {args.b}: {name}", file=sys.stderr)

    cycles = min(len(a_edges), len(b_edges))
    if cycles < args.min_cycles:
        print(
            f"wave-compare: only {cycles} common cycles "
            f"(a={len(a_edges)}, b={len(b_edges)}), need {args.min_cycles}",
            file=sys.stderr,
        )
        return 1

    mismatches = 0
    for cycle in range(cycles):
        for name in common:
            left = a_samples[name][cycle]
            right = b_samples[name][cycle]
            if left != right:
                if mismatches == 0:
                    print(
                        f"wave-compare: first divergence at cycle {cycle} "
                        f"(a time {a_edges[cycle]}, b time {b_edges[cycle]})",
                        file=sys.stderr,
                    )
                print(
                    f"  cycle {cycle}: {name}: a={left} b={right}",
                    file=sys.stderr,
                )
                mismatches += 1
                if mismatches >= 20:
                    print("  ... further mismatches suppressed",
                          file=sys.stderr)
                    break
        if mismatches >= 20:
            break

    status = "equal" if mismatches == 0 else "MISMATCH"
    print(
        f"WAVE_COMPARE status={status} cycles={cycles} "
        f"signals={len(common)} a_cycles={len(a_edges)} "
        f"b_cycles={len(b_edges)}"
    )
    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
