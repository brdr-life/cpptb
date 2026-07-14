import inspect
import os
import time

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge, Timer
from cocotb.utils import get_sim_time


MASK32 = (1 << 32) - 1
MASK137 = (1 << 137) - 1
WIDE137_XOR = int("1a55aa55aa50123456789abcdefdeadbeef", 16)
SUPPORTED_WORKLOADS = {"control", "wide_echo_137", "signal_edge"}


def stimulus(iteration):
    return ((((iteration + 1) * 0x1F123BB5) & MASK32) ^ 0xC001D00D) & MASK32


def expected_response(iteration):
    return ((stimulus(iteration) ^ 0xA5A55A5A) + iteration) & MASK32


def wide137_stimulus(iteration):
    value = 0
    for word in range(5):
        value |= stimulus(iteration * 5 + word) << (word * 32)
    return value & MASK137


def start_clock(signal):
    clock = Clock(signal, 2, unit="ns")
    maybe_coro = clock.start(start_high=False)
    if inspect.isawaitable(maybe_coro):
        cocotb.start_soon(maybe_coro)
    return clock


class BenchState:
    def __init__(self, workload, iterations):
        self.workload = workload
        self.iterations = iterations
        self.transactions = 0
        self.checks = 0
        self.checksum = 0x811C9DC5
        self.failures = 0
        self.wide_echo_137 = 0
        self.signal_edges = 0

    def check(self, label, actual, expected):
        self.checks += 1
        if actual == expected:
            return
        self.failures += 1
        if self.failures <= 8:
            print(
                "FRAMEWORK_COMPARISON_MISMATCH "
                f"mode=cocotb workload={self.workload} label={label} "
                f"actual=0x{actual:x} expected=0x{expected:x}"
            )


async def wait_ready(dut):
    while int(dut.req_ready.value) == 0:
        await RisingEdge(dut.clk)


async def transact(dut, state, iteration, payload):
    await wait_ready(dut)
    await FallingEdge(dut.clk)
    dut.req_data.value = payload
    dut.req_valid.value = 1

    await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    dut.req_valid.value = 0

    while True:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        if int(dut.rsp_valid.value) != 0:
            break

    response = int(dut.rsp_data.value)
    state.check("response", response, expected_response(iteration))
    state.checksum = ((state.checksum ^ response) * 0x01000193) & MASK32
    state.transactions += 1


async def transact_signal_edge(dut, state, iteration, payload):
    await wait_ready(dut)
    await FallingEdge(dut.clk)
    dut.req_data.value = payload
    dut.req_valid.value = 1

    await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    dut.req_valid.value = 0

    await RisingEdge(dut.rsp_valid)
    state.signal_edges += 1
    response = int(dut.rsp_data.value)
    state.check("response", response, expected_response(iteration))
    state.checksum = ((state.checksum ^ response) * 0x01000193) & MASK32
    state.transactions += 1


async def check_wide137(dut, state, iteration):
    value = wide137_stimulus(iteration)
    state.wide_echo_137 += 1
    await FallingEdge(dut.clk)
    dut.wide137_i.value = value
    await RisingEdge(dut.clk)
    await Timer(1, unit="ps")
    state.check("wide137", int(dut.wide137_o.value), value ^ WIDE137_XOR)


@cocotb.test()
async def authoring_core_comparison(dut):
    workload = os.environ.get("FRAMEWORK_COMPARISON_WORKLOAD", "control")
    iterations = int(os.environ.get("FRAMEWORK_COMPARISON_ITERS", "1000"))
    if workload not in SUPPORTED_WORKLOADS:
        raise ValueError(f"unsupported comparison workload: {workload}")
    if iterations <= 0:
        raise ValueError("FRAMEWORK_COMPARISON_ITERS must be positive")

    state = BenchState(workload, iterations)
    dut.clk.value = 0
    dut.rst_n.value = 0
    dut.req_valid.value = 0
    dut.req_data.value = 0
    dut.rsp_ready.value = 1
    dut.wide137_i.value = 0
    start_clock(dut.clk)

    start = time.perf_counter()
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1

    for iteration in range(iterations):
        if workload == "wide_echo_137":
            await check_wide137(dut, state, iteration)

        payload = stimulus(iteration)
        if workload == "signal_edge":
            await transact_signal_edge(dut, state, iteration, payload)
        else:
            await transact(dut, state, iteration, payload)

    while int(dut.response_count.value) != iterations:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")

    state.check("request count", int(dut.request_count.value), iterations)
    state.check("response count", int(dut.response_count.value), iterations)

    wall_ms = (time.perf_counter() - start) * 1000.0
    sim_cycles = (int(get_sim_time("fs")) + 1_000_000) // 2_000_000
    print(
        "FRAMEWORK_COMPARISON_RESULT "
        f"mode=cocotb workload={workload} iterations={iterations} "
        f"transactions={state.transactions} checks={state.checks} "
        f"sim_cycles={sim_cycles} checksum={state.checksum} "
        f"failures={state.failures} wide_echo_137={state.wide_echo_137} "
        f"signal_edges={state.signal_edges} wall_ms={wall_ms:.3f}"
    )
    assert state.failures == 0
