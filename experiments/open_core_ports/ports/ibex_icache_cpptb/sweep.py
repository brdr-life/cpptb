#!/usr/bin/env python3
"""Replay one recording many times, to measure a rate rather than sample it.

`replay.py` records the baseline once and replays it once. Under `--mode items`
only the core item stream comes from the recording and every delay below the
sequence is drawn again here, so one replay is one sample of a distribution and
the difference between the two harnesses is buried in it. A recording costs
about twenty seconds of the baseline and a replay costs a fifth of a second, so
this runs the port many times against each recording, on a different seed each
time, and compares the mean with what the recording itself did.

    python3 replay.py ibex_icache_many_errors --seeds 30 --mode pins --keep
    python3 sweep.py --test ibex_icache_many_errors --seeds 30

The recordings have to be on disk already: `replay.py --keep` leaves them under
`build/replay`, and this reads them from there. `--mode pins` alone is enough,
and is the faster way to make them.

What comes out is, for each rate, the mean and standard error over recordings
of (the port's mean over its seeds) / (what the recording did). The port's own
sub-item draws are averaged away; the recording's are not, so the standard
error still falls only as the number of recordings grows. Both spreads are
printed, because which of the two dominates decides whether more recordings or
more port seeds is the cheaper way to tighten a number.

`--match-key-cfg` reads `zero_delays` and `device_delay_max` out of each
recording's UVM_HIGH log and forces them on the port. `push_pull_agent_cfg` is
randomised once per run, so the key device's whole delay budget for a run is
one draw from a heavy-tailed distribution; averaging the port over many seeds
converges it to the population mean while the baseline keeps its single draw,
and that asymmetry is worth a few tenths of a percent of `grants/fetch` on the
tests that invalidate. Matching them removes it.

`--env NAME=VALUE` is passed to the item replays only, which is how
`ICACHE_KEY_DELAY_SCALE` and the rest were measured. See RESULTS.md.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
BINARY = (ROOT / "work" / "ibex_icache_cpptb" / "cpptb" / "ibex_icache_cpptb" /
          "obj" / "Vdpi_ibex_icache_cpptb")
REPLAYS = HERE / "build" / "replay"

REPORT_RE = re.compile(r"^cpptb-icache (\S+) (.*)$", re.M)
RESULT_RE = re.compile(r"RESULT iterations=\d+ checks=(\d+) sim_cycles=(\d+) "
                       r"wall_ms=([0-9.]+) failures=(\d+)")

RATES = [("insns/item", "insns_requested", "items"),
         ("fetches/insn", "fetches", "insns_requested"),
         ("grants/fetch", "mem_grants", "fetches"),
         ("grants/insn", "mem_grants", "insns_requested"),
         ("err/resp", "mem_response_errors", "mem_responses"),
         ("cycles/item", "cycles", "items")]


class RunError(Exception):
    pass


def run(test: str, seed: int, prefix: Path, mode: str,
        extra_env: dict) -> dict:
    """One replay. Returns the counters off its report line."""
    environment = dict(os.environ)
    environment["CPPTB_TEST"] = test
    environment["CPPTB_RANDOM_SEED"] = str(seed)
    environment.pop("ICACHE_REPLAY", None)
    environment.pop("ICACHE_ITEMS", None)
    environment["ICACHE_REPLAY" if mode == "pins" else "ICACHE_ITEMS"] = \
        str(prefix)
    environment.update(extra_env)
    completed = subprocess.run([str(BINARY)], env=environment, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=600,
                               check=False)
    text = completed.stdout
    result = RESULT_RE.search(text)
    if result is None:
        raise RunError(f"{test} seed {seed} {mode} printed no result line:\n"
                       f"{text[-2000:]}")
    if int(result.group(4)) != 0:
        raise RunError(f"{test} seed {seed} {mode} failed:\n{text[-2000:]}")
    match = REPORT_RE.search(text)
    if match is None or "replay-pins-only" in match.group(2):
        raise RunError(f"{test} seed {seed} {mode} reported no counters; the "
                       f"two combo tests cannot be replayed this way")
    counters = {}
    for pair in match.group(2).split():
        key, _, value = pair.partition("=")
        counters[key] = int(value)
    # A run that counted nothing reads exactly like a run that agreed, so it is
    # refused rather than averaged in.
    if counters.get("items", 0) == 0 or counters.get("mem_grants", 0) == 0:
        raise RunError(f"{test} seed {seed} {mode} counted nothing: {counters}")
    return counters


def key_cfg(log: Path) -> dict:
    """zero_delays and device_delay_max out of the baseline's UVM_HIGH log.

    push_pull_agent_cfg is printed once, by uvm_field_int, so those two are
    hex. The per-transaction delays in the same log are not: convert2string
    prints them with %0d behind a literal "0x", which is a good way to read a
    delay of 131 as one of 305.
    """
    text = log.read_text(errors="replace")

    def field(name: str, width: int) -> int:
        match = re.search(rf"{name}\s+integral\s+{width}\s+'h([0-9a-f]+)", text)
        if match is None:
            raise RunError(f"{log}: no {name}. Was the recording made at "
                           f"UVM_HIGH?")
        return int(match.group(1), 16)

    return {"ICACHE_KEY_ZERO_DELAYS": str(field("zero_delays", 1)),
            # Zero means "leave the draw alone" to the port, so a run whose
            # maximum really was zero passes 1 and relies on zero_delays.
            "ICACHE_KEY_DELAY_MAX": str(field("device_delay_max", 32) or 1)}


def rate(counters: dict, top: str, bottom: str) -> float:
    below = counters.get(bottom, 0)
    return counters[top] / below if below else float("nan")


def summarise(test: str, by_seed: dict, port_seeds: int, extra_env: dict,
              per_seed_rate: str | None) -> None:
    print(f"\n{test}: {len(by_seed)} recordings x {port_seeds} port seeds")
    if extra_env:
        print(f"  item-replay environment: {extra_env}")

    for label, top, bottom in RATES:
        ratios = []
        for seed in sorted(by_seed):
            block = by_seed[seed]
            uvm = rate(block["pins"], top, bottom)
            port = statistics.fmean(rate(c, top, bottom)
                                    for c in block["items"])
            # A seed-0 test can see no errored response at all, which is a
            # ratio with nothing under it rather than a value.
            if uvm and uvm == uvm and port == port:
                ratios.append(port / uvm)
        if not ratios:
            print(f"  {label:14} no recording had a nonzero denominator")
            continue
        mean = statistics.fmean(ratios)
        sem = (statistics.stdev(ratios) / math.sqrt(len(ratios))
               if len(ratios) > 1 else 0.0)
        away = abs(mean - 1.0) / sem if sem else float("nan")
        print(f"  {label:14} port/uvm {mean:8.5f} +- {sem:7.5f} "
              f"({away:4.1f} se)")

    if per_seed_rate:
        top, _, bottom = per_seed_rate.partition("/")
        print(f"  per recording, {top} over {bottom}")
        for seed in sorted(by_seed):
            uvm = rate(by_seed[seed]["pins"], top, bottom)
            port = statistics.fmean(rate(c, top, bottom)
                                    for c in by_seed[seed]["items"])
            print(f"    seed {seed}  uvm {uvm:9.5f}  port {port:9.5f}  "
                  f"ratio {port / uvm if uvm else float('nan'):8.5f}")

    # Which spread dominates decides whether more recordings or more port seeds
    # is the cheaper way to tighten a number. The first is what averaging over
    # port seeds removes; the second is what only more recordings can remove.
    within = []
    port_means = []
    for seed in sorted(by_seed):
        values = [rate(c, "mem_grants", "fetches")
                  for c in by_seed[seed]["items"]]
        port_means.append(statistics.fmean(values))
        if len(values) > 1:
            within.append(statistics.stdev(values) / port_means[-1])
    uvm_values = [rate(by_seed[s]["pins"], "mem_grants", "fetches")
                  for s in sorted(by_seed)]
    print("  spread of grants/fetch, as a relative standard deviation")
    if within:
        print(f"    the port, over its seeds against one recording "
              f"{statistics.fmean(within):.4f}")
    if len(port_means) > 1:
        print(f"    the port's means, across recordings                "
              f"{statistics.stdev(port_means) / statistics.fmean(port_means):.4f}")
        print(f"    the baseline, across recordings                    "
              f"{statistics.stdev(uvm_values) / statistics.fmean(uvm_values):.4f}")

    for label, top, bottom in RATES:
        uvm_top = sum(by_seed[s]["pins"][top] for s in by_seed)
        uvm_bot = sum(by_seed[s]["pins"][bottom] for s in by_seed)
        port_top = sum(c[top] for s in by_seed for c in by_seed[s]["items"])
        port_bot = sum(c[bottom] for s in by_seed for c in by_seed[s]["items"])
        if not uvm_bot or not port_bot or not uvm_top:
            continue
        print(f"  pooled {label:14} uvm {uvm_top / uvm_bot:8.5f}  "
              f"port {port_top / port_bot:8.5f}  "
              f"ratio {(port_top / port_bot) / (uvm_top / uvm_bot):8.5f}")


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--test", default="ibex_icache_many_errors")
    parser.add_argument("--seeds", type=int, default=10,
                        help="recordings to use, starting at --seed")
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--port-seeds", type=int, default=25,
                        help="item replays per recording")
    parser.add_argument("--port-seed", type=int, default=100000,
                        help="first port seed; each recording gets its own "
                             "block of a thousand from here")
    parser.add_argument("--match-key-cfg", action="store_true",
                        help="force the recorded run's zero_delays and "
                             "device_delay_max on the port")
    parser.add_argument("--env", action="append", default=[],
                        metavar="NAME=VALUE",
                        help="passed to the item replays; repeatable")
    parser.add_argument("--per-seed", metavar="TOP/BOTTOM",
                        help="also print one line per recording for this rate, "
                             "as counter names")
    parser.add_argument("--jobs", type=int,
                        default=max(1, os.cpu_count() or 4))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    if not BINARY.is_file():
        print(f"sweep: no simulator at {BINARY}\n"
              f"run: uv run --frozen cpptb build --project {HERE}",
              file=sys.stderr)
        return 1
    extra_env = dict(pair.split("=", 1) for pair in args.env)

    jobs = []
    for index in range(args.seeds):
        seed = args.seed + index
        prefix = REPLAYS / f"{args.test}.{seed}"
        if not Path(f"{prefix}.items").is_file():
            print(f"sweep: no recording at {prefix}.items\n"
                  f"run: python3 replay.py {args.test} --seed {args.seed} "
                  f"--seeds {args.seeds} --mode pins --keep",
                  file=sys.stderr)
            return 1
        env = dict(extra_env)
        if args.match_key_cfg:
            env.update(key_cfg(Path(f"{prefix}.log")))
        jobs.append(("pins", seed, prefix, seed, {}))
        for offset in range(args.port_seeds):
            # A distinct block of port seeds per recording. Reusing one block
            # for every recording would leave whatever that block happens to
            # draw as a common component of every port mean, which no number of
            # recordings averages away.
            jobs.append(("items", seed, prefix,
                         args.port_seed + 1000 * index + offset, env))

    results = []
    try:
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=args.jobs) as pool:
            futures = {
                pool.submit(run, args.test, port_seed, prefix, mode, env):
                    (mode, seed, port_seed)
                for mode, seed, prefix, port_seed, env in jobs}
            for future in concurrent.futures.as_completed(futures):
                mode, seed, port_seed = futures[future]
                results.append({"mode": mode, "seed": seed,
                                "port_seed": port_seed,
                                "counters": future.result()})
    except (RunError, subprocess.TimeoutExpired) as error:
        print(f"sweep: {error}", file=sys.stderr)
        return 1

    by_seed = {}
    for entry in results:
        block = by_seed.setdefault(entry["seed"], {"pins": None, "items": []})
        if entry["mode"] == "pins":
            block["pins"] = entry["counters"]
        else:
            block["items"].append(entry["counters"])

    summarise(args.test, by_seed, args.port_seeds, extra_env, args.per_seed)

    if args.json:
        args.json.write_text(json.dumps(
            {"test": args.test, "env": extra_env, "results": results},
            indent=1) + "\n", encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
