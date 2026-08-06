#!/usr/bin/env python3
"""Merge a regression's coverage models into one, and report it.

A single test's coverage figure means very little. What a regression exists to
produce is the union: which bins any test reached, and -- more usefully -- which
bins no test reached at all. `run_directed.py` writes one `coverage.json` per
entry; this merges them and prints the report a coverage review actually
consumes.

    python3 coverage.py build/directed/<run>
    python3 coverage.py build/directed/<run> --unhit      # only the holes
    python3 coverage.py build/directed/<run> --json merged.json

The merge is a sum of hit counts over an identical model, and it verifies the
model rather than assuming it: every entry must declare the same covergroup,
the same coverpoints in the same order, and the same bins with the same kinds.
A run whose model differs -- because the covergroup was edited half way through
a regression, say -- is reported and refused rather than quietly averaged, which
is the same discipline `CoverageSnapshot::merge` applies inside the testbench.

There is no UVM equivalent to compare against. Verilator 5.050 cannot build
Ibex's functional coverage at all: it hits two separate internal faults, and
even past them it discards 456 coverage constructs. See ../../FINDINGS.md.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class MergeError(RuntimeError):
    pass


def model_key(coverage: dict) -> tuple:
    """What has to match for two snapshots to be summable."""
    return (
        coverage["name"],
        tuple((point["name"], tuple((b["name"], b["kind"])
                                    for b in point["bins"]))
              for point in coverage["points"]),
        tuple((cross["name"], tuple(cross["points"]),
               tuple(tuple(b["bins"]) for b in cross["bins"]))
              for cross in coverage["crosses"]),
    )


def merge(into: dict, other: dict) -> None:
    into["samples"] += other["samples"]
    into["illegal_hits"] += other["illegal_hits"]
    for mine, theirs in zip(into["points"], other["points"]):
        mine["samples"] += theirs["samples"]
        mine["illegal_hits"] += theirs["illegal_hits"]
        for a, b in zip(mine["bins"], theirs["bins"]):
            a["hits"] += b["hits"]
    for mine, theirs in zip(into["crosses"], other["crosses"]):
        mine["illegal_hits"] += theirs["illegal_hits"]
        for a, b in zip(mine["bins"], theirs["bins"]):
            a["hits"] += b["hits"]


def load(run: Path) -> tuple[dict, int, list[str]]:
    files = sorted(run.glob("*/coverage.json"))
    if not files:
        raise MergeError(
            f"no coverage.json under {run}; the runner writes one per entry, "
            f"so either this run predates that or every entry failed to build")

    merged: dict | None = None
    reference: tuple | None = None
    contributing: list[str] = []
    for path in files:
        try:
            coverage = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise MergeError(f"{path}: {error}") from error
        key = model_key(coverage)
        if merged is None:
            merged, reference = coverage, key
        elif key != reference:
            raise MergeError(
                f"{path.parent.name} declares a different coverage model from "
                f"{contributing[0]}; the covergroup changed part way through "
                f"this run and the two cannot be summed")
        else:
            merge(merged, coverage)
        contributing.append(path.parent.name)
    assert merged is not None
    return merged, len(files), contributing


def bin_is_coverable(kind: str) -> bool:
    return kind in ("ordinary", "transition")


def report(coverage: dict, entries: int, unhit_only: bool) -> None:
    print(f"covergroup {coverage['name']}: {entries} entries merged, "
          f"{coverage['samples']:,} samples")

    total = hit = 0
    holes: list[str] = []
    for point in coverage["points"]:
        coverable = [b for b in point["bins"] if bin_is_coverable(b["kind"])]
        if not coverable:
            continue
        covered = [b for b in coverable if b["hits"]]
        total += len(coverable)
        hit += len(covered)
        if not unhit_only:
            percent = 100.0 * len(covered) / len(coverable)
            flag = "" if len(covered) == len(coverable) else "   <-- holes"
            print(f"  {point['name']:<32} {len(covered):>3}/"
                  f"{len(coverable):<3} {percent:5.1f}%{flag}")
        holes += [f"{point['name']}.{b['name']}"
                  for b in coverable if not b["hits"]]

    for cross in coverage["crosses"]:
        if not cross["bins"]:
            continue
        covered = [b for b in cross["bins"] if b["hits"]]
        total += len(cross["bins"])
        hit += len(covered)
        if not unhit_only:
            percent = 100.0 * len(covered) / len(cross["bins"])
            flag = "" if len(covered) == len(cross["bins"]) else "   <-- holes"
            print(f"  {cross['name']:<32} {len(covered):>3}/"
                  f"{len(cross['bins']):<3} {percent:5.1f}%{flag}")
        holes += [f"{cross['name']}[{', '.join(b['bins'])}]"
                  for b in cross["bins"] if not b["hits"]]

    # An illegal bin that was reached is the opposite of a hole and matters
    # more: it is a property the run violated.
    illegal = [(point["name"], b["name"], b["hits"])
               for point in coverage["points"] for b in point["bins"]
               if b["kind"] == "illegal" and b["hits"]]

    if holes:
        print(f"\n{len(holes)} bin(s) no entry reached:")
        for name in holes:
            print(f"  {name}")

    if illegal:
        print(f"\n{len(illegal)} illegal bin(s) reached:")
        for point, name, hits in illegal:
            print(f"  {point}.{name}  {hits:,}")

    print(f"\n{hit} of {total} bins covered, "
          f"{100.0 * hit / total if total else 100.0:.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("run", type=Path,
                        help="a directory under build/directed")
    parser.add_argument("--unhit", action="store_true",
                        help="print only the bins nothing reached")
    parser.add_argument("--json", type=Path, help="write the merged model here")
    args = parser.parse_args()

    # Relative to where it was invoked, the way a command-line tool should be,
    # and only then relative to the port directory. Resolving against the
    # script's own directory first turned `coverage.py ports/x/build/...` run
    # from the experiment root into `ports/x/ports/x/build/...`.
    run = args.run
    if not run.is_absolute() and not run.is_dir():
        run = HERE / args.run
    try:
        merged, entries, _ = load(run)
    except MergeError as error:
        print(f"coverage: {error}", file=sys.stderr)
        return 1

    report(merged, entries, args.unhit)
    if args.json:
        args.json.write_text(json.dumps(merged, indent=2) + "\n",
                             encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
