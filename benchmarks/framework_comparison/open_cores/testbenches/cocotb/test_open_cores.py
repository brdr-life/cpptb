import inspect
import os
import time

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import FallingEdge, RisingEdge, Timer
from cocotb.utils import get_sim_time


MASK32 = (1 << 32) - 1
FIRMWARE = (
    0x10002083, 0x12345137, 0x67810113, 0x00D11193,
    0x00314133, 0x01115193, 0x00314133, 0x00511193,
    0x00314133, 0xFFF08093, 0xFE0092E3, 0x10000237,
    0x00222023, 0x0000006F,
)
AES_KEY = (
    0x2B7E1516, 0x28AED2A6, 0xABF71588, 0x09CF4F3C,
    0, 0, 0, 0,
)
AES_PLAINTEXT = (
    (0x6BC1BEE2, 0x2E409F96, 0xE93D7E11, 0x7393172A),
    (0xAE2D8A57, 0x1E03AC9C, 0x9EB76FAC, 0x45AF8E51),
    (0x30C81C46, 0xA35CE411, 0xE5FBC119, 0x1A0A52EF),
    (0xF69F2445, 0xDF4F9B17, 0xAD2B417B, 0xE66C3710),
)
AES_CIPHERTEXT = (
    (0x3AD77BB4, 0x0D7A3660, 0xA89ECAF3, 0x2466EF97),
    (0xF5D3D585, 0x03B9699D, 0xE785895A, 0x96FDBAAF),
    (0x43B1CD7F, 0x598ECE23, 0x881B00E3, 0xED030688),
    (0x7B0C785E, 0x27E8AD3F, 0x82232071, 0x04725DD4),
)


def stimulus(ordinal):
    return ((((ordinal + 1) * 0x1F123BB5) & MASK32) ^ 0xC001D00D) & MASK32


def frame_byte(packet, offset):
    return stimulus(packet * 2048 + offset) & 0xFF


def frame_length(packet):
    return 64 + ((packet * 37) % 1455)


def crc32_byte(current, data):
    value = current ^ data
    for _ in range(8):
        value = ((value >> 1) ^ 0xEDB88320) if value & 1 else value >> 1
    return value & MASK32


def xorshift32(value):
    value ^= (value << 13) & MASK32
    value ^= value >> 17
    value ^= (value << 5) & MASK32
    return value & MASK32


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
                "OPEN_CORE_BENCH_MISMATCH "
                f"mode=cocotb workload={self.workload} label={label} "
                f"actual=0x{actual:08x} expected=0x{expected:08x}"
            )

    def fold(self, value):
        self.checksum = ((self.checksum ^ (value & MASK32)) * 0x01000193) & MASK32


def start_clock(signal):
    maybe_coro = Clock(signal, 2, unit="ns").start(start_high=False)
    if inspect.isawaitable(maybe_coro):
        cocotb.start_soon(maybe_coro)


def initialize_inputs(dut):
    dut.clk.value = 0
    dut.rst_n.value = 1
    dut.cpu_prog_we.value = 0
    dut.cpu_prog_addr.value = 0
    dut.cpu_prog_data.value = 0
    dut.aes_cs.value = 0
    dut.aes_we.value = 0
    dut.aes_address.value = 0
    dut.aes_write_data.value = 0
    dut.fcs_tdata.value = 0
    dut.fcs_tkeep.value = 0
    dut.fcs_tvalid.value = 0
    dut.fcs_tlast.value = 0


async def reset_dut(dut):
    await FallingEdge(dut.clk)
    dut.rst_n.value = 0
    for _ in range(4):
        await RisingEdge(dut.clk)
    await FallingEdge(dut.clk)
    dut.rst_n.value = 1


async def program_word(dut, address, data):
    await FallingEdge(dut.clk)
    dut.cpu_prog_addr.value = address
    dut.cpu_prog_data.value = data
    dut.cpu_prog_we.value = 1
    await RisingEdge(dut.clk)


async def run_picorv32(dut, state):
    await FallingEdge(dut.clk)
    dut.rst_n.value = 0
    for index, instruction in enumerate(FIRMWARE):
        await program_word(dut, index * 4, instruction)
    await program_word(dut, 0x100, state.iterations)
    await FallingEdge(dut.clk)
    dut.cpu_prog_we.value = 0
    dut.rst_n.value = 1
    await RisingEdge(dut.cpu_done)
    await Timer(1, unit="ps")

    expected = 0x12345678
    for _ in range(state.iterations):
        expected = xorshift32(expected)
    state.check("firmware result", int(dut.cpu_result.value), expected)
    state.check("CPU trap", int(dut.cpu_trap.value), 0)
    state.fold(int(dut.cpu_result.value))
    state.transactions = state.iterations


async def aes_write(dut, address, data):
    await FallingEdge(dut.clk)
    dut.aes_cs.value = 1
    dut.aes_we.value = 1
    dut.aes_address.value = address
    dut.aes_write_data.value = data
    await RisingEdge(dut.clk)


async def aes_select_read(dut, address):
    await FallingEdge(dut.clk)
    dut.aes_cs.value = 1
    dut.aes_we.value = 0
    dut.aes_address.value = address


async def aes_wait_status(dut, mask):
    saw_clear = False
    await aes_select_read(dut, 0x09)
    while True:
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")
        if not int(dut.aes_read_data.value) & mask:
            saw_clear = True
        elif saw_clear:
            return


async def aes_read(dut, address):
    await aes_select_read(dut, address)
    await Timer(1, unit="ps")
    return int(dut.aes_read_data.value)


async def run_aes(dut, state):
    await reset_dut(dut)
    await aes_write(dut, 0x0A, 1)
    for index, value in enumerate(AES_KEY):
        await aes_write(dut, 0x10 + index, value)
    await aes_write(dut, 0x08, 1)
    await aes_wait_status(dut, 1)

    for block in range(state.iterations):
        vector = block & 3
        for word, value in enumerate(AES_PLAINTEXT[vector]):
            await aes_write(dut, 0x20 + word, value)
        await aes_write(dut, 0x08, 2)
        await aes_wait_status(dut, 2)
        for word, expected in enumerate(AES_CIPHERTEXT[vector]):
            actual = await aes_read(dut, 0x30 + word)
            state.check("AES ciphertext word", actual, expected)
            state.fold(actual)
        state.transactions += 1
    status = await aes_read(dut, 0x09)
    state.check("AES ready", status & 1, 1)


async def run_fcs(dut, state):
    await reset_dut(dut)
    for packet in range(state.iterations):
        length = frame_length(packet)
        expected = 0xFFFFFFFF
        beat = 0
        for offset in range(0, length, 8):
            chunk = min(8, length - offset)
            data = 0
            for lane in range(chunk):
                value = frame_byte(packet, offset + lane)
                data |= value << (lane * 8)
                expected = crc32_byte(expected, value)
            if (packet + beat) % 17 == 0:
                await FallingEdge(dut.clk)
                dut.fcs_tvalid.value = 0
                await RisingEdge(dut.clk)
            await FallingEdge(dut.clk)
            dut.fcs_tdata.value = data
            dut.fcs_tkeep.value = (1 << chunk) - 1
            dut.fcs_tlast.value = int(offset + chunk == length)
            dut.fcs_tvalid.value = 1
            await RisingEdge(dut.clk)
            beat += 1
        await Timer(1, unit="ps")
        actual = int(dut.fcs_result.value)
        state.check("Ethernet FCS", actual, ~expected)
        state.fold(actual)
        state.transactions += 1
    dut.fcs_tvalid.value = 0
    dut.fcs_tlast.value = 0


@cocotb.test()
async def open_cores_benchmark(dut):
    workload = os.environ.get("OPEN_CORE_BENCH_WORKLOAD", "picorv32_firmware")
    iterations = int(os.environ.get("OPEN_CORE_BENCH_ITERS", "100"))
    if iterations <= 0:
        raise ValueError("OPEN_CORE_BENCH_ITERS must be positive")

    state = BenchState(workload, iterations)
    initialize_inputs(dut)
    start_clock(dut.clk)
    start = time.perf_counter()
    if workload == "picorv32_firmware":
        await run_picorv32(dut, state)
    elif workload == "secworks_aes128":
        await run_aes(dut, state)
    elif workload == "ethernet_fcs64":
        await run_fcs(dut, state)
    else:
        raise ValueError(f"unsupported open-core workload: {workload}")

    wall_ms = (time.perf_counter() - start) * 1000.0
    sim_cycles = (int(get_sim_time("fs")) + 1_000_000) // 2_000_000
    print(
        "OPEN_CORE_BENCH_RESULT "
        f"mode=cocotb workload={workload} iterations={iterations} "
        f"transactions={state.transactions} checks={state.checks} "
        f"sim_cycles={sim_cycles} checksum={state.checksum} "
        f"failures={state.failures} wall_ms={wall_ms:.3f}"
    )
    assert state.failures == 0
