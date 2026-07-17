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
    "queue",
    "queue_sync",
    "all",
    "wide64",
    "wide_echo_137",
    "wide_slice",
    "fixed_mac",
    "array_index",
    "array_wide",
    "mem_rw",
    "hier_probe",
    "mem_backdoor",
    "mem_probe_read",
    "mem_probe_deposit",
    "mem_probe_read_deposit",
    "signal_edge",
    "array_multidim",
    "force_release",
    "packed_view",
    "force_direct",
    "hier_data",
    "timing_phases",
    "test_lifecycle",
    "dynamic_spawn",
    "dynamic_task",
    "dynamic_spawn_scheduler",
    "dynamic_spawn_suspending",
    "dynamic_monitor",
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
    "queue_send",
    "queue_receive",
    "queue_put",
    "queue_get",
    "lock_acquire",
    "semaphore_acquire",
    "wide64",
    "wide_echo_137",
    "wide_slice",
    "fixed_mac",
    "array_index",
    "array_wide",
    "array_multidim",
    "mem_rw",
    "hier_probe_reads",
    "hier_probe_deposits",
    "mem_backdoor_reads",
    "mem_backdoor_deposits",
    "probe_diag_reads",
    "probe_diag_deposits",
    "signal_edges",
    "force_release",
    "packed_view",
    "hier_data_reads",
    "hier_data_deposits",
    "timing_phases",
    "test_lifecycle",
    "dynamic_spawn",
)

RESULT_FIELDS = (
    "mode",
    "kernel",
    "iterations",
    "transactions",
    "checks",
    "sim_cycles",
    "spawned_processes",
    "checksum",
    "failures",
    *FEATURE_FIELDS,
)


@dataclass(frozen=True)
class ExpectedCounts:
    iterations: int
    transactions: int
    checks: int
    spawned_processes: int = 0
    task_value: int = 0
    clock_cycles: int = 0
    timeouts: int = 0
    timeout_hits: int = 0
    task_timeouts: int = 0
    task_timeout_hits: int = 0
    wait_until: int = 0
    event_set: int = 0
    event_wait: int = 0
    queue_send: int = 0
    queue_receive: int = 0
    queue_put: int = 0
    queue_get: int = 0
    lock_acquire: int = 0
    semaphore_acquire: int = 0
    wide64: int = 0
    wide_echo_137: int = 0
    wide_slice: int = 0
    fixed_mac: int = 0
    array_index: int = 0
    array_wide: int = 0
    array_multidim: int = 0
    mem_rw: int = 0
    hier_probe_reads: int = 0
    hier_probe_deposits: int = 0
    mem_backdoor_reads: int = 0
    mem_backdoor_deposits: int = 0
    probe_diag_reads: int = 0
    probe_diag_deposits: int = 0
    signal_edges: int = 0
    force_release: int = 0
    packed_view: int = 0
    hier_data_reads: int = 0
    hier_data_deposits: int = 0
    timing_phases: int = 0
    test_lifecycle: int = 0
    dynamic_spawn: int = 0

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

    queue_send = iterations if enabled or kernel == "queue" else 0
    queue_receive = queue_send
    if queue_send:
        feature_checks += iterations

    queue_put = iterations if kernel == "queue_sync" else 0
    queue_get = queue_put
    lock_acquire = queue_put
    semaphore_acquire = queue_put
    if queue_put:
        feature_checks += iterations

    wide64 = iterations if enabled or kernel == "wide64" else 0
    if wide64:
        feature_checks += iterations

    wide_echo_137 = iterations if enabled or kernel == "wide_echo_137" else 0
    if wide_echo_137:
        feature_checks += iterations

    wide_slice = iterations if enabled or kernel == "wide_slice" else 0
    if wide_slice:
        feature_checks += iterations

    fixed_mac = iterations if enabled or kernel == "fixed_mac" else 0
    if fixed_mac:
        feature_checks += iterations

    array_index = iterations if enabled or kernel == "array_index" else 0
    if array_index:
        feature_checks += iterations * 8

    array_wide = iterations if enabled or kernel == "array_wide" else 0
    if array_wide:
        feature_checks += iterations * 4

    array_multidim = iterations if kernel == "array_multidim" else 0
    if array_multidim:
        feature_checks += iterations * 6

    mem_rw = iterations if enabled or kernel == "mem_rw" else 0
    if mem_rw:
        feature_checks += iterations

    hier_probe_reads = 2 * iterations if enabled or kernel == "hier_probe" else 0
    hier_probe_deposits = (
        iterations if enabled or kernel == "hier_probe" else 0
    )
    if hier_probe_reads:
        feature_checks += 2 * iterations

    mem_backdoor_reads = (
        iterations if enabled or kernel == "mem_backdoor" else 0
    )
    mem_backdoor_deposits = mem_backdoor_reads
    if mem_backdoor_reads:
        feature_checks += 2 * iterations

    probe_diag_reads = (
        iterations
        if kernel in ("mem_probe_read", "mem_probe_read_deposit")
        else 0
    )
    probe_diag_deposits = (
        iterations
        if kernel in ("mem_probe_deposit", "mem_probe_read_deposit")
        else 0
    )
    if kernel == "mem_probe_read":
        feature_checks += 2 * iterations
    elif kernel == "mem_probe_deposit":
        feature_checks += iterations
    elif kernel == "mem_probe_read_deposit":
        feature_checks += 2 * iterations

    signal_edges = iterations if kernel == "signal_edge" else 0

    force_release = (
        iterations if kernel in ("force_release", "force_direct") else 0
    )
    if force_release:
        feature_checks += (
            iterations if kernel == "force_direct" else 2 * iterations
        )

    packed_view = iterations if kernel == "packed_view" else 0
    if packed_view:
        feature_checks += 4 * iterations

    hier_data_reads = 2 * iterations if kernel == "hier_data" else 0
    hier_data_deposits = 2 * iterations if kernel == "hier_data" else 0
    if hier_data_reads:
        feature_checks += 2 * iterations

    if kernel == "force_direct":
        return ExpectedCounts(
            iterations=iterations,
            transactions=0,
            checks=iterations,
            force_release=force_release,
        )

    if kernel == "timing_phases":
        return ExpectedCounts(
            iterations=iterations,
            transactions=0,
            checks=2 * iterations,
            timing_phases=iterations,
        )

    if kernel == "test_lifecycle":
        return ExpectedCounts(
            iterations=iterations,
            transactions=0,
            checks=3 * iterations,
            spawned_processes=1,
            test_lifecycle=iterations,
        )

    if kernel in (
        "dynamic_spawn",
        "dynamic_task",
        "dynamic_spawn_scheduler",
        "dynamic_spawn_suspending",
    ):
        spawned_processes = iterations
        if kernel == "dynamic_task":
            spawned_processes = 0
        elif kernel == "dynamic_spawn_suspending":
            spawned_processes = 2 * iterations
        return ExpectedCounts(
            iterations=iterations,
            transactions=0,
            checks=iterations,
            spawned_processes=spawned_processes,
            dynamic_spawn=iterations,
        )

    if kernel == "dynamic_monitor":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 3,
            spawned_processes=2,
            queue_put=iterations,
            queue_get=iterations,
        )

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
        queue_send=queue_send,
        queue_receive=queue_receive,
        queue_put=queue_put,
        queue_get=queue_get,
        lock_acquire=lock_acquire,
        semaphore_acquire=semaphore_acquire,
        wide64=wide64,
        wide_echo_137=wide_echo_137,
        wide_slice=wide_slice,
        fixed_mac=fixed_mac,
        array_index=array_index,
        array_wide=array_wide,
        array_multidim=array_multidim,
        mem_rw=mem_rw,
        hier_probe_reads=hier_probe_reads,
        hier_probe_deposits=hier_probe_deposits,
        mem_backdoor_reads=mem_backdoor_reads,
        mem_backdoor_deposits=mem_backdoor_deposits,
        probe_diag_reads=probe_diag_reads,
        probe_diag_deposits=probe_diag_deposits,
        signal_edges=signal_edges,
        force_release=force_release,
        packed_view=packed_view,
        hier_data_reads=hier_data_reads,
        hier_data_deposits=hier_data_deposits,
        timing_phases=0,
    )


def stimulus(iteration: int) -> int:
    return ((((iteration + 1) * 0x1F123BB5) & 0xFFFFFFFF) ^ 0xC001D00D) & 0xFFFFFFFF


def response(iteration: int) -> int:
    return ((stimulus(iteration) ^ 0xA5A55A5A) + iteration) & 0xFFFFFFFF


def expected_checksum(iterations: int, *, kernel: str | None = None) -> int:
    if iterations <= 0:
        raise ValueError("iterations must be greater than zero")
    if kernel in (
        "force_direct",
        "timing_phases",
        "test_lifecycle",
        "dynamic_spawn",
        "dynamic_task",
        "dynamic_spawn_scheduler",
        "dynamic_spawn_suspending",
    ):
        return 0x811C9DC5
    checksum = 0x811C9DC5
    for iteration in range(iterations):
        checksum = ((checksum ^ response(iteration)) * 0x01000193) & 0xFFFFFFFF
    return checksum
