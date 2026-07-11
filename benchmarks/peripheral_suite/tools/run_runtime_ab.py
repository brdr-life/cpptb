#!/usr/bin/env python3
"""Isolated reconstructed-old versus current-runtime C++ DPI diagnostic."""

import argparse
import datetime
import hashlib
import json
import math
import sys
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
REPO = BENCH_DIR.parents[1]
sys.path.insert(0, str(BENCH_DIR))
import run_benchmark as benchmark  # noqa: E402


INITIAL_PAIRS = 16
EXTRA_PAIRS = 16
MATERIAL_RATIO = 1.05
MAX_DIAGNOSTIC_DEVIATION = 0.05
RESULT_IDENTITY = "CPP_DPI_PERIPHERAL_RESULT"

OLD_BINARY = REPO / "build" / "diagnostics" / "runtime_old_obj" / "Vdpi_peripheral_suite"
NEW_BINARY = benchmark.CPP_DPI_BINARY
SNAPSHOT_ROOT = REPO / "benchmarks" / "diagnostics" / "runtime_old"
SOURCE_PROVENANCE = SNAPSHOT_ROOT / "provenance.json"
BUILD_PROVENANCE = SNAPSHOT_ROOT / "build_provenance.json"

DEFAULT_JSON = benchmark.RESULT_DIR / "runtime_ab_latest.json"
DEFAULT_MARKDOWN = benchmark.RESULT_DIR / "runtime_ab_latest.md"
DEFAULT_JOURNAL = benchmark.RESULT_DIR / "runtime_ab_latest.jsonl"
DEFAULT_AA_ARTIFACT = benchmark.RESULT_DIR / "aa_latest.json"
AA_MAX_AGE = datetime.timedelta(minutes=30)


class AAPreflightError(RuntimeError):
    def __init__(self, message, preflight):
        super().__init__(message)
        self.preflight = preflight


def _sha256(path):
    return benchmark.binary_sha256(path)


def _capture_git_state():
    capture = getattr(benchmark, "capture_git_state", None)
    if capture:
        return capture()
    revision = benchmark._metadata_command(["git", "rev-parse", "HEAD"])
    status = benchmark._metadata_command(["git", "status", "--porcelain"])
    return {
        "commit": revision["output"] if revision["returncode"] == 0 else None,
        "dirty": status["returncode"] != 0 or bool(status["output"]),
    }


def _pair_boundary_probe(run_id, phase, probe_runner=None):
    if probe_runner:
        evidence = probe_runner(run_id, phase)
    else:
        evidence = {
            "sample_environment": benchmark.sample_environment(),
            "power_thermal": benchmark.probe_pair_boundary(),
        }
    return {
        "event": "environment_probe",
        "reference": f"{phase}:{run_id}",
        "phase": phase,
        "sequence_index": run_id,
        **evidence,
    }


def _environment_validity(probes):
    assess = getattr(benchmark, "environment_validity", None)
    if not assess:
        return {"status": "unknown", "reason": "environment helper unavailable"}
    samples = [probe.get("sample_environment", {}) for probe in probes]
    boundaries = [probe.get("power_thermal", {}) for probe in probes]
    return assess(samples, boundaries)


def _environment_is_invalid(validity):
    if isinstance(validity, str):
        return validity in {"invalid", "invalid_environment"}
    if not isinstance(validity, dict):
        return False
    return validity.get("status") in {"invalid", "invalid_environment"} or validity.get(
        "valid"
    ) is False


def _journal_probes(journal):
    if not journal or not journal.path.exists():
        return []
    return [
        entry
        for entry in (
            json.loads(line)
            for line in journal.path.read_text(encoding="utf-8").splitlines()
        )
        if entry.get("event") == "environment_probe"
    ]


def validate_aa_preflight(
    path, iters, new_binary_sha256, now=None, current_git_commit=None
):
    path = Path(path)
    preflight = {
        "artifact_path": str(path),
        "artifact_sha256": None,
        "artifact_timestamp_utc": None,
        "artifact_status": None,
        "maximum_age_seconds": int(AA_MAX_AGE.total_seconds()),
        "current_git_commit": current_git_commit,
    }

    def reject(message):
        raise AAPreflightError(f"A/A preflight rejected: {message}", preflight)

    try:
        raw = path.read_bytes()
    except OSError as error:
        reject(f"cannot read {path}: {error}")
    preflight["artifact_sha256"] = hashlib.sha256(raw).hexdigest()
    try:
        artifact = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        reject(f"malformed JSON in {path}: {error}")
    if not isinstance(artifact, dict):
        reject("artifact root must be a JSON object")

    preflight["artifact_status"] = artifact.get("status")
    preflight["artifact_binary_sha256"] = artifact.get("binary_sha256")
    preflight["artifact_measured_pairs"] = artifact.get("measured_pairs")
    metadata = artifact.get("metadata")
    git_state = artifact.get("git_state")
    artifact_start = git_state.get("start") if isinstance(git_state, dict) else None
    artifact_git_commit = (
        artifact_start.get("commit") if isinstance(artifact_start, dict) else None
    )
    if artifact_git_commit is None and isinstance(metadata, dict):
        metadata_git = metadata.get("git")
        if isinstance(metadata_git, dict):
            artifact_git_commit = metadata_git.get("commit")
    preflight["artifact_git_commit"] = artifact_git_commit
    config = metadata.get("config") if isinstance(metadata, dict) else None
    timestamp_text = metadata.get("timestamp_utc") if isinstance(metadata, dict) else None
    artifact_iters = config.get("iterations") if isinstance(config, dict) else None
    preflight["artifact_iterations"] = artifact_iters
    preflight["artifact_timestamp_utc"] = timestamp_text

    if artifact.get("status") != "passed":
        reject("artifact status is not passed")
    if artifact.get("binary_sha256") != new_binary_sha256:
        reject("artifact binary SHA256 does not match the current binary")
    if current_git_commit is not None and artifact_git_commit != current_git_commit:
        reject("artifact git commit does not match the current start commit")
    if isinstance(artifact_iters, bool) or artifact_iters != iters:
        reject("artifact iterations do not match requested runtime A/B iterations")
    measured_pairs = artifact.get("measured_pairs")
    if isinstance(measured_pairs, bool) or not isinstance(measured_pairs, int):
        reject("artifact measured_pairs must be an integer")
    if measured_pairs < 20:
        reject("artifact measured_pairs is less than 20")
    if not isinstance(timestamp_text, str):
        reject("artifact metadata timestamp is missing or malformed")
    try:
        timestamp = datetime.datetime.fromisoformat(timestamp_text.replace("Z", "+00:00"))
    except ValueError:
        reject("artifact metadata timestamp is malformed")
    if timestamp.tzinfo is None or timestamp.utcoffset() is None:
        reject("artifact metadata timestamp must include a UTC offset")
    now = now or datetime.datetime.now(datetime.timezone.utc)
    if now.tzinfo is None or now.utcoffset() is None:
        raise ValueError("preflight current time must include a UTC offset")
    age = now.astimezone(datetime.timezone.utc) - timestamp.astimezone(
        datetime.timezone.utc
    )
    preflight["artifact_age_seconds"] = age.total_seconds()
    if age < datetime.timedelta(0):
        reject("artifact timestamp is in the future")
    if age > AA_MAX_AGE:
        reject("artifact is older than 30 minutes")
    return preflight


def load_source_evidence():
    provenance = json.loads(SOURCE_PROVENANCE.read_text(encoding="utf-8"))
    source_hashes = {}
    for entry in provenance["files"]:
        relative_path = entry["snapshot_path"]
        actual = _sha256(SNAPSHOT_ROOT / relative_path)
        if actual != entry["sha256"]:
            raise RuntimeError(
                f"snapshot source hash mismatch for {relative_path}: "
                f"{actual} != {entry['sha256']}"
            )
        source_hashes[relative_path] = actual
    confirmations = provenance["runtime_confirmation_captures"]
    if not confirmations or not all(
        item.get("byte_identical_to_accepted") for item in confirmations
    ):
        raise RuntimeError("runtime confirmation captures are not byte-identical")
    build_provenance = json.loads(BUILD_PROVENANCE.read_text(encoding="utf-8"))
    for relative_path, expected in build_provenance["input_sha256"].items():
        actual = _sha256(REPO / relative_path)
        if actual != expected:
            raise RuntimeError(
                f"documented build input hash mismatch for {relative_path}: "
                f"{actual} != {expected}"
            )
    return {
        "source_sha256": source_hashes,
        "source_provenance_sha256": _sha256(SOURCE_PROVENANCE),
        "source_provenance": provenance,
        "build_provenance": build_provenance,
        "build_provenance_sha256": _sha256(BUILD_PROVENANCE),
    }


def runtime_command(binary, iters):
    return [str(binary), f"+PERIPHERAL_SUITE_ITERS={iters}"]


def run_runtime_sample(run, iters, label, binary=None):
    if label not in {"old", "new"}:
        raise ValueError(f"unknown runtime label: {label}")
    binary = Path(binary or (OLD_BINARY if label == "old" else NEW_BINARY))
    command = runtime_command(binary, iters)
    command_result = benchmark.run_command(
        command, env=benchmark._dpi_environment("tracked")
    )
    output, process_ms = command_result[:2]
    resources = command_result[2] if len(command_result) > 2 else None
    fields = benchmark.parse_result(output, RESULT_IDENTITY)
    return {
        "run": run,
        "label": label,
        "command": command,
        "result_identity": RESULT_IDENTITY,
        "iterations": int(fields["iterations"]),
        "internal_wall_ms": float(fields["wall_ms"]),
        "process_wall_ms": process_ms,
        "checks": int(fields["checks"]),
        "sim_cycles": int(fields["sim_cycles"]),
        "failures": int(fields["failures"]),
        "resources": resources,
    }


def _annotate(sample, label, sequence_index, slot, pair_order, digest, warmup=False):
    sample.update(
        {
            "label": label,
            "sequence_index": sequence_index,
            "slot": slot,
            "pair_order": pair_order,
            "binary_sha256": digest,
            "warmup": warmup,
        }
    )
    return sample


def assert_same_runtime_workload(sample_sets):
    benchmark.assert_same_workload(sample_sets)
    by_label = {
        label: {int(sample["run"]): sample for sample in samples}
        for label, samples in sample_sets.items()
    }
    run_ids = sorted(next(iter(by_label.values())))
    for run_id in run_ids:
        identities = {
            label: samples[run_id].get("result_identity")
            for label, samples in by_label.items()
        }
        if len(set(identities.values())) != 1 or next(iter(identities.values())) != RESULT_IDENTITY:
            raise SystemExit(f"result identity mismatch in run {run_id}: {identities}")
        iterations = {
            label: int(samples[run_id].get("iterations", -1))
            for label, samples in by_label.items()
        }
        if len(set(iterations.values())) != 1:
            raise SystemExit(f"iteration mismatch in run {run_id}: {iterations}")


def collect_pairs(
    iters,
    pair_count,
    start_run=1,
    sample_runner=None,
    journal=None,
    binary_hashes=None,
    probe_runner=None,
    probes=None,
):
    if pair_count <= 0 or pair_count % 2:
        raise ValueError("runtime A/B pair count must be positive and even")
    sample_runner = sample_runner or run_runtime_sample
    binary_hashes = binary_hashes or {
        "old": _sha256(OLD_BINARY),
        "new": _sha256(NEW_BINARY),
    }
    samples = {"old": [], "new": []}
    probes = probes if probes is not None else []
    for run_id in range(start_run, start_run + pair_count):
        order = ["old", "new"] if run_id % 2 else ["new", "old"]
        probe = _pair_boundary_probe(run_id, "measured", probe_runner)
        probes.append(probe)
        if journal:
            journal.append(probe)
        pair = {}
        for slot, label in enumerate(order, start=1):
            sample = _annotate(
                sample_runner(run_id, iters, label),
                label,
                run_id,
                slot,
                order,
                binary_hashes[label],
            )
            sample["environment_probe_reference"] = probe["reference"]
            pair[label] = sample
            if journal:
                journal.append({"event": "sample", "label": label, "sample": sample})
        samples["old"].append(pair["old"])
        samples["new"].append(pair["new"])
    return samples["old"], samples["new"]


def runtime_statistics(old_samples, new_samples):
    sample_count = len(old_samples)
    if sample_count != len(new_samples) or not sample_count or sample_count % 2:
        return {
            "status": "invalid_environment",
            "error": "runtime A/B statistics require matching non-empty even samples",
        }
    try:
        assert_same_runtime_workload({"old": old_samples, "new": new_samples})
    except (SystemExit, KeyError, TypeError, ValueError) as error:
        return {"status": "invalid_environment", "error": str(error)}

    old_summary = benchmark.summarize("old", old_samples)
    new_summary = benchmark.summarize("new", new_samples)
    result = benchmark.paired_ratio_statistics(new_summary, old_summary, "new", "old")
    if result["status"] != "ok":
        result["status"] = "invalid_environment"
        return result

    old_by_run = {int(sample["run"]): sample for sample in old_samples}
    new_by_run = {int(sample["run"]): sample for sample in new_samples}
    order_strata = {"old_first": [], "new_first": []}
    slot_ratios = []
    ordered_ratios = []
    for run_id, ratio in zip(sorted(old_by_run), result["paired_ratios"]):
        old_sample = old_by_run[run_id]
        new_sample = new_by_run[run_id]
        order = old_sample.get("pair_order")
        if order != new_sample.get("pair_order"):
            return {"status": "invalid_environment", "error": "mismatched pair order"}
        if order == ["old", "new"] and old_sample.get("slot") == 1 and new_sample.get("slot") == 2:
            order_strata["old_first"].append(ratio)
            first, second = old_sample, new_sample
        elif order == ["new", "old"] and new_sample.get("slot") == 1 and old_sample.get("slot") == 2:
            order_strata["new_first"].append(ratio)
            first, second = new_sample, old_sample
        else:
            return {"status": "invalid_environment", "error": "invalid pair order or slot"}
        first_ms = float(first["process_wall_ms"])
        second_ms = float(second["process_wall_ms"])
        if not math.isfinite(first_ms) or first_ms <= 0 or not math.isfinite(second_ms) or second_ms < 0:
            return {"status": "invalid_environment", "error": "invalid slot timing"}
        slot_ratios.append(second_ms / first_ms)
        ordered_ratios.append(ratio)

    expected_per_stratum = sample_count // 2
    if any(len(values) != expected_per_stratum for values in order_strata.values()):
        return {"status": "invalid_environment", "error": "unbalanced pair ordering"}

    paired_ratio = result["ratio"]
    old_median = float(old_summary["process_wall_ms_median"])
    new_median = float(new_summary["process_wall_ms_median"])
    if (
        not math.isfinite(paired_ratio)
        or paired_ratio <= 0
        or not math.isfinite(old_median)
        or old_median <= 0
        or not math.isfinite(new_median)
        or new_median <= 0
    ):
        return {
            "status": "invalid_environment",
            "error": "paired and independent process medians must be finite and positive",
        }
    independent_ratio = new_median / old_median
    old_first = benchmark.median(order_strata["old_first"])
    new_first = benchmark.median(order_strata["new_first"])
    midpoint = sample_count // 2
    first_half = benchmark.median(ordered_ratios[:midpoint])
    second_half = benchmark.median(ordered_ratios[midpoint:])
    if first_half <= 0 or second_half <= 0:
        return {"status": "invalid_environment", "error": "invalid half medians"}
    independent_disagreement = abs(independent_ratio - paired_ratio) / paired_ratio
    order_gap = abs(old_first - new_first) / paired_ratio
    slot_effect = benchmark.median(slot_ratios)
    half_drift = abs(second_half / first_half - 1.0)
    issues = []
    if independent_disagreement > MAX_DIAGNOSTIC_DEVIATION:
        issues.append("independent ratio disagrees with paired ratio by more than 5%")
    if order_gap > MAX_DIAGNOSTIC_DEVIATION:
        issues.append("order strata differ by more than 5%")
    if abs(slot_effect - 1.0) > MAX_DIAGNOSTIC_DEVIATION:
        issues.append("second-slot/first-slot effect exceeds 5%")
    if half_drift > MAX_DIAGNOSTIC_DEVIATION:
        issues.append("half-split drift exceeds 5%")

    result.update(
        {
            "status": "invalid_environment" if issues else "ok",
            "independent_median_ratio": independent_ratio,
            "independent_paired_relative_disagreement": independent_disagreement,
            "order_strata": order_strata,
            "old_first_paired_median": old_first,
            "new_first_paired_median": new_first,
            "order_stratum_gap": order_gap,
            "second_slot_over_first_slot_ratios": slot_ratios,
            "second_slot_over_first_slot_median": slot_effect,
            "first_half_paired_median": first_half,
            "second_half_paired_median": second_half,
            "half_split_drift": half_drift,
            "environment_issues": issues,
        }
    )
    if issues:
        result["error"] = "; ".join(issues)
    return result


def classify_runtime(statistics):
    if statistics.get("status") != "ok":
        return "invalid_environment"
    if (
        statistics["independent_paired_relative_disagreement"]
        > MAX_DIAGNOSTIC_DEVIATION
    ):
        return "invalid_environment"
    interval = statistics["two_sided_95_median_ci"]
    strata = (
        statistics["old_first_paired_median"],
        statistics["new_first_paired_median"],
    )
    independent_clean = (
        statistics["independent_paired_relative_disagreement"]
        <= MAX_DIAGNOSTIC_DEVIATION
    )
    if (
        interval["lower"] > MATERIAL_RATIO
        and all(value > MATERIAL_RATIO for value in strata)
        and independent_clean
    ):
        return "regression_confirmed"
    if interval["upper"] <= MATERIAL_RATIO:
        return "no_material_regression"
    return "inconclusive"


def run_runtime_comparison(
    iters,
    sample_runner=None,
    journal=None,
    binary_hashes=None,
    probe_runner=None,
):
    sample_runner = sample_runner or run_runtime_sample
    binary_hashes = binary_hashes or {
        "old": _sha256(OLD_BINARY),
        "new": _sha256(NEW_BINARY),
    }
    probes = []
    warmup_probe = _pair_boundary_probe(0, "warmup", probe_runner)
    probes.append(warmup_probe)
    if journal:
        journal.append(warmup_probe)
    warmups = []
    warmup_order = ["old", "new"]
    for slot, label in enumerate(warmup_order, start=1):
        sample = _annotate(
            sample_runner(0, iters, label),
            label,
            0,
            slot,
            warmup_order,
            binary_hashes[label],
            warmup=True,
        )
        sample["environment_probe_reference"] = warmup_probe["reference"]
        warmups.append(sample)
        if journal:
            journal.append({"event": "sample", "label": label, "sample": sample})
    try:
        assert_same_runtime_workload({"old": [warmups[0]], "new": [warmups[1]]})
    except (SystemExit, KeyError, TypeError, ValueError) as error:
        return {
            "status": "invalid_environment",
            "error": str(error),
            "requested_pairs": INITIAL_PAIRS,
            "measured_pairs": 0,
            "extra_batch_collected": False,
            "warmup_samples": warmups,
            "binary_sha256": binary_hashes,
            "environment": {
                "probes": probes,
                "validity": _environment_validity(probes),
            },
        }

    old_samples, new_samples = collect_pairs(
        iters,
        INITIAL_PAIRS,
        sample_runner=sample_runner,
        journal=journal,
        binary_hashes=binary_hashes,
        probe_runner=probe_runner,
        probes=probes,
    )
    statistics = runtime_statistics(old_samples, new_samples)
    status = classify_runtime(statistics)
    extra_batch_collected = status == "inconclusive"
    if extra_batch_collected:
        extra_old, extra_new = collect_pairs(
            iters,
            EXTRA_PAIRS,
            start_run=INITIAL_PAIRS + 1,
            sample_runner=sample_runner,
            journal=journal,
            binary_hashes=binary_hashes,
            probe_runner=probe_runner,
            probes=probes,
        )
        old_samples.extend(extra_old)
        new_samples.extend(extra_new)
        statistics = runtime_statistics(old_samples, new_samples)
        status = classify_runtime(statistics)

    validity = _environment_validity(probes)
    if status == "regression_confirmed" and _environment_is_invalid(validity):
        status = "invalid_environment"
        statistics = dict(statistics)
        statistics["error"] = "regression evidence was collected in an invalid environment"

    result = {
        "benchmark": "reconstructed_old_vs_current_cpp_dpi_runtime",
        "status": status,
        "requested_pairs": INITIAL_PAIRS,
        "maximum_pairs": INITIAL_PAIRS + EXTRA_PAIRS,
        "measured_pairs": len(old_samples),
        "extra_batch_collected": extra_batch_collected,
        "warmup_samples": warmups,
        "binary_sha256": binary_hashes,
        "old": benchmark.summarize("old", old_samples),
        "new": benchmark.summarize("new", new_samples),
        "statistics": statistics,
        "environment": {"probes": probes, "validity": validity},
    }
    if status == "invalid_environment":
        result["error"] = statistics.get("error", "invalid runtime A/B environment")
    return result


def render_markdown(result):
    lines = [
        "# Reconstructed old/current runtime A/B diagnostic",
        "",
        f"- Status: `{result['status']}`",
        f"- Measured pairs: `{result.get('measured_pairs', 0)}`",
        f"- Extra batch collected: `{str(result.get('extra_batch_collected', False)).lower()}`",
    ]
    statistics = result.get("statistics", {})
    if "ratio" in statistics:
        interval = statistics["two_sided_95_median_ci"]
        lines.extend(
            [
                f"- Paired new/old median: `{statistics['ratio']:.4f}x`",
                f"- Exact two-sided 95% CI: `[{interval['lower']:.4f}, {interval['upper']:.4f}]x`",
                f"- Old-first median: `{statistics['old_first_paired_median']:.4f}x`",
                f"- New-first median: `{statistics['new_first_paired_median']:.4f}x`",
                f"- Independent new/old ratio: `{statistics['independent_median_ratio']:.4f}x`",
                "- Second-slot/first-slot median: "
                f"`{statistics['second_slot_over_first_slot_median']:.4f}x`",
                f"- Half-split drift: `{statistics['half_split_drift']:.2%}`",
            ]
        )
    if result.get("error"):
        lines.append(f"- Error: {result['error']}")
    lines.extend(["", "Raw samples are retained in the JSON result and durable JSONL journal."])
    return "\n".join(lines) + "\n"


def persist_result(result, json_path=DEFAULT_JSON, markdown_path=DEFAULT_MARKDOWN):
    benchmark.persist_diagnostic_result(
        result, json_path, markdown_path, markdown=render_markdown(result)
    )


def _parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=benchmark.DEFAULT_ITERATIONS)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--old-binary", type=Path, default=OLD_BINARY)
    parser.add_argument("--new-binary", type=Path, default=NEW_BINARY)
    parser.add_argument("--json", type=Path, default=DEFAULT_JSON)
    parser.add_argument("--markdown", type=Path, default=DEFAULT_MARKDOWN)
    parser.add_argument("--journal", type=Path, default=DEFAULT_JOURNAL)
    parser.add_argument("--aa-artifact", type=Path, default=DEFAULT_AA_ARTIFACT)
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be greater than zero")
    return args


def main(argv=None):
    args = _parse_args(argv)
    start_git_state = _capture_git_state()
    journal = benchmark.SampleJournal(args.journal, truncate=True)
    evidence = None
    binary_hashes = None
    aa_preflight = {"artifact_path": str(args.aa_artifact)}
    try:
        if not args.skip_build:
            benchmark.run_command(
                ["make", "peripheral-suite-runtime-old-diagnostic-build"]
            )
        for label, path in (("old", args.old_binary), ("new", args.new_binary)):
            if not path.is_file():
                raise RuntimeError(f"{label} binary does not exist: {path}")
        binary_hashes = {
            "old": _sha256(args.old_binary),
            "new": _sha256(args.new_binary),
        }
        aa_preflight = validate_aa_preflight(
            args.aa_artifact,
            args.iters,
            binary_hashes["new"],
            current_git_commit=start_git_state.get("commit"),
        )
        evidence = load_source_evidence()
        expected_old_hash = evidence["build_provenance"]["binary_sha256"]
        if binary_hashes["old"] != expected_old_hash:
            raise RuntimeError(
                "old binary hash does not match documented reconstruction: "
                f"{binary_hashes['old']} != {expected_old_hash}"
            )

        def sample_runner(run, iters, label):
            binary = args.old_binary if label == "old" else args.new_binary
            return run_runtime_sample(run, iters, label, binary=binary)

        result = run_runtime_comparison(
            args.iters,
            sample_runner=sample_runner,
            journal=journal,
            binary_hashes=binary_hashes,
        )
        final_hashes = {
            "old": _sha256(args.old_binary),
            "new": _sha256(args.new_binary),
        }
        if final_hashes != binary_hashes:
            result["status"] = "invalid_environment"
            result["error"] = "binary hash changed during measurement"
            result["final_binary_sha256"] = final_hashes
    except (
        benchmark.CommandExecutionError,
        benchmark.ResultParseError,
        OSError,
        RuntimeError,
        ValueError,
    ) as error:
        if isinstance(error, AAPreflightError):
            aa_preflight = error.preflight
        result = {
            "benchmark": "reconstructed_old_vs_current_cpp_dpi_runtime",
            "status": "invalid_environment",
            "error": str(error),
            "requested_pairs": INITIAL_PAIRS,
            "measured_pairs": len(
                {
                    entry["sample"].get("run")
                    for entry in benchmark._journal_samples(journal)
                    if not entry["sample"].get("warmup")
                }
            ),
            "extra_batch_collected": False,
            "samples": benchmark._journal_samples(journal),
            "binary_sha256": binary_hashes,
            "aa_preflight": aa_preflight,
            "environment": {
                "probes": _journal_probes(journal),
                "validity": _environment_validity(_journal_probes(journal)),
            },
        }
        if isinstance(error, benchmark.CommandExecutionError):
            result["command_failure"] = {
                "command": error.command,
                "returncode": error.returncode,
                "output": error.output,
                "resources": getattr(error, "resources", None),
            }

    if evidence:
        result.update(evidence)
    result["aa_preflight"] = aa_preflight
    config = {
        "iterations": args.iters,
        "initial_pairs": INITIAL_PAIRS,
        "extra_pairs": EXTRA_PAIRS,
        "warmups_per_binary": 1,
        "strict_pair_order": "old,new on odd runs; new,old on even runs",
        "material_ratio": MATERIAL_RATIO,
        "maximum_diagnostic_deviation": MAX_DIAGNOSTIC_DEVIATION,
        "build_commands": {
            "old": ["make", "peripheral-suite-runtime-old-diagnostic-build"],
            "new": None,
        },
        "run_commands": {
            "old": runtime_command(args.old_binary, args.iters),
            "new": runtime_command(args.new_binary, args.iters),
        },
        "binary_sha256": binary_hashes,
        "skip_build": args.skip_build,
        "aa_preflight": aa_preflight,
    }
    result["metadata"] = benchmark.collect_metadata(
        config, git_state=start_git_state
    )
    result["git_state"] = {
        "start": start_git_state,
        "end": _capture_git_state(),
    }
    persist_result(result, args.json, args.markdown)
    print(render_markdown(result), end="")
    return 0 if result["status"] == "no_material_regression" else 1


if __name__ == "__main__":
    raise SystemExit(main())
