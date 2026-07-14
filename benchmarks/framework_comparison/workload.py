"""Shared contract for the four-framework authoring comparison."""

from __future__ import annotations

from dataclasses import dataclass

from benchmarks.authoring_core.workload import expected_checksum, expected_counts


AUTHORING_WORKLOADS = ("control", "wide_echo_137", "signal_edge")
MODES = ("pure_sv", "cpp_dpi", "cpp_vpi", "cocotb")


@dataclass(frozen=True)
class WorkloadDescription:
    title: str
    purpose: str


DESCRIPTIONS = {
    "control": WorkloadDescription(
        "Clocked request/response",
        "32-bit drive, clock-edge scheduling, 1 ps observation, and response check",
    ),
    "wide_echo_137": WorkloadDescription(
        "137-bit packed traffic",
        "arbitrary-width drive/read plus the same clocked request/response path",
    ),
    "signal_edge": WorkloadDescription(
        "DUT-generated edge wait",
        "resume directly from a response-valid edge rather than polling",
    ),
    "peripheral_suite": WorkloadDescription(
        "Concurrent APB peripherals",
        "parallel timer, SPI, and I2C drivers with concurrent monitors",
    ),
}


def expected_authoring_result(workload: str, iterations: int) -> dict[str, int]:
    if workload not in AUTHORING_WORKLOADS:
        raise ValueError(f"unsupported authoring comparison workload: {workload}")
    counts = expected_counts(workload, iterations)
    return {
        "iterations": iterations,
        "transactions": counts.transactions,
        "checks": counts.checks,
        "checksum": expected_checksum(iterations, kernel=workload),
        "failures": 0,
        "wide_echo_137": counts.wide_echo_137,
        "signal_edges": counts.signal_edges,
    }


def rotated_mode_order(round_index: int) -> tuple[str, ...]:
    offset = round_index % len(MODES)
    return MODES[offset:] + MODES[:offset]
