import inspect
import os
import time

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge


IRQ_ENABLE = 0x000
IRQ_PENDING = 0x004
IRQ_SET_PENDING = 0x008
IRQ_CLEAR_PENDING = 0x00C
EVENT_ENABLE = 0x010
EVENT_PENDING = 0x014
EVENT_CLEAR_PENDING = 0x01C
SLEEP_CTRL = 0x020
SLEEP_STATUS = 0x024


class BenchState:
    def __init__(self):
        self.checks = 0
        self.failures = 0
        self.sequence_done = False

    def check(self, actual, expected):
        self.checks += 1
        actual = int(actual)
        expected = int(expected)
        if actual == expected:
            return

        self.failures += 1
        if self.failures <= 8:
            print(
                f"COCOTB_BENCH_MISMATCH actual=0x{actual:08x} "
                f"expected=0x{expected:08x}"
            )


def start_clock(signal, period_ns):
    clock = Clock(signal, period_ns, unit="ns")
    maybe_coro = clock.start()
    if inspect.isawaitable(maybe_coro):
        cocotb.start_soon(maybe_coro)
    return clock


def drive_apb_idle(dut):
    dut.PADDR.value = 0
    dut.PWDATA.value = 0
    dut.PWRITE.value = 0
    dut.PSEL.value = 0
    dut.PENABLE.value = 0


async def apb_write(dut, state, byte_addr, data):
    await FallingEdge(dut.HCLK)
    dut.PADDR.value = byte_addr & 0xFFF
    dut.PWDATA.value = data
    dut.PWRITE.value = 1
    dut.PSEL.value = 1
    dut.PENABLE.value = 0

    await FallingEdge(dut.HCLK)
    dut.PENABLE.value = 1

    await RisingEdge(dut.HCLK)
    await ReadOnly()
    state.check(dut.PREADY.value, 1)
    state.check(dut.PSLVERR.value, 0)

    await FallingEdge(dut.HCLK)
    drive_apb_idle(dut)


async def apb_read_expect(dut, state, byte_addr, expected):
    await FallingEdge(dut.HCLK)
    dut.PADDR.value = byte_addr & 0xFFF
    dut.PWDATA.value = 0
    dut.PWRITE.value = 0
    dut.PSEL.value = 1
    dut.PENABLE.value = 0

    await FallingEdge(dut.HCLK)
    dut.PENABLE.value = 1

    await RisingEdge(dut.HCLK)
    await ReadOnly()
    state.check(dut.PRDATA.value, expected)
    state.check(dut.PREADY.value, 1)
    state.check(dut.PSLVERR.value, 0)

    await FallingEdge(dut.HCLK)
    drive_apb_idle(dut)


async def reset_driver(dut):
    dut.HRESETn.value = 0
    dut.irq_i.value = 0
    dut.event_i.value = 0
    dut.fetch_enable_i.value = 1
    dut.core_busy_i.value = 0
    drive_apb_idle(dut)

    await ClockCycles(dut.HCLK, 5)
    dut.HRESETn.value = 1
    await ClockCycles(dut.HCLK, 4)


async def wait_reset_released(dut):
    while int(dut.HRESETn.value) == 0:
        await RisingEdge(dut.HCLK)


async def traffic_sequence(dut, state, iterations):
    await wait_reset_released(dut)
    await ClockCycles(dut.HCLK, 2)

    await apb_read_expect(dut, state, IRQ_ENABLE, 0)
    await apb_read_expect(dut, state, IRQ_PENDING, 0)
    await apb_read_expect(dut, state, EVENT_PENDING, 0)

    for i in range(iterations):
        irq_bit = 1 << ((i % 30) + 1)
        sw_bit = 1 << ((i * 7) % 31)
        event_bit = 1 << (i % 16)

        await apb_write(dut, state, IRQ_PENDING, 0)
        await apb_write(dut, state, IRQ_ENABLE, irq_bit | sw_bit)

        dut.irq_i.value = irq_bit
        await ClockCycles(dut.HCLK, 2)
        await apb_read_expect(dut, state, IRQ_PENDING, irq_bit)

        dut.irq_i.value = 0
        await apb_write(dut, state, IRQ_SET_PENDING, sw_bit)
        await ClockCycles(dut.HCLK, 1)
        await apb_read_expect(dut, state, IRQ_PENDING, irq_bit | sw_bit)

        await apb_write(dut, state, IRQ_CLEAR_PENDING, irq_bit | sw_bit)
        await ClockCycles(dut.HCLK, 1)
        await apb_read_expect(dut, state, IRQ_PENDING, 0)

        await apb_write(dut, state, EVENT_ENABLE, event_bit)
        dut.event_i.value = event_bit
        await ClockCycles(dut.HCLK, 2)
        await apb_read_expect(dut, state, EVENT_PENDING, event_bit)

        dut.event_i.value = 0
        await apb_write(dut, state, EVENT_CLEAR_PENDING, event_bit)
        await ClockCycles(dut.HCLK, 1)
        await apb_read_expect(dut, state, EVENT_PENDING, 0)

        if i % 4 == 0:
            await apb_write(dut, state, SLEEP_CTRL, 1)
            await ClockCycles(dut.HCLK, 4)
            await apb_read_expect(dut, state, SLEEP_STATUS, 1)
            state.check(dut.clk_gate_core_o.value, 0)
            state.check(dut.fetch_enable_o.value, 0)

            dut.event_i.value = event_bit
            await ClockCycles(dut.HCLK, 3)
            await apb_read_expect(dut, state, SLEEP_CTRL, 0)
            state.check(dut.clk_gate_core_o.value, 1)

            dut.event_i.value = 0
            await apb_write(dut, state, EVENT_CLEAR_PENDING, event_bit)
            await ClockCycles(dut.HCLK, 1)
            await apb_read_expect(dut, state, EVENT_PENDING, 0)

    state.sequence_done = True


async def irq_monitor(dut, state):
    await wait_reset_released(dut)

    while not state.sequence_done:
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        irq = int(dut.irq_o.value)
        expected = 0 if irq == 0 else 1
        actual = 0 if irq == 0 else 1 if (irq & (irq - 1)) == 0 else 0
        state.check(actual, expected)


async def sleep_monitor(dut, state):
    await wait_reset_released(dut)

    while not state.sequence_done:
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        if int(dut.clk_gate_core_o.value) == 0:
            state.check(dut.fetch_enable_o.value, 0)


@cocotb.test()
async def apb_event_benchmark(dut):
    iterations = int(os.environ.get("BENCH_ITERS", "1000"))
    state = BenchState()

    start_clock(dut.HCLK, 2)
    start_clock(dut.clk_i, 2)

    start = time.perf_counter()
    cocotb.start_soon(reset_driver(dut))
    sequence = cocotb.start_soon(traffic_sequence(dut, state, iterations))
    irq_mon = cocotb.start_soon(irq_monitor(dut, state))
    sleep_mon = cocotb.start_soon(sleep_monitor(dut, state))

    await sequence
    await RisingEdge(dut.HCLK)
    await irq_mon
    await sleep_mon
    wall_ms = (time.perf_counter() - start) * 1000.0

    print(
        f"COCOTB_BENCH_RESULT iterations={iterations} checks={state.checks} "
        f"wall_ms={wall_ms:.3f} failures={state.failures}"
    )
    assert state.failures == 0
