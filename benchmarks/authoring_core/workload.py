"""Authoring-core workload contract shared by the runner and direct tests."""

from __future__ import annotations

from dataclasses import dataclass, asdict


KERNELS = (
    "control",
    "task_value",
    "clock_cycles",
    "timeout",
    "task_timeout",
    "wait_until",
    "event",
    "channel",
    "all",
)

FEATURE_FIELDS = (
    "task_value",
    "clock_cycles",
    "timeouts",
    "timeout_hits",
    "task_timeouts",
    "task_timeout_hits",
    "wait_until",
    "event_set",
    "event_wait",
    "channel_send",
    "channel_receive",
)

RESULT_FIELDS = (
    "mode",
    "kernel",
    "iterations",
    "transactions",
    "checks",
    "sim_cycles",
    "checksum",
    "failures",
    *FEATURE_FIELDS,
)


@dataclass(frozen=True)
class ExpectedCounts:
    iterations: int
    transactions: int
    checks: int
    task_value: int = 0
    clock_cycles: int = 0
    timeouts: int = 0
    timeout_hits: int = 0
    task_timeouts: int = 0
    task_timeout_hits: int = 0
    wait_until: int = 0
    event_set: int = 0
    event_wait: int = 0
    channel_send: int = 0
    channel_receive: int = 0

    def fields(self) -> dict[str, int]:
        return asdict(self)


def expected_counts(kernel: str, iterations: int) -> ExpectedCounts:
    if kernel not in KERNELS:
        raise ValueError(f"unknown authoring-core kernel: {kernel}")
    if iterations <= 0:
        raise ValueError("iterations must be greater than zero")

    enabled = kernel == "all"
    feature_checks = 0

    task_value = iterations if enabled or kernel == "task_value" else 0
    if task_value:
        feature_checks += iterations

    clock_cycles = iterations if enabled or kernel == "clock_cycles" else 0

    timeouts = iterations if enabled or kernel == "timeout" else 0
    timeout_hits = iterations // 2 if timeouts else 0
    if timeouts:
        feature_checks += iterations

    task_timeouts = iterations if enabled or kernel == "task_timeout" else 0
    task_timeout_hits = iterations // 2 if task_timeouts else 0
    if task_timeouts:
        feature_checks += iterations

    wait_until = iterations if enabled or kernel == "wait_until" else 0
    if wait_until:
        feature_checks += iterations

    event_set = iterations if enabled or kernel == "event" else 0
    event_wait = event_set
    if event_set:
        feature_checks += iterations

    channel_send = iterations if enabled or kernel == "channel" else 0
    channel_receive = channel_send
    if channel_send:
        feature_checks += iterations

    return ExpectedCounts(
        iterations=iterations,
        transactions=iterations,
        checks=iterations + feature_checks + 2,
        task_value=task_value,
        clock_cycles=clock_cycles,
        timeouts=timeouts,
        timeout_hits=timeout_hits,
        task_timeouts=task_timeouts,
        task_timeout_hits=task_timeout_hits,
        wait_until=wait_until,
        event_set=event_set,
        event_wait=event_wait,
        channel_send=channel_send,
        channel_receive=channel_receive,
    )


def stimulus(iteration: int) -> int:
    return ((((iteration + 1) * 0x1F123BB5) & 0xFFFFFFFF) ^ 0xC001D00D) & 0xFFFFFFFF


def response(iteration: int) -> int:
    return ((stimulus(iteration) ^ 0xA5A55A5A) + iteration) & 0xFFFFFFFF


def expected_checksum(iterations: int) -> int:
    if iterations <= 0:
        raise ValueError("iterations must be greater than zero")
    checksum = 0x811C9DC5
    for iteration in range(iterations):
        checksum = ((checksum ^ response(iteration)) * 0x01000193) & 0xFFFFFFFF
    return checksum
