#!/usr/bin/env python3
"""Assert two timing backends produced identical runs of the same tests.

Given two cpptb results directories -- one built with
timing_backend = "verilator-direct", one with "vpi" -- this requires,
for every test in the first:

  * the result JSON matches field for field, except ``wall_time_ns``,
    which measures the host rather than the simulation;
  * a ``.vcd`` dump, when present, is byte-identical;
  * an ``.fst`` dump, when present, is byte-identical outside the FST
    header's date field, the one place the writer records wall-clock
    time. In the FST header block the fixed-size fields end at byte 74,
    the writer string spans [74, 202) and the date string [202, 321);
    everything after that -- geometry, hierarchy, every value-change
    block -- must match exactly.

The claim is deliberately stronger than trajectory equivalence: the two
backends must schedule the same evals at the same simulation times, so
the dumps come out identical, not merely equivalent under sampling.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

FST_DATE_FIELD = (202, 321)


def fail(msg: str) -> None:
    print(f"backend_compare: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def compare_json(a: Path, b: Path) -> None:
    ra = json.loads(a.read_text())
    rb = json.loads(b.read_text())
    ra.pop("wall_time_ns", None)
    rb.pop("wall_time_ns", None)
    if ra != rb:
        keys = sorted(
            k
            for k in set(ra) | set(rb)
            if ra.get(k, "<missing>") != rb.get(k, "<missing>")
        )
        fail(f"{a.name}: result records differ in {', '.join(keys)}")


def compare_vcd(a: Path, b: Path) -> None:
    if a.read_bytes() != b.read_bytes():
        fail(f"{a.name}: vcd dumps are not byte-identical")


def compare_fst(a: Path, b: Path) -> None:
    da, db = bytearray(a.read_bytes()), bytearray(b.read_bytes())
    if len(da) != len(db):
        fail(f"{a.name}: fst dumps differ in size ({len(da)} vs {len(db)})")
    lo, hi = FST_DATE_FIELD
    da[lo:hi] = db[lo:hi] = b""
    if da != db:
        fail(f"{a.name}: fst dumps differ outside the header date field")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direct", type=Path, help="results dir, direct backend")
    parser.add_argument("vpi", type=Path, help="results dir, vpi backend")
    args = parser.parse_args()

    records = sorted(args.direct.glob("*.json"))
    if not records:
        fail(f"no result records under {args.direct}")

    waves = 0
    for rec in records:
        other = args.vpi / rec.name
        if not other.exists():
            fail(f"{rec.name}: present under {args.direct}, missing under {args.vpi}")
        compare_json(rec, other)
        for ext, cmp_fn in ((".vcd", compare_vcd), (".fst", compare_fst)):
            wave = rec.with_suffix(ext)
            if wave.exists():
                peer = args.vpi / wave.name
                if not peer.exists():
                    fail(f"{wave.name}: missing under {args.vpi}")
                cmp_fn(wave, peer)
                waves += 1
    if waves == 0:
        fail("no wave dumps found; run both sides with --wave")
    print(
        f"backend_compare: OK tests={len(records)} waves={waves} "
        f"({args.direct.parent.name} == {args.vpi.parent.name})"
    )


if __name__ == "__main__":
    main()
