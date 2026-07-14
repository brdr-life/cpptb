#!/usr/bin/env python3
"""Run feature-registry checks and benchmarks without overlapping commands."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable, Iterable, Mapping, Sequence
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
RESULT_DIR = REPO / "benchmarks" / "results" / "regression"
sys.path.insert(0, str(REPO))
DEFAULT_LOAD_THRESHOLD = 1.00
DEFAULT_SETTLE_POLL_SECONDS = 5.0
DEFAULT_SETTLE_TIMEOUT_SECONDS = 60.0
STATUS_PRECEDENCE = {
    "passed": 0,
    "passed_inconclusive": 1,
    "invalid_environment": 2,
    "failed": 3,
}
MULTICLOCK_FIELDS = ("iterations", "checks", "sim_cycles", "failures")
MULTICLOCK_PATTERNS = {
    "cpp_dpi": re.compile(r"^CPP_DPI_[A-Z0-9_]+_RESULT\s+(?P<fields>.+)$"),
    "pure_sv": re.compile(r"^PURE_SV_[A-Z0-9_]+_RESULT\s+(?P<fields>.+)$"),
}


def atomic_write_text(path: Path, text: str) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def atomic_write_json(path: Path, value: Mapping[str, object]) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=True) + "\n")


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def run_command(command: Sequence[str]) -> dict[str, object]:
    """Run exactly one child process, forcing recursive make invocations serial."""
    environment = os.environ.copy()
    environment["MAKEFLAGS"] = "-j1"
    try:
        completed = subprocess.run(
            list(command),
            cwd=REPO,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as error:
        return {
            "command": list(command),
            "returncode": 127,
            "stdout": "",
            "stderr": str(error),
        }
    return {
        "command": list(command),
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def normalized_load_probe() -> dict[str, object]:
    cpu_count = os.cpu_count() or 1
    try:
        load_1m = os.getloadavg()[0]
    except (AttributeError, OSError):
        load_1m = None
    return {
        "timestamp_utc": _utc_now(),
        "logical_cpu_count": cpu_count,
        "load_average_1m": load_1m,
        "normalized_load_1m": load_1m / cpu_count if load_1m is not None else None,
    }


def _normalized_load(probe: Mapping[str, object]) -> float | None:
    value = probe.get("normalized_load_1m")
    if value is None and isinstance(probe.get("load"), Mapping):
        value = probe["load"].get("normalized_load_1m")  # type: ignore[index]
    if value is None:
        return None
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ValueError("normalized load probe must be finite and non-negative")
    return result


def settle_normalized_load(
    probe_runner: Callable[[], Mapping[str, object]] = normalized_load_probe,
    *,
    threshold: float = DEFAULT_LOAD_THRESHOLD,
    poll_seconds: float = DEFAULT_SETTLE_POLL_SECONDS,
    timeout_seconds: float = DEFAULT_SETTLE_TIMEOUT_SECONDS,
    sleep_runner: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
) -> dict[str, object]:
    """Wait for bounded normalized load and preserve every observed probe."""
    if not math.isfinite(threshold) or threshold < 0:
        raise ValueError("load threshold must be finite and non-negative")
    if poll_seconds <= 0 or timeout_seconds < 0:
        raise ValueError("settle poll must be positive and timeout non-negative")
    started = monotonic()
    probes: list[dict[str, object]] = []
    while True:
        probe = dict(probe_runner())
        observed = _normalized_load(probe)
        elapsed = max(0.0, monotonic() - started)
        probes.append(
            {
                **probe,
                "normalized_load_1m": observed,
                "elapsed_seconds": elapsed,
            }
        )
        if observed is None or observed <= threshold:
            return {
                "status": "settled" if observed is not None else "unavailable",
                "threshold": threshold,
                "poll_seconds": poll_seconds,
                "timeout_seconds": timeout_seconds,
                "elapsed_seconds": elapsed,
                "probes": probes,
            }
        if elapsed >= timeout_seconds:
            return {
                "status": "timed_out",
                "threshold": threshold,
                "poll_seconds": poll_seconds,
                "timeout_seconds": timeout_seconds,
                "elapsed_seconds": elapsed,
                "probes": probes,
            }
        sleep_runner(min(poll_seconds, max(0.0, timeout_seconds - elapsed)))


def _entry_value(entry: object, *names: str, default: object = None) -> object:
    for name in names:
        if isinstance(entry, Mapping) and name in entry:
            return entry[name]
        if hasattr(entry, name):
            return getattr(entry, name)
    return default


def feature_id(entry: object) -> str:
    value = _entry_value(entry, "id", "feature_id", "slug", "name")
    if not isinstance(value, str) or not value:
        raise ValueError(f"registry entry has no feature id: {entry!r}")
    return value


def _registry_entries(registry: object | None = None) -> list[object]:
    if registry is None:
        errors = []
        for module_name in (
            "benchmarks.registry",
            "benchmarks.feature_registry",
            "registry",
            "feature_registry",
        ):
            try:
                registry = importlib.import_module(module_name)
                break
            except ModuleNotFoundError as error:
                errors.append(str(error))
        else:
            raise RuntimeError("feature registry is unavailable: " + "; ".join(errors))
    if isinstance(registry, Mapping):
        values: object = list(registry.values())
    elif isinstance(registry, Iterable) and not isinstance(registry, (str, bytes)):
        values = list(registry)
    else:
        values = None
        for name in (
            "list_benchmarks",
            "all_features",
            "get_features",
            "entries",
            "features",
            "BENCHMARKS",
            "ENTRIES",
            "FEATURES",
            "REGISTRY",
            "FEATURE_REGISTRY",
        ):
            candidate = getattr(registry, name, None)
            if candidate is not None:
                values = candidate() if callable(candidate) else candidate
                break
    if isinstance(values, Mapping):
        values = list(values.values())
    if not isinstance(values, Iterable) or isinstance(values, (str, bytes)):
        raise TypeError("registry API did not provide iterable feature entries")
    entries = list(values)
    ids = [feature_id(entry) for entry in entries]
    if len(ids) != len(set(ids)):
        raise ValueError("registry contains duplicate feature ids")
    return entries


def validate_registry(registry: object | None = None) -> list[object]:
    if registry is None:
        registry = importlib.import_module("benchmarks.registry")
    entries = _registry_entries(registry)
    if registry is not None and not isinstance(registry, (Mapping, list, tuple, set)):
        for name in (
            "check_consistency",
            "validate_registry",
            "validate",
            "check_registry",
        ):
            validator = getattr(registry, name, None)
            if callable(validator):
                result = validator()
                if result is False:
                    raise ValueError("registry validation failed")
                break
    for entry in entries:
        feature_id(entry)
        if _has_benchmark(entry):
            _benchmark_commands(entry)
    return entries


def select_feature(entries: Iterable[object], requested: str) -> object:
    matches = [entry for entry in entries if feature_id(entry) == requested]
    if not matches:
        available = ", ".join(feature_id(entry) for entry in entries)
        raise KeyError(f"unknown feature {requested!r}; available: {available}")
    return matches[0]


def _nested(entry: object, name: str) -> object | None:
    value = _entry_value(entry, name)
    return None if value is None else value


def _coerce_command(value: object, *, context: str) -> list[str]:
    if isinstance(value, str):
        raise TypeError(f"{context} command must be an argument list, not shell text")
    if (
        not isinstance(value, Sequence)
        or not value
        or not all(isinstance(item, str) for item in value)
    ):
        raise TypeError(f"{context} command must be a non-empty argument list")
    command = list(value)
    if any(item == "-j" or item.startswith("-j") for item in command):
        raise ValueError(f"{context} command may not enable parallel make")
    return command


def _command_list(entry: object, kind: str, *, required: bool = False) -> list[str] | None:
    block = _nested(entry, kind)
    value = _entry_value(entry, f"{kind}_command", f"{kind}_cmd")
    if value is None and block is not None:
        value = _entry_value(block, "command", "cmd")
    if value is None and kind == "semantic":
        value = _entry_value(entry, "test_command", "check_command")
    if value is None and kind == "benchmark":
        runner = _entry_value(entry, "runner")
        commands = _entry_value(runner, "commands") if runner is not None else None
        if isinstance(commands, Sequence) and len(commands) == 1:
            value = commands[0]
    if value is None:
        if required:
            raise ValueError(f"{feature_id(entry)} has no {kind} command")
        return None
    return _coerce_command(value, context=f"{feature_id(entry)} {kind}")


def _has_benchmark(entry: object) -> bool:
    enabled = _entry_value(entry, "benchmark_enabled", default=None)
    if enabled is False:
        return False
    return any(
        value is not None
        for value in (
            _nested(entry, "benchmark"),
            _entry_value(entry, "benchmark_command", "benchmark_cmd"),
            _entry_value(entry, "cpp_command", "cpp_dpi_command"),
            _entry_value(_entry_value(entry, "runner"), "commands"),
        )
    )


def _build_commands(entry: object, *, semantic: bool = False) -> list[list[str]]:
    block = _nested(entry, "benchmark")
    value = _entry_value(entry, "build_commands", "build_command", "build_cmd")
    if semantic:
        runner = _entry_value(entry, "runner")
        semantic_targets = _entry_value(runner, "semantic_build_targets")
        if isinstance(semantic_targets, Sequence) and semantic_targets:
            value = ["make", *semantic_targets]
    if value is None and block is not None:
        value = _entry_value(block, "build_commands", "build_command", "build_cmd")
    if value is None:
        targets = _entry_value(entry, "build_targets")
        if isinstance(targets, Sequence) and targets:
            value = ["make", *targets]
    if value is None:
        return []
    if isinstance(value, Sequence) and value and all(isinstance(item, str) for item in value):
        return [_coerce_command(value, context=f"{feature_id(entry)} build")]
    if not isinstance(value, Sequence):
        raise TypeError(f"{feature_id(entry)} build commands must be a sequence")
    return [
        _coerce_command(command, context=f"{feature_id(entry)} build")
        for command in value
    ]


def _adapter(entry: object) -> str:
    block = _nested(entry, "benchmark")
    value = _entry_value(entry, "adapter", "benchmark_adapter", default=None)
    if value is None and block is not None:
        value = _entry_value(block, "adapter", "kind", "mode", default=None)
    value = getattr(value, "value", value)
    gate = _entry_value(entry, "gate_policy")
    gate = getattr(gate, "value", gate)
    if value == "dpi_multiclock" or gate == "equivalence_only":
        return "equivalence_only"
    return str(value or "runner")


def _gate_policy(entry: object) -> str:
    value = _entry_value(entry, "gate_policy", default="hard_1_10")
    return str(getattr(value, "value", value))


def _waiver_metadata(entry: object) -> dict[str, object] | None:
    waiver = _entry_value(entry, "waiver")
    if waiver is None:
        return None
    max_ratio = _entry_value(waiver, "max_ratio")
    approved_on = _entry_value(waiver, "approved_on")
    rationale = _entry_value(waiver, "rationale")
    if not isinstance(max_ratio, (int, float)) or not math.isfinite(max_ratio):
        raise ValueError(f"{feature_id(entry)} waiver max_ratio must be finite")
    if max_ratio <= 1.10:
        raise ValueError(f"{feature_id(entry)} waiver max_ratio must exceed 1.10")
    if not isinstance(approved_on, str) or not approved_on:
        raise ValueError(f"{feature_id(entry)} waiver approved_on is required")
    if not isinstance(rationale, str) or not rationale.strip():
        raise ValueError(f"{feature_id(entry)} waiver rationale is required")
    return {
        "approved_on": approved_on,
        "max_ratio": float(max_ratio),
        "rationale": rationale,
    }


def _authoring_guard_ratio(
    payload: Mapping[str, object], feature: str
) -> float | None:
    kernels = payload.get("kernels")
    if not isinstance(kernels, Mapping):
        return None
    summary = kernels.get(feature)
    if not isinstance(summary, Mapping):
        return None
    guard = summary.get("guard")
    if not isinstance(guard, Mapping):
        return None
    ratio = guard.get("ratio")
    if not isinstance(ratio, (int, float)) or not math.isfinite(ratio):
        return None
    return float(ratio)


def _registry_runner_commands(entry: object) -> list[tuple[str, list[str]]] | None:
    runner = _entry_value(entry, "runner")
    commands = _entry_value(runner, "commands") if runner is not None else None
    if not isinstance(commands, Sequence) or not commands:
        return None
    prepared: list[tuple[str, list[str]]] = []
    labels = (
        ("cpp_dpi", "pure_sv")
        if _adapter(entry) == "equivalence_only"
        else ("runner",)
    )
    if len(commands) != len(labels):
        raise ValueError(
            f"{feature_id(entry)} adapter expects {len(labels)} runner "
            f"command(s), got {len(commands)}"
        )
    for label, raw_command in zip(labels, commands):
        command = _coerce_command(
            raw_command, context=f"{feature_id(entry)} {label}"
        )
        if _adapter(entry) != "equivalence_only":
            iterations_argument = _entry_value(runner, "iterations_argument")
            if iterations_argument:
                command.extend(
                    [
                        str(iterations_argument),
                        str(_entry_value(entry, "default_iterations")),
                    ]
                )
            kernel_argument = _entry_value(runner, "kernel_argument")
            if kernel_argument:
                # The authoring runner's registry-facing selector is --example.
                argument = (
                    "--example"
                    if str(kernel_argument) == "--kernels"
                    else str(kernel_argument)
                )
                command.extend([argument, feature_id(entry)])
            if "run_benchmark.py" in " ".join(command):
                command.append("--skip-build")
        prepared.append((label, command))
    return prepared


def _benchmark_commands(entry: object) -> list[tuple[str, list[str]]]:
    block = _nested(entry, "benchmark")
    adapter = _adapter(entry)
    registry_commands = _registry_runner_commands(entry)
    if registry_commands is not None:
        return registry_commands
    if adapter in {"multiclock", "equivalence_only"}:
        cpp = _entry_value(entry, "cpp_command", "cpp_dpi_command")
        sv = _entry_value(entry, "sv_command", "pure_sv_command")
        if block is not None:
            cpp = cpp or _entry_value(block, "cpp_command", "cpp_dpi_command")
            sv = sv or _entry_value(block, "sv_command", "pure_sv_command")
        return [
            ("cpp_dpi", _coerce_command(cpp, context=f"{feature_id(entry)} C++ DPI")),
            ("pure_sv", _coerce_command(sv, context=f"{feature_id(entry)} pure SV")),
        ]
    command = _command_list(entry, "benchmark", required=True)
    assert command is not None
    return [("runner", command)]


def _command_result(command: Sequence[str], value: object) -> dict[str, object]:
    if isinstance(value, subprocess.CompletedProcess):
        result = {
            "returncode": value.returncode,
            "stdout": value.stdout or "",
            "stderr": value.stderr or "",
        }
    elif isinstance(value, Mapping):
        result = dict(value)
    elif isinstance(value, tuple) and len(value) == 3:
        result = {"returncode": value[0], "stdout": value[1], "stderr": value[2]}
    else:
        raise TypeError("command runner must return a mapping, CompletedProcess, or 3-tuple")
    result.setdefault("returncode", 0)
    result.setdefault("stdout", "")
    result.setdefault("stderr", "")
    result["command"] = list(command)
    result["returncode"] = int(result["returncode"])
    result["stdout"] = str(result["stdout"])
    result["stderr"] = str(result["stderr"])
    return result


def _invoke(
    command: Sequence[str], command_runner: Callable[[Sequence[str]], object]
) -> dict[str, object]:
    return _command_result(command, command_runner(list(command)))


def parse_multiclock_result(output: str, mode: str) -> dict[str, int]:
    if mode not in MULTICLOCK_PATTERNS:
        raise ValueError(f"unknown multiclock mode: {mode}")
    matches = [
        match
        for line in output.splitlines()
        if (match := MULTICLOCK_PATTERNS[mode].match(line.strip()))
    ]
    if len(matches) != 1:
        marker = MULTICLOCK_PATTERNS[mode].pattern.split(r"\s+")[0].lstrip("^")
        raise ValueError(f"expected exactly one {marker}, found {len(matches)}")
    fields: dict[str, int] = {}
    tokens = matches[0].group("fields").split()
    keys = tuple(token.split("=", 1)[0] for token in tokens if "=" in token)
    allowed_keys = (
        ("iterations", "checks", "sim_cycles", "wall_ms", "failures")
        if mode == "cpp_dpi"
        else MULTICLOCK_FIELDS
    )
    if keys not in {MULTICLOCK_FIELDS, allowed_keys}:
        raise ValueError(
            f"multiclock result fields must be {MULTICLOCK_FIELDS} with only the "
            f"known C++ wall_ms diagnostic optional, got {keys}"
        )
    for token in tokens:
        if "=" not in token:
            raise ValueError(f"invalid multiclock field token: {token!r}")
        key, raw = token.split("=", 1)
        if key == "wall_ms":
            try:
                wall_ms = float(raw)
            except ValueError as error:
                raise ValueError(f"invalid multiclock wall_ms={raw!r}") from error
            if not math.isfinite(wall_ms) or wall_ms < 0:
                raise ValueError("multiclock wall_ms must be finite and non-negative")
            continue
        if key in fields:
            raise ValueError(f"duplicate multiclock field: {key}")
        try:
            fields[key] = int(raw, 10)
        except ValueError as error:
            raise ValueError(f"invalid multiclock integer {key}={raw!r}") from error
    if tuple(fields) != MULTICLOCK_FIELDS:
        raise ValueError(f"missing multiclock semantic fields: got {tuple(fields)}")
    if any(value < 0 for value in fields.values()):
        raise ValueError("multiclock fields must be non-negative")
    return fields


def compare_multiclock(cpp_output: str, sv_output: str) -> dict[str, object]:
    cpp = parse_multiclock_result(cpp_output, "cpp_dpi")
    sv = parse_multiclock_result(sv_output, "pure_sv")
    mismatches = {
        field: {"cpp_dpi": cpp[field], "pure_sv": sv[field]}
        for field in MULTICLOCK_FIELDS
        if cpp[field] != sv[field]
    }
    status = "passed" if not mismatches and cpp["failures"] == 0 else "failed"
    return {
        "status": status,
        "measurement_mode": "equivalence_only",
        "fields": list(MULTICLOCK_FIELDS),
        "cpp_dpi": cpp,
        "pure_sv": sv,
        "exact_match": not mismatches,
        "mismatches": mismatches,
    }


def normalize_status(value: object, returncode: int = 0) -> str:
    status = str(value or "passed")
    aliases = {
        "success": "passed",
        "ok": "passed",
        "hard_failure": "failed",
        "command_error": "failed",
        "workload_error": "failed",
        "error": "failed",
        "inconclusive": "passed_inconclusive",
    }
    status = aliases.get(status, status)
    if status not in STATUS_PRECEDENCE:
        raise ValueError(f"unknown regression status: {status}")
    if returncode != 0 and status in {"passed", "passed_inconclusive"}:
        return "failed"
    return status


def aggregate_status(statuses: Iterable[str]) -> str:
    values = list(statuses)
    if not values:
        return "passed"
    return max(values, key=STATUS_PRECEDENCE.__getitem__)


def _payload_status(payload: Mapping[str, object], returncode: int = 0) -> str:
    statuses = [
        normalize_status(payload.get("status", payload.get("verdict")), returncode)
    ]
    kernels = payload.get("kernels")
    if isinstance(kernels, Mapping):
        for summary in kernels.values():
            if not isinstance(summary, Mapping):
                continue
            guard = summary.get("guard")
            if isinstance(guard, Mapping):
                statuses.append(normalize_status(guard.get("status", guard.get("verdict"))))
    for key in ("guard", "performance_guard"):
        guard = payload.get(key)
        if isinstance(guard, Mapping):
            statuses.append(normalize_status(guard.get("status", guard.get("verdict"))))
    return aggregate_status(statuses)


def _status_from_runner(result: Mapping[str, object]) -> str:
    payload = result.get("result")
    if isinstance(payload, Mapping):
        return _payload_status(payload, int(result["returncode"]))
    output = str(result.get("stdout", "")).strip()
    if output.startswith("{"):
        try:
            parsed = json.loads(output)
        except json.JSONDecodeError:
            parsed = None
        if isinstance(parsed, Mapping):
            return _payload_status(parsed, int(result["returncode"]))
    return normalize_status(None, int(result["returncode"]))


def _runner_result_path(entry: object) -> Path | None:
    adapter_kind = _entry_value(entry, "adapter_kind")
    adapter_kind = getattr(adapter_kind, "value", adapter_kind)
    if adapter_kind == "authoring_core":
        return (
            REPO
            / "benchmarks"
            / "authoring_core"
            / "results"
            / feature_id(entry)
            / "latest.json"
        )
    if adapter_kind == "peripheral_suite":
        return REPO / "benchmarks" / "peripheral_suite" / "results" / "latest.json"
    return None


def _result_signature(path: Path | None) -> tuple[int, int, int] | None:
    if path is None:
        return None
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    return (stat.st_ino, stat.st_mtime_ns, stat.st_size)


def _load_runner_result(
    entry: object,
    *,
    previous_signature: tuple[int, int, int] | None = None,
) -> dict[str, object] | None:
    path = _runner_result_path(entry)
    if path is None or not path.is_file():
        return None
    if previous_signature is not None and _result_signature(path) == previous_signature:
        return None
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"runner result is not a JSON object: {path}")
    return value


def _safe_id(value: str) -> str:
    safe = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-.")
    if not safe:
        raise ValueError(f"feature id is not path-safe: {value!r}")
    return safe


def _render_entry_markdown(result: Mapping[str, object]) -> str:
    settle = result.get("settle")
    settle_status = (
        settle.get("status", "not_run")
        if isinstance(settle, Mapping)
        else "not_run"
    )
    lines = [
        f"# Feature regression: {result['feature']}",
        "",
        f"- Status: `{result['status']}`",
        f"- Adapter: `{result.get('adapter', 'runner')}`",
        f"- Gate policy: `{result.get('gate_policy', 'hard_1_10')}`",
        f"- Load settle: `{settle_status}`",
    ]
    waiver = result.get("waiver")
    if isinstance(waiver, Mapping):
        lines.extend(
            [
                f"- Waiver diagnostic: `{result.get('diagnostic_status', '-')}`",
                f"- Measured ratio: `{waiver.get('measured_ratio', '-')}`",
                f"- Waiver ceiling: `{waiver.get('max_ratio', '-')}`",
            ]
        )
    lines.extend(["", ""])
    return "\n".join(lines)


def _persist_entry(result_dir: Path, result: Mapping[str, object]) -> None:
    directory = result_dir / "entries" / _safe_id(str(result["feature"]))
    atomic_write_json(directory / "latest.json", result)
    atomic_write_text(directory / "latest.md", _render_entry_markdown(result))


def run_semantic_check(
    entry: object,
    *,
    command_runner: Callable[[Sequence[str]], object] = run_command,
) -> dict[str, object]:
    builds, build_failed = _run_builds([entry], command_runner, semantic=True)
    explicit = _command_list(entry, "semantic", required=False)
    commands = (
        [("semantic", explicit)] if explicit is not None else _benchmark_commands(entry)
    )
    adapter_kind = _entry_value(entry, "adapter_kind")
    adapter_kind = getattr(adapter_kind, "value", adapter_kind)
    if adapter_kind in {"authoring_core", "peripheral_suite"}:
        commands = [
            (label, [*command, "--semantic-only"])
            for label, command in commands
        ]
    if build_failed:
        return {
            "feature": feature_id(entry),
            "status": "failed",
            "builds": builds,
            "commands": {},
            "comparison": None,
        }
    result_path = _runner_result_path(entry)
    previous_signature = _result_signature(result_path)
    command_results = {
        label: _invoke(command, command_runner) for label, command in commands
    }
    status = (
        "failed"
        if any(int(result["returncode"]) != 0 for result in command_results.values())
        else "passed"
    )
    comparison = None
    if status == "passed" and _adapter(entry) == "equivalence_only":
        comparison = compare_multiclock(
            str(command_results["cpp_dpi"]["stdout"]),
            str(command_results["pure_sv"]["stdout"]),
        )
        status = str(comparison["status"])
    diagnostic_status = None
    if _gate_policy(entry) == "diagnostic":
        runner_result = _load_runner_result(
            entry, previous_signature=previous_signature
        )
        if runner_result is not None:
            diagnostic_status = _payload_status(
                runner_result,
                int(next(iter(command_results.values()))["returncode"]),
            )
            status = (
                "failed"
                if runner_result.get("status")
                in {"command_error", "workload_error", "error"}
                else "passed"
            )
        elif result_path is not None:
            diagnostic_status = "failed"
            status = "failed"
    return {
        "feature": feature_id(entry),
        "status": status,
        "builds": builds,
        "commands": command_results,
        "comparison": comparison,
        "diagnostic_status": diagnostic_status,
    }


def _measure_entry(
    entry: object,
    *,
    command_runner: Callable[[Sequence[str]], object],
) -> dict[str, object]:
    outputs: dict[str, object] = {}
    result_path = _runner_result_path(entry)
    previous_signature = _result_signature(result_path)
    commands = _benchmark_commands(entry)
    for label, command in commands:
        outputs[label] = _invoke(command, command_runner)
    if _adapter(entry) in {"multiclock", "equivalence_only"}:
        if any(int(value["returncode"]) != 0 for value in outputs.values()):  # type: ignore[index]
            status = "failed"
            comparison = None
        else:
            try:
                comparison = compare_multiclock(
                    str(outputs["cpp_dpi"]["stdout"]),  # type: ignore[index]
                    str(outputs["pure_sv"]["stdout"]),  # type: ignore[index]
                )
                status = str(comparison["status"])
            except ValueError as error:
                comparison = {
                    "status": "failed",
                    "error": str(error),
                    "measurement_mode": "equivalence_only",
                }
                status = "failed"
    else:
        comparison = None
        runner_result = _load_runner_result(
            entry, previous_signature=previous_signature
        )
        if runner_result is not None:
            outputs["runner_result"] = runner_result
            status = _payload_status(
                runner_result, int(outputs["runner"]["returncode"])  # type: ignore[index]
            )
        elif result_path is None:
            status = _status_from_runner(outputs["runner"])  # type: ignore[arg-type]
        else:
            status = "failed"
    policy = _gate_policy(entry)
    diagnostic_status = None
    waiver_evidence = None
    if policy == "diagnostic":
        diagnostic_status = status
        runner_result = outputs.get("runner_result")
        missing_expected_result = result_path is not None and runner_result is None
        status = (
            "failed"
            if missing_expected_result
            or (
                isinstance(runner_result, Mapping)
                and runner_result.get("status")
                in {"command_error", "workload_error", "error"}
            )
            else "passed"
        )
    elif policy == "waived_hard_1_10":
        diagnostic_status = status
        waiver = _waiver_metadata(entry)
        runner_result = outputs.get("runner_result")
        missing_expected_result = result_path is not None and runner_result is None
        result_error = (
            isinstance(runner_result, Mapping)
            and runner_result.get("status")
            in {"command_error", "workload_error", "error"}
        )
        ratio = (
            _authoring_guard_ratio(runner_result, feature_id(entry))
            if isinstance(runner_result, Mapping)
            else None
        )
        waiver_evidence = dict(waiver or {})
        waiver_evidence["measured_ratio"] = ratio
        waiver_evidence["original_status"] = diagnostic_status
        if missing_expected_result or result_error or waiver is None or ratio is None:
            status = "failed"
        elif diagnostic_status == "invalid_environment":
            status = "invalid_environment"
        else:
            status = "passed" if ratio <= float(waiver["max_ratio"]) else "failed"
        waiver_evidence["status"] = status
    return {
        "feature": feature_id(entry),
        "status": status,
        "adapter": _adapter(entry),
        "runner_outputs": outputs,
        "comparison": comparison,
        "diagnostic_status": diagnostic_status,
        "gate_policy": policy,
        "waiver": waiver_evidence,
    }


def _run_builds(
    entries: Sequence[object],
    command_runner: Callable[[Sequence[str]], object],
    *,
    semantic: bool = False,
) -> tuple[list[dict[str, object]], set[str]]:
    owners_by_command: dict[tuple[str, ...], list[str]] = {}
    for entry in entries:
        for command in _build_commands(entry, semantic=semantic):
            owners_by_command.setdefault(tuple(command), []).append(feature_id(entry))
    evidence: list[dict[str, object]] = []
    failed: set[str] = set()
    for command, owners in owners_by_command.items():
        try:
            result = _invoke(command, command_runner)
        except Exception as error:
            result = {
                "command": list(command),
                "returncode": 1,
                "stdout": "",
                "stderr": f"{type(error).__name__}: {error}",
            }
        result["features"] = owners
        evidence.append(result)
        if int(result["returncode"]) != 0:
            failed.update(owners)
    return evidence, failed


def run_benchmarks(
    entries: Sequence[object],
    *,
    command_runner: Callable[[Sequence[str]], object] = run_command,
    probe_runner: Callable[[], Mapping[str, object]] = normalized_load_probe,
    sleep_runner: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
    threshold: float = DEFAULT_LOAD_THRESHOLD,
    poll_seconds: float = DEFAULT_SETTLE_POLL_SECONDS,
    timeout_seconds: float = DEFAULT_SETTLE_TIMEOUT_SECONDS,
    result_dir: Path = RESULT_DIR,
) -> dict[str, object]:
    """Build every selection first, then measure entries one at a time."""
    selected = [entry for entry in entries if _has_benchmark(entry)]
    builds, build_failed = _run_builds(selected, command_runner)
    results: list[dict[str, object]] = []
    for entry in selected:
        entry_id = feature_id(entry)
        if entry_id in build_failed:
            result = {
                "feature": entry_id,
                "status": "failed",
                "adapter": _adapter(entry),
                "settle": {"status": "not_run", "reason": "build_failed"},
                "runner_outputs": {},
                "error": "one or more required build commands failed",
            }
        else:
            try:
                settle = settle_normalized_load(
                    probe_runner,
                    threshold=threshold,
                    poll_seconds=poll_seconds,
                    timeout_seconds=timeout_seconds,
                    sleep_runner=sleep_runner,
                    monotonic=monotonic,
                )
            except Exception as error:
                settle = {
                    "status": "probe_failed",
                    "threshold": threshold,
                    "poll_seconds": poll_seconds,
                    "timeout_seconds": timeout_seconds,
                    "error": f"{type(error).__name__}: {error}",
                    "probes": [],
                }
            if settle["status"] in {"timed_out", "unavailable"}:
                result = {
                    "feature": entry_id,
                    "status": "invalid_environment",
                    "adapter": _adapter(entry),
                    "settle": settle,
                    "runner_outputs": {},
                    "error": (
                        "normalized load did not settle before the timeout"
                        if settle["status"] == "timed_out"
                        else "normalized load probe is unavailable"
                    ),
                }
            elif settle["status"] == "probe_failed":
                result = {
                    "feature": entry_id,
                    "status": "invalid_environment",
                    "adapter": _adapter(entry),
                    "settle": settle,
                    "runner_outputs": {},
                    "error": "normalized load probe failed",
                }
            else:
                try:
                    result = _measure_entry(entry, command_runner=command_runner)
                    result["settle"] = settle
                except Exception as error:
                    result = {
                        "feature": entry_id,
                        "status": "failed",
                        "adapter": _adapter(entry),
                        "settle": settle,
                        "runner_outputs": {},
                        "error": f"{type(error).__name__}: {error}",
                    }
        _persist_entry(result_dir, result)
        results.append(result)
    return {
        "schema_version": 1,
        "generated_at_utc": _utc_now(),
        "status": aggregate_status(str(result["status"]) for result in results),
        "serial": True,
        "builds_completed_before_measurement": True,
        "config": {
            "normalized_load_threshold": threshold,
            "settle_poll_seconds": poll_seconds,
            "settle_timeout_seconds": timeout_seconds,
            "samples_normalized": False,
        },
        "builds": builds,
        "entries": results,
    }


def _render_index(result: Mapping[str, object]) -> str:
    lines = [
        "# Feature regression",
        "",
        f"- Status: `{result['status']}`",
        "- Execution: `serial`",
        "- Samples normalized: `false`",
        "",
        "| Feature | Adapter | Policy | Settle | Status | Diagnostic | Waiver |",
        "|---|---|---|---|---|---|---|",
    ]
    for entry in result["entries"]:  # type: ignore[index]
        lines.append(
            f"| `{entry['feature']}` | `{entry.get('adapter', 'runner')}` | "
            f"`{entry.get('gate_policy', 'hard_1_10')}` | "
            f"`{entry.get('settle', {}).get('status', 'not_run')}` | `{entry['status']}` | "
            f"`{entry.get('diagnostic_status') or '-'}` | "
            f"`{entry.get('waiver', {}).get('status', '-') if isinstance(entry.get('waiver'), Mapping) else '-'}` |"
        )
    lines.append("")
    return "\n".join(lines)


def persist_index(result: Mapping[str, object], result_dir: Path = RESULT_DIR) -> None:
    atomic_write_json(result_dir / "latest.json", result)
    atomic_write_text(result_dir / "latest.md", _render_index(result))


def _parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "command",
        choices=("list", "semantic-check", "benchmark", "regression", "registry-check"),
    )
    parser.add_argument("feature", nargs="?")
    parser.add_argument("--load-threshold", type=float, default=DEFAULT_LOAD_THRESHOLD)
    parser.add_argument("--settle-poll", type=float, default=DEFAULT_SETTLE_POLL_SECONDS)
    parser.add_argument("--settle-timeout", type=float, default=DEFAULT_SETTLE_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    needs_feature = args.command in {"semantic-check", "benchmark"}
    if needs_feature != bool(args.feature):
        parser.error(f"{args.command} {'requires' if needs_feature else 'does not accept'} FEATURE")
    return args


def main(argv: Sequence[str] | None = None, *, registry: object | None = None) -> int:
    args = _parse_args(argv)
    try:
        entries = validate_registry(registry)
        if args.command == "registry-check":
            print(f"registry valid: {len(entries)} features")
            return 0
        if args.command == "list":
            for entry in entries:
                benchmark = "benchmark" if _has_benchmark(entry) else "semantic-only"
                print(f"{feature_id(entry)}\t{benchmark}")
            return 0
        if args.command == "semantic-check":
            result = run_semantic_check(select_feature(entries, args.feature))
            print(json.dumps(result, indent=2))
            return 0 if result["status"] == "passed" else 1
        selected = (
            entries
            if args.command == "regression"
            else [select_feature(entries, args.feature)]
        )
        result = run_benchmarks(
            selected,
            threshold=args.load_threshold,
            poll_seconds=args.settle_poll,
            timeout_seconds=args.settle_timeout,
        )
        persist_index(result)
        print(_render_index(result))
        return 0 if result["status"] in {"passed", "passed_inconclusive"} else 1
    except (KeyError, RuntimeError, TypeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
