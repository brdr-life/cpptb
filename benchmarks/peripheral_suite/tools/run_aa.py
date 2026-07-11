#!/usr/bin/env python3
"""A/A environment diagnostic for the peripheral-suite C++ DPI binary."""

import argparse
import sys
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH_DIR))
import run_benchmark as benchmark  # noqa: E402


INITIAL_PAIRS = 20
EXTRA_PAIRS = 20
DEFAULT_JSON = benchmark.RESULT_DIR / "aa_latest.json"
DEFAULT_MARKDOWN = benchmark.RESULT_DIR / "aa_latest.md"
DEFAULT_JOURNAL = benchmark.RESULT_DIR / "aa_latest.jsonl"


def run_aa_sample(run, iters, label):
    sample = benchmark.run_cpp_dpi_sample(run, iters, "tracked")
    sample["label"] = label
    return sample


def _annotate(sample, label, sequence_index, slot, order, requested_pairs, digest):
    sample.update(
        {
            "label": label,
            "binary_sha256": digest,
            "sequence_index": sequence_index,
            "slot": slot,
            "pair_order": order,
            "requested_pair_count": requested_pairs,
            "measured_pair_count": max(0, sequence_index - (slot == 1)),
        }
    )
    return sample


def collect_aa_pairs(
    iters,
    pair_count,
    start_run=1,
    sample_runner=None,
    journal=None,
    requested_pairs=None,
    digest=None,
):
    if pair_count <= 0 or pair_count % 2:
        raise ValueError("A/A pair count must be positive and even")
    sample_runner = sample_runner or run_aa_sample
    requested_pairs = requested_pairs or pair_count
    digest = digest if digest is not None else benchmark.binary_sha256(
        benchmark.CPP_DPI_BINARY
    )
    a_samples = []
    b_samples = []

    for sequence_index in range(start_run, start_run + pair_count):
        if sequence_index % 2:
            order = ["A", "B"]
            first_label, second_label = order
        else:
            order = ["B", "A"]
            first_label, second_label = order

        first = _annotate(
            sample_runner(sequence_index, iters, first_label),
            first_label,
            sequence_index,
            1,
            order,
            requested_pairs,
            digest,
        )
        if journal:
            journal.append({"event": "sample", "label": first_label, "sample": first})
        second = _annotate(
            sample_runner(sequence_index, iters, second_label),
            second_label,
            sequence_index,
            2,
            order,
            requested_pairs,
            digest,
        )
        if journal:
            journal.append({"event": "sample", "label": second_label, "sample": second})

        by_label = {first_label: first, second_label: second}
        a_samples.append(by_label["A"])
        b_samples.append(by_label["B"])

    return a_samples, b_samples


def aa_statistics(a_samples, b_samples):
    if len(a_samples) != len(b_samples) or not a_samples or len(a_samples) % 2:
        return {
            "status": "invalid_environment",
            "error": "A/A statistics require matching, non-empty, even sample counts",
        }

    try:
        benchmark.assert_same_workload({"A": a_samples, "B": b_samples})
    except SystemExit as error:
        return {
            "status": "invalid_environment",
            "error": str(error),
            "paired_ratios": [],
        }

    a_summary = benchmark.summarize("A", a_samples)
    b_summary = benchmark.summarize("B", b_samples)
    comparison = benchmark.paired_ratio_statistics(b_summary, a_summary, "B", "A")
    if comparison["status"] == "invalid_samples":
        comparison["status"] = "invalid_environment"
        return comparison
    if comparison["ratio"] <= 0:
        return {
            "status": "invalid_environment",
            "error": "A/A paired ratios must be positive",
            "paired_ratios": comparison["paired_ratios"],
        }

    a_by_run = {int(sample["run"]): sample for sample in a_samples}
    b_by_run = {int(sample["run"]): sample for sample in b_samples}
    a_first = []
    b_first = []
    slot_ratios = []
    ordered_pair_ratios = []
    for run_id, pair_ratio in zip(sorted(a_by_run), comparison["paired_ratios"]):
        order = a_by_run[run_id].get("pair_order")
        if order != b_by_run[run_id].get("pair_order"):
            return {"status": "invalid_environment", "error": "mismatched pair order"}
        if order == ["A", "B"]:
            a_first.append(pair_ratio)
            slot_ratios.append(pair_ratio)
        elif order == ["B", "A"]:
            b_first.append(pair_ratio)
            slot_ratios.append(1.0 / pair_ratio)
        else:
            return {"status": "invalid_environment", "error": "invalid pair order"}
        ordered_pair_ratios.append(pair_ratio)

    if len(a_first) != len(b_first):
        return {"status": "invalid_environment", "error": "unbalanced A/B ordering"}

    midpoint = len(ordered_pair_ratios) // 2
    first_half_median = benchmark.median(ordered_pair_ratios[:midpoint])
    second_half_median = benchmark.median(ordered_pair_ratios[midpoint:])
    comparison.update(
        {
            "a_first_paired_median": benchmark.median(a_first),
            "b_first_paired_median": benchmark.median(b_first),
            "second_slot_over_first_slot_median": benchmark.median(slot_ratios),
            "first_half_paired_median": first_half_median,
            "second_half_paired_median": second_half_median,
            "half_split_drift": abs(second_half_median / first_half_median - 1.0),
            "order_stratum_gap": abs(
                benchmark.median(a_first) - benchmark.median(b_first)
            )
            / comparison["ratio"],
        }
    )
    return comparison


def classify_aa(statistics):
    if statistics.get("status") == "invalid_environment":
        return "invalid_environment"
    ratio = statistics["ratio"]
    a_first = statistics["a_first_paired_median"]
    b_first = statistics["b_first_paired_median"]
    interval = statistics["two_sided_95_median_ci"]
    ci_contains_one = interval["lower"] <= 1.0 <= interval["upper"]

    if (
        ci_contains_one
        and 0.98 <= ratio <= 1.02
        and 0.97 <= a_first <= 1.03
        and 0.97 <= b_first <= 1.03
    ):
        return "passed"
    if (
        not ci_contains_one
        or not 0.95 <= a_first <= 1.05
        or not 0.95 <= b_first <= 1.05
        or statistics["order_stratum_gap"] > 0.05
    ):
        return "failed"
    return "inconclusive"


def run_aa_comparison(iters, sample_runner=None, journal=None):
    sample_runner = sample_runner or run_aa_sample
    digest = benchmark.binary_sha256(benchmark.CPP_DPI_BINARY)
    warmups = []
    for slot, label in enumerate(("A", "B"), start=1):
        sample = _annotate(
            sample_runner(0, iters, label),
            label,
            0,
            slot,
            ["A", "B"],
            INITIAL_PAIRS,
            digest,
        )
        sample["warmup"] = True
        warmups.append(sample)
        if journal:
            journal.append({"event": "sample", "label": label, "sample": sample})

    try:
        benchmark.assert_same_workload({"A": [warmups[0]], "B": [warmups[1]]})
    except SystemExit as error:
        return {
            "status": "invalid_environment",
            "error": str(error),
            "warmup_samples": warmups,
            "requested_pairs": INITIAL_PAIRS,
            "measured_pairs": 0,
            "extra_batch_collected": False,
            "binary_sha256": digest,
        }

    a_samples, b_samples = collect_aa_pairs(
        iters,
        INITIAL_PAIRS,
        sample_runner=sample_runner,
        journal=journal,
        requested_pairs=INITIAL_PAIRS,
        digest=digest,
    )
    statistics = aa_statistics(a_samples, b_samples)
    status = classify_aa(statistics)
    extra_batch_collected = status == "inconclusive"
    if extra_batch_collected:
        extra_a, extra_b = collect_aa_pairs(
            iters,
            EXTRA_PAIRS,
            start_run=INITIAL_PAIRS + 1,
            sample_runner=sample_runner,
            journal=journal,
            requested_pairs=INITIAL_PAIRS + EXTRA_PAIRS,
            digest=digest,
        )
        a_samples.extend(extra_a)
        b_samples.extend(extra_b)
        statistics = aa_statistics(a_samples, b_samples)
        status = classify_aa(statistics)

    result = {
        "status": status,
        "requested_pairs": INITIAL_PAIRS,
        "measured_pairs": len(a_samples),
        "extra_batch_collected": extra_batch_collected,
        "binary": str(benchmark.CPP_DPI_BINARY),
        "binary_sha256": digest,
        "warmup_samples": warmups,
        "A": benchmark.summarize("A", a_samples),
        "B": benchmark.summarize("B", b_samples),
        "statistics": statistics,
    }
    if status == "invalid_environment":
        result["error"] = statistics.get("error", "invalid A/A samples")
    return result


def render_markdown(result):
    lines = [
        "# Peripheral-suite C++ DPI A/A diagnostic",
        "",
        f"- Status: `{result['status']}`",
        f"- Measured pairs: `{result['measured_pairs']}`",
        f"- Extra batch collected: `{str(result['extra_batch_collected']).lower()}`",
    ]
    statistics = result.get("statistics")
    if statistics and "ratio" in statistics:
        interval = statistics["two_sided_95_median_ci"]
        lines.extend(
            [
                f"- Paired B/A median: `{statistics['ratio']:.4f}x`",
                f"- Exact two-sided 95% CI: `[{interval['lower']:.4f}, {interval['upper']:.4f}]x`",
                f"- A-first median: `{statistics['a_first_paired_median']:.4f}x`",
                f"- B-first median: `{statistics['b_first_paired_median']:.4f}x`",
                "- Second-slot/first-slot median: "
                f"`{statistics['second_slot_over_first_slot_median']:.4f}x`",
                f"- Half-split drift: `{statistics['half_split_drift']:.2%}`",
            ]
        )
    if result.get("error"):
        lines.append(f"- Error: {result['error']}")
    return "\n".join(lines) + "\n"


def _parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=benchmark.DEFAULT_ITERATIONS)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be greater than zero")
    return args


def main(argv=None):
    args = _parse_args(argv)
    benchmark.RESULT_DIR.mkdir(parents=True, exist_ok=True)
    journal = benchmark.SampleJournal(DEFAULT_JOURNAL, truncate=True)
    try:
        if not args.skip_build:
            benchmark.run_command(["make", "peripheral-suite-dpi-build"])
        result = run_aa_comparison(args.iters, journal=journal)
    except (benchmark.CommandExecutionError, benchmark.ResultParseError) as error:
        samples = benchmark._journal_samples(journal)
        result = {
            "status": "command_error",
            "error": str(error),
            "command_failure": {
                "command": getattr(error, "command", None),
                "returncode": getattr(error, "returncode", None),
                "output": error.output,
            },
            "requested_pairs": INITIAL_PAIRS,
            "measured_pairs": len(
                {
                    entry["sample"].get("sequence_index")
                    for entry in samples
                    if not entry["sample"].get("warmup")
                }
            ),
            "extra_batch_collected": False,
            "samples": samples,
        }

    config = {
        "iterations": args.iters,
        "initial_pairs": INITIAL_PAIRS,
        "extra_pairs": EXTRA_PAIRS,
        "warmups_per_label": 1,
        "binary": str(benchmark.CPP_DPI_BINARY),
        "binary_sha256": benchmark.binary_sha256(benchmark.CPP_DPI_BINARY),
        "argv": [str(benchmark.CPP_DPI_BINARY), f"+PERIPHERAL_SUITE_ITERS={args.iters}"],
        "environment": "identical tracked DPI environment for A and B",
        "skip_build": args.skip_build,
    }
    result["metadata"] = benchmark.collect_metadata(config)
    benchmark.atomic_write_json(DEFAULT_JSON, result)
    benchmark.atomic_write_text(DEFAULT_MARKDOWN, render_markdown(result))
    print(render_markdown(result), end="")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
