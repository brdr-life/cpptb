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
    "analysis_fanout",
    "random_stimulus",
    "constrained_packet",
    "constraint_extensions",
    "coverage_sampling",
    "apb_component",
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
    "analysis_write",
    "analysis_delivery",
    "random_stimulus",
    "constrained_packet",
    "constraint_extensions",
    "coverage_sampling",
    "apb_component",
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
    analysis_write: int = 0
    analysis_delivery: int = 0
    random_stimulus: int = 0
    constrained_packet: int = 0
    constraint_extensions: int = 0
    coverage_sampling: int = 0
    apb_component: int = 0

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

    if kernel == "analysis_fanout":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 6,
            spawned_processes=1,
            queue_put=iterations,
            queue_get=iterations,
            analysis_write=2 * iterations,
            analysis_delivery=3 * iterations,
        )

    if kernel == "random_stimulus":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 2,
            random_stimulus=iterations,
        )

    if kernel == "constrained_packet":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 2,
            constrained_packet=iterations,
        )

    if kernel == "constraint_extensions":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 2,
            constraint_extensions=iterations,
        )

    if kernel == "coverage_sampling":
        return ExpectedCounts(
            iterations=iterations,
            transactions=iterations,
            checks=iterations + 7,
            coverage_sampling=iterations,
        )

    if kernel == "apb_component":
        return ExpectedCounts(
            iterations=iterations,
            transactions=2 * iterations,
            checks=5 * iterations + 4,
            apb_component=2 * iterations,
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


_MASK64 = (1 << 64) - 1


def _rotate_left64(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (64 - shift))) & _MASK64


def random_payloads(iterations: int):
    splitmix_state = 1
    state = []
    for _ in range(4):
        splitmix_state = (splitmix_state + 0x9E3779B97F4A7C15) & _MASK64
        value = splitmix_state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _MASK64
        state.append((value ^ (value >> 31)) & _MASK64)

    def next_u64() -> int:
        result = (_rotate_left64((state[1] * 5) & _MASK64, 7) * 9) & _MASK64
        shifted = (state[1] << 17) & _MASK64
        state[2] ^= state[0]
        state[3] ^= state[1]
        state[1] ^= state[2]
        state[0] ^= state[3]
        state[2] ^= shifted
        state[3] = _rotate_left64(state[3], 45)
        return result

    def below(bound: int) -> int:
        threshold = ((-bound) & _MASK64) % bound
        while True:
            value = next_u64()
            if value >= threshold:
                return value % bound

    weighted_masks = (
        0,
        0x01010101,
        0x01010101,
        0x13579BDF,
        0x13579BDF,
        0x13579BDF,
        0xA5A55A5A,
        0xA5A55A5A,
        0xA5A55A5A,
        0xA5A55A5A,
    )
    for _ in range(iterations):
        payload = next_u64() & 0xFFFFFFFF
        payload ^= weighted_masks[below(10)]
        first_wide = next_u64()
        second_wide = next_u64()
        payload ^= first_wide & 0xFFFFFFFF
        payload ^= (first_wide >> 32) & 0xFFFFFFFF
        if second_wide & 1:
            payload ^= 0x80000000
        order = [0, 1, 2, 3]
        for remaining in range(4, 1, -1):
            selected = below(remaining)
            order[remaining - 1], order[selected] = (
                order[selected], order[remaining - 1]
            )
        payload ^= order[0] | (order[1] << 4) | (order[2] << 8) | (order[3] << 12)
        yield payload & 0xFFFFFFFF


def constrained_packet_payloads(iterations: int):
    splitmix_state = 1
    state = []
    for _ in range(4):
        splitmix_state = (splitmix_state + 0x9E3779B97F4A7C15) & _MASK64
        value = splitmix_state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _MASK64
        state.append((value ^ (value >> 31)) & _MASK64)

    def next_u64() -> int:
        result = (_rotate_left64((state[1] * 5) & _MASK64, 7) * 9) & _MASK64
        shifted = (state[1] << 17) & _MASK64
        state[2] ^= state[0]
        state[3] ^= state[1]
        state[1] ^= state[2]
        state[0] ^= state[3]
        state[2] ^= shifted
        state[3] = _rotate_left64(state[3], 45)
        return result

    def below(bound: int) -> int:
        threshold = ((-bound) & _MASK64) % bound
        while True:
            value = next_u64()
            if value >= threshold:
                return value % bound

    for _ in range(iterations):
        while True:
            opcode = below(7)
            length = 64 + below(1437)
            address = 0x1000 + below(4096)
            tag = below(256)
            if length % 4 != 0:
                continue
            if address % 4 != 0:
                continue
            if opcode == 6 and length > 256:
                continue
            yield (
                (opcode << 29) ^ (length << 16) ^ (address << 1) ^ tag
            ) & 0xFFFFFFFF
            break


def constraint_extension_payloads(iterations: int):
    splitmix_state = 1
    state = []
    for _ in range(4):
        splitmix_state = (splitmix_state + 0x9E3779B97F4A7C15) & _MASK64
        value = splitmix_state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _MASK64
        state.append((value ^ (value >> 31)) & _MASK64)

    def next_u64() -> int:
        result = (_rotate_left64((state[1] * 5) & _MASK64, 7) * 9) & _MASK64
        shifted = (state[1] << 17) & _MASK64
        state[2] ^= state[0]
        state[3] ^= state[1]
        state[1] ^= state[2]
        state[0] ^= state[3]
        state[2] ^= shifted
        state[3] = _rotate_left64(state[3], 45)
        return result

    def below(bound: int) -> int:
        threshold = ((-bound) & _MASK64) % bound
        while True:
            value = next_u64()
            if value >= threshold:
                return value % bound

    opcodes = (1, 3, 5)
    for _ in range(iterations):
        while True:
            opcode = opcodes[below(3)]
            selected = below(4)
            if selected == 0:
                length = 64 + below(1)
            else:
                length = 128 + below(4)
            route = 2 + below(1)
            byte0 = below(256)
            byte1 = below(256)
            token0 = below(1 << 32)
            token1 = below(1 << 32)
            token2 = 1 + below(1)
            if byte0 == byte1:
                continue
            yield (
                (opcode << 29)
                ^ (length << 16)
                ^ (route << 24)
                ^ (byte0 << 8)
                ^ byte1
                ^ token0
                ^ token1
                ^ (token2 << 31)
            ) & 0xFFFFFFFF
            break


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
    if kernel == "random_stimulus":
        responses = (
            ((payload ^ 0xA5A55A5A) + iteration) & 0xFFFFFFFF
            for iteration, payload in enumerate(random_payloads(iterations))
        )
    elif kernel == "constrained_packet":
        responses = (
            ((payload ^ 0xA5A55A5A) + iteration) & 0xFFFFFFFF
            for iteration, payload in enumerate(
                constrained_packet_payloads(iterations)
            )
        )
    elif kernel == "constraint_extensions":
        responses = (
            ((payload ^ 0xA5A55A5A) + iteration) & 0xFFFFFFFF
            for iteration, payload in enumerate(
                constraint_extension_payloads(iterations)
            )
        )
    elif kernel == "apb_component":
        responses = (stimulus(iteration) for iteration in range(iterations))
    else:
        responses = (response(iteration) for iteration in range(iterations))
    for value in responses:
        checksum = ((checksum ^ value) * 0x01000193) & 0xFFFFFFFF
    return checksum
