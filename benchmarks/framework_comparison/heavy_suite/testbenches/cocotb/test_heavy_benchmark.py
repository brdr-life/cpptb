import inspect
import os
import time

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge, Timer
from cocotb.utils import get_sim_time


MASK32 = (1 << 32) - 1
WORKLOADS = {"streaming_fir", "packet_crc32", "matrix4x4"}


def stimulus(ordinal):
    return ((((ordinal + 1) * 0x1F123BB5) & MASK32) ^ 0xC001D00D) & MASK32


def signed16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def fir_coefficient(tap):
    return ((tap * 7) % 19) - 9


def packet_byte(packet, offset):
    return stimulus(packet * 96 + offset) & 0xFF


def crc32_byte(current, data):
    value = current ^ data
    for _ in range(8):
        value = ((value >> 1) ^ 0xEDB88320) if value & 1 else value >> 1
    return value & MASK32


def matrix_value(block, matrix, index):
    return ((stimulus(block * 32 + matrix * 16 + index) >> 8) & 0x7FF) - 1024


class BenchState:
    def __init__(self, workload, iterations):
        self.workload = workload
        self.iterations = iterations
        self.transactions = 0
        self.checks = 0
        self.checksum = 0x811C9DC5
        self.failures = 0

    def check(self, label, actual, expected):
        self.checks += 1
        actual &= MASK32
        expected &= MASK32
        if actual == expected:
            return
        self.failures += 1
        if self.failures <= 8:
            print(
                "HEAVY_BENCH_MISMATCH "
                f"mode=cocotb workload={self.workload} label={label} "
                f"actual=0x{actual:08x} expected=0x{expected:08x}"
            )

    def fold(self, value):
        self.checksum = ((self.checksum ^ (value & MASK32)) * 0x01000193) & MASK32


def start_clock(signal):
    maybe_coro = Clock(signal, 2, unit="ns").start(start_high=False)
    if inspect.isawaitable(maybe_coro):
        cocotb.start_soon(maybe_coro)


async def reset_dut(dut):
    dut.clk.value = 0
    dut.rst_n.value = 0
    dut.fir_in_valid.value = 0
    dut.fir_in_sample.value = 0
    dut.fir_out_ready.value = 1
    dut.crc_in_valid.value = 0
    dut.crc_in_data.value = 0
    dut.crc_in_last.value = 0
    dut.crc_out_ready.value = 1
    dut.mat_load_valid.value = 0
    dut.mat_load_select.value = 0
    dut.mat_load_index.value = 0
    dut.mat_load_data.value = 0
    dut.mat_start.value = 0
    dut.mat_out_ready.value = 1
    start_clock(dut.clk)
    for _ in range(4):
        await RisingEdge(dut.clk)
    dut.rst_n.value = 1


async def run_fir(dut, state):
    history = [0] * 32
    for iteration in range(state.iterations):
        sample = signed16(stimulus(iteration))
        expected = sample * fir_coefficient(0)
        expected += sum(history[tap - 1] * fir_coefficient(tap) for tap in range(1, 32))
        history[1:] = history[:-1]
        history[0] = sample

        await FallingEdge(dut.clk)
        dut.fir_in_sample.value = sample & 0xFFFF
        dut.fir_in_valid.value = 1
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        result = int(dut.fir_out_result.value)
        state.check("FIR result", result, expected)
        state.fold(result)
        state.transactions += 1
    dut.fir_in_valid.value = 0
    state.check("FIR accepted sample count", int(dut.fir_sample_count.value), state.iterations)


async def run_crc(dut, state):
    for packet in range(state.iterations):
        expected = 0xFFFFFFFF
        length = 32 + (packet & 63)
        for offset in range(length):
            data = packet_byte(packet, offset)
            expected = crc32_byte(expected, data)
            await FallingEdge(dut.clk)
            dut.crc_in_data.value = data
            dut.crc_in_last.value = int(offset + 1 == length)
            dut.crc_in_valid.value = 1
            await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        result = int(dut.crc_out_result.value)
        state.check("packet CRC32", result, ~expected)
        state.fold(result)
        state.transactions += 1
    dut.crc_in_valid.value = 0
    dut.crc_in_last.value = 0
    state.check("CRC packet count", int(dut.crc_packet_count.value), state.iterations)


async def load_matrix_value(dut, select, index, value):
    while int(dut.mat_load_ready.value) == 0:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
    await FallingEdge(dut.clk)
    dut.mat_load_select.value = select
    dut.mat_load_index.value = index
    dut.mat_load_data.value = value & 0xFFFF
    dut.mat_load_valid.value = 1
    await RisingEdge(dut.clk)


async def run_matrix(dut, state):
    for block in range(state.iterations):
        matrix_a = [matrix_value(block, 0, index) for index in range(16)]
        matrix_b = [matrix_value(block, 1, index) for index in range(16)]
        for index, value in enumerate(matrix_a):
            await load_matrix_value(dut, 0, index, value)
        for index, value in enumerate(matrix_b):
            await load_matrix_value(dut, 1, index, value)

        await FallingEdge(dut.clk)
        dut.mat_load_valid.value = 0
        dut.mat_start.value = 1
        await RisingEdge(dut.clk)
        await FallingEdge(dut.clk)
        dut.mat_start.value = 0

        for output in range(16):
            await RisingEdge(dut.clk)
            await Timer(1, unit="ps")
            row, column = divmod(output, 4)
            expected = sum(
                matrix_a[row * 4 + element] * matrix_b[element * 4 + column]
                for element in range(4)
            )
            state.check("matrix output index", int(dut.mat_out_index.value), output)
            result = int(dut.mat_out_data.value)
            state.check("matrix output data", result, expected)
            state.fold(result)
        state.transactions += 1
    state.check("matrix block count", int(dut.mat_block_count.value), state.iterations)


@cocotb.test()
async def heavy_benchmark(dut):
    workload = os.environ.get("HEAVY_BENCH_WORKLOAD", "streaming_fir")
    iterations = int(os.environ.get("HEAVY_BENCH_ITERS", "1000"))
    if workload not in WORKLOADS:
        raise ValueError(f"unsupported heavy workload: {workload}")
    if iterations <= 0:
        raise ValueError("HEAVY_BENCH_ITERS must be positive")

    state = BenchState(workload, iterations)
    await reset_dut(dut)
    start = time.perf_counter()
    if workload == "streaming_fir":
        await run_fir(dut, state)
    elif workload == "packet_crc32":
        await run_crc(dut, state)
    else:
        await run_matrix(dut, state)

    wall_ms = (time.perf_counter() - start) * 1000.0
    sim_cycles = (int(get_sim_time("fs")) + 1_000_000) // 2_000_000
    print(
        "HEAVY_BENCH_RESULT "
        f"mode=cocotb workload={workload} iterations={iterations} "
        f"transactions={state.transactions} checks={state.checks} "
        f"sim_cycles={sim_cycles} checksum={state.checksum} "
        f"failures={state.failures} wall_ms={wall_ms:.3f}"
    )
    assert state.failures == 0
