import inspect
import os
import time

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge
from cocotb.utils import get_sim_time


TIMER_REG_TIMER = 0
TIMER_REG_CTRL = 1
TIMER_REG_CMP = 2

SPI_STATUS = 0
SPI_CLKDIV = 1
SPI_CMD = 2
SPI_ADDR = 3
SPI_LEN = 4
SPI_DUMMY = 5
SPI_TXFIFO = 6
SPI_RXFIFO = 8
SPI_INTCFG = 9

I2C_PRESCALER = 0
I2C_CTRL = 1
I2C_STATUS = 3
I2C_TX = 4
I2C_CMD = 5


def timer_addr(timer_index, reg_index):
    return ((timer_index & 1) << 4) | ((reg_index & 3) << 2)


def word_addr(word_index):
    return word_index << 2


class BenchState:
    def __init__(self):
        self.checks = 0
        self.failures = 0
        self.active_sequences = 3

    def check(self, actual, expected, label="check"):
        self.checks += 1
        actual = int(actual)
        expected = int(expected)
        if actual == expected:
            return

        self.failures += 1
        if self.failures <= 8:
            print(
                f"COCOTB_PERIPHERAL_MISMATCH {label} "
                f"actual=0x{actual:08x} expected=0x{expected:08x}"
            )


class ApbBus:
    def __init__(self, dut, prefix):
        self.PADDR = getattr(dut, f"{prefix}_PADDR")
        self.PWDATA = getattr(dut, f"{prefix}_PWDATA")
        self.PWRITE = getattr(dut, f"{prefix}_PWRITE")
        self.PSEL = getattr(dut, f"{prefix}_PSEL")
        self.PENABLE = getattr(dut, f"{prefix}_PENABLE")
        self.PRDATA = getattr(dut, f"{prefix}_PRDATA")
        self.PREADY = getattr(dut, f"{prefix}_PREADY")
        self.PSLVERR = getattr(dut, f"{prefix}_PSLVERR")

    def idle(self):
        self.PADDR.value = 0
        self.PWDATA.value = 0
        self.PWRITE.value = 0
        self.PSEL.value = 0
        self.PENABLE.value = 0


class ApbMaster:
    def __init__(self, bus, clock, state):
        self.bus = bus
        self.clock = clock
        self.state = state

    async def write(self, byte_addr, data):
        await FallingEdge(self.clock)
        self.bus.PADDR.value = byte_addr & 0xFFF
        self.bus.PWDATA.value = data
        self.bus.PWRITE.value = 1
        self.bus.PSEL.value = 1
        self.bus.PENABLE.value = 0

        await FallingEdge(self.clock)
        self.bus.PENABLE.value = 1

        await RisingEdge(self.clock)
        await ReadOnly()
        self.state.check(self.bus.PREADY.value, 1, f"write PREADY addr=0x{byte_addr:03x}")
        self.state.check(self.bus.PSLVERR.value, 0, f"write PSLVERR addr=0x{byte_addr:03x}")

        await FallingEdge(self.clock)
        self.bus.idle()

    async def read_expect(self, byte_addr, expected):
        await FallingEdge(self.clock)
        self.bus.PADDR.value = byte_addr & 0xFFF
        self.bus.PWDATA.value = 0
        self.bus.PWRITE.value = 0
        self.bus.PSEL.value = 1
        self.bus.PENABLE.value = 0

        await FallingEdge(self.clock)
        self.bus.PENABLE.value = 1

        await RisingEdge(self.clock)
        await ReadOnly()
        self.state.check(self.bus.PRDATA.value, expected, f"read PRDATA addr=0x{byte_addr:03x}")
        self.state.check(self.bus.PREADY.value, 1, f"read PREADY addr=0x{byte_addr:03x}")
        self.state.check(self.bus.PSLVERR.value, 0, f"read PSLVERR addr=0x{byte_addr:03x}")

        await FallingEdge(self.clock)
        self.bus.idle()

    async def read_ok(self, byte_addr):
        await FallingEdge(self.clock)
        self.bus.PADDR.value = byte_addr & 0xFFF
        self.bus.PWDATA.value = 0
        self.bus.PWRITE.value = 0
        self.bus.PSEL.value = 1
        self.bus.PENABLE.value = 0

        await FallingEdge(self.clock)
        self.bus.PENABLE.value = 1

        await RisingEdge(self.clock)
        await ReadOnly()
        self.state.check(self.bus.PREADY.value, 1, f"read-ok PREADY addr=0x{byte_addr:03x}")
        self.state.check(self.bus.PSLVERR.value, 0, f"read-ok PSLVERR addr=0x{byte_addr:03x}")

        await FallingEdge(self.clock)
        self.bus.idle()


def start_clock(signal, period_ns):
    clock = Clock(signal, period_ns, unit="ns")
    maybe_coro = clock.start()
    if inspect.isawaitable(maybe_coro):
        cocotb.start_soon(maybe_coro)
    return clock


def int_value(signal):
    return int(signal.value)


async def wait_reset_released(dut):
    while int_value(dut.HRESETn) == 0:
        await RisingEdge(dut.HCLK)


async def reset_driver(dut):
    timer_bus = ApbBus(dut, "timer")
    spi_bus = ApbBus(dut, "spi")
    i2c_bus = ApbBus(dut, "i2c")

    dut.HRESETn.value = 0
    timer_bus.idle()
    spi_bus.idle()
    i2c_bus.idle()

    dut.spi_status.value = 0
    dut.spi_data_tx_ready.value = 1
    dut.spi_data_rx.value = 0
    dut.spi_data_rx_valid.value = 1
    dut.i2c_scl_pad_i.value = 1
    dut.i2c_sda_pad_i.value = 1

    await ClockCycles(dut.HCLK, 8)
    dut.HRESETn.value = 1
    await ClockCycles(dut.HCLK, 8)


async def wait_timer_irq(dut, state, bit, max_cycles):
    seen = False
    for _ in range(max_cycles):
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        if ((int_value(dut.timer_irq) >> bit) & 1) != 0:
            seen = True
            break
    state.check(1 if seen else 0, 1, "timer irq wait")


async def timer_sequence(dut, state, iterations):
    await wait_reset_released(dut)
    await ClockCycles(dut.HCLK, 2)

    apb = ApbMaster(ApbBus(dut, "timer"), dut.HCLK, state)
    for i in range(iterations):
        timer_index = i & 1
        cmp_value = 3 + (i & 7)
        prescale = (i >> 2) & 3
        ctrl = 1 | (prescale << 3)

        await apb.write(timer_addr(timer_index, TIMER_REG_TIMER), 0)
        await apb.write(timer_addr(timer_index, TIMER_REG_CMP), cmp_value)
        await apb.write(timer_addr(timer_index, TIMER_REG_CTRL), ctrl)
        await wait_timer_irq(dut, state, timer_index * 2 + 1, 48 + prescale * cmp_value * 4)
        await apb.write(timer_addr(timer_index, TIMER_REG_CTRL), 0)

        if i % 8 == 0:
            await apb.write(timer_addr(timer_index, TIMER_REG_CMP), 0)
            await apb.write(timer_addr(timer_index, TIMER_REG_TIMER), 0xFFFF_FFFE)
            await apb.write(timer_addr(timer_index, TIMER_REG_CTRL), 1)
            await wait_timer_irq(dut, state, timer_index * 2, 8)
            await apb.write(timer_addr(timer_index, TIMER_REG_CTRL), 0)

    state.active_sequences -= 1


async def spi_sequence(dut, state, iterations):
    await wait_reset_released(dut)
    await ClockCycles(dut.HCLK, 4)

    apb = ApbMaster(ApbBus(dut, "spi"), dut.HCLK, state)
    for i in range(iterations):
        div = (i * 13 + 7) & 0xFF
        cmd = (0x1100_0000 ^ (i * 0x0101_0101)) & 0xFFFF_FFFF
        addr = (0x5500_0000 ^ (i * 0x0011_0021)) & 0xFFFF_FFFF
        length = (
            ((i & 0xFF) << 24)
            | (((i + 3) & 0xFF) << 16)
            | (((i + 5) & 0x3F) << 8)
            | ((i + 7) & 0x3F)
        )
        dummy = (((0xAB00 | (i & 0xFF)) << 16) | (0x1200 | (i & 0xFF))) & 0xFFFF_FFFF
        intcfg = (
            0x8000_0000
            | ((i & 0x1F) << 24)
            | (((i + 1) & 0x1F) << 16)
            | (((i + 2) & 0x1F) << 8)
            | ((i + 3) & 0x1F)
        )
        status = (0xA500_0000 | ((i & 0xF) << 8) | (i & 0xFF)) & 0xFFFF_FFFF
        rx = (0xCAFE_0000 ^ (i * 0x1021)) & 0xFFFF_FFFF

        dut.spi_status.value = status
        dut.spi_data_rx.value = rx
        dut.spi_data_rx_valid.value = 1
        dut.spi_data_tx_ready.value = 0 if (i & 3) == 0 else 1

        await apb.write(word_addr(SPI_STATUS), ((i & 0xF) << 8) | (i & 0xF))
        await ReadOnly()
        state.check(dut.spi_csreg.value, i & 0xF, "spi csreg")

        await apb.write(word_addr(SPI_CLKDIV), div)
        await ReadOnly()
        state.check(dut.spi_clk_div.value, div, "spi clk_div")

        await apb.write(word_addr(SPI_CMD), cmd)
        await apb.write(word_addr(SPI_ADDR), addr)
        await apb.write(word_addr(SPI_LEN), length)
        await apb.write(word_addr(SPI_DUMMY), dummy)
        await apb.write(word_addr(SPI_INTCFG), intcfg)
        await ReadOnly()

        state.check(dut.spi_cmd.value, cmd, "spi cmd")
        state.check(dut.spi_addr.value, addr, "spi addr")
        state.check(dut.spi_cmd_len.value, length & 0x3F, "spi cmd_len")
        state.check(dut.spi_addr_len.value, (length >> 8) & 0x3F, "spi addr_len")
        state.check(dut.spi_data_len.value, (length >> 16) & 0xFFFF, "spi data_len")
        state.check(dut.spi_dummy_rd.value, dummy & 0xFFFF, "spi dummy_rd")
        state.check(dut.spi_dummy_wr.value, (dummy >> 16) & 0xFFFF, "spi dummy_wr")

        await apb.write(word_addr(SPI_TXFIFO), 0x1357_0000 | i)
        await apb.read_expect(word_addr(SPI_RXFIFO), rx)
        await apb.read_expect(word_addr(SPI_STATUS), status)
        await apb.read_expect(word_addr(SPI_CMD), cmd)
        await apb.read_expect(word_addr(SPI_ADDR), addr)
        await apb.read_expect(word_addr(SPI_LEN), length & 0xFFFF_3F3F)
        await apb.read_expect(word_addr(SPI_DUMMY), dummy)

    state.active_sequences -= 1


async def i2c_sequence(dut, state, iterations):
    await wait_reset_released(dut)
    await ClockCycles(dut.HCLK, 6)

    apb = ApbMaster(ApbBus(dut, "i2c"), dut.HCLK, state)
    for i in range(iterations):
        prescaler = 4 + (i & 7)
        tx = 0x40 | (i & 0x3F)

        dut.i2c_scl_pad_i.value = 1
        dut.i2c_sda_pad_i.value = 1
        await apb.write(word_addr(I2C_PRESCALER), prescaler)
        await apb.write(word_addr(I2C_CTRL), 0xC0)
        await apb.write(word_addr(I2C_TX), tx)
        await apb.read_expect(word_addr(I2C_PRESCALER), prescaler)
        await apb.read_expect(word_addr(I2C_CTRL), 0xC0)
        await apb.read_expect(word_addr(I2C_TX), tx)

        await apb.write(word_addr(I2C_CMD), 0x90)
        await ClockCycles(dut.HCLK, 20 + (i & 0xF))
        await apb.read_ok(word_addr(I2C_STATUS))
        await apb.write(word_addr(I2C_CMD), 0x01)

        if (i & 3) == 0:
            dut.i2c_sda_pad_i.value = 0
            await ClockCycles(dut.HCLK, 4)
            dut.i2c_sda_pad_i.value = 1
            await ClockCycles(dut.HCLK, 4)

    state.active_sequences -= 1


async def timer_monitor(dut, state):
    await wait_reset_released(dut)
    while state.active_sequences != 0:
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        state.check(int_value(dut.timer_irq) & ~0xF, 0, "timer irq high bits")


async def spi_monitor(dut, state):
    await wait_reset_released(dut)
    while state.active_sequences != 0:
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        state.check(1 if int_value(dut.spi_clk_div_valid) <= 1 else 0, 1, "spi clk_div_valid")
        state.check(1 if int_value(dut.spi_data_tx_valid) <= 1 else 0, 1, "spi data_tx_valid")
        state.check(1 if int_value(dut.spi_data_rx_ready) <= 1 else 0, 1, "spi data_rx_ready")


async def i2c_monitor(dut, state):
    await wait_reset_released(dut)
    while state.active_sequences != 0:
        await RisingEdge(dut.HCLK)
        await ReadOnly()
        state.check(1 if int_value(dut.i2c_interrupt) <= 1 else 0, 1, "i2c interrupt")
        state.check(1 if int_value(dut.i2c_scl_padoen_o) <= 1 else 0, 1, "i2c scl_padoen")
        state.check(1 if int_value(dut.i2c_sda_padoen_o) <= 1 else 0, 1, "i2c sda_padoen")


@cocotb.test()
async def peripheral_suite_benchmark(dut):
    iterations = int(os.environ.get("PERIPHERAL_SUITE_ITERS", "1000"))
    state = BenchState()

    start_clock(dut.HCLK, 2)

    start = time.perf_counter()
    cocotb.start_soon(reset_driver(dut))
    timer_task = cocotb.start_soon(timer_sequence(dut, state, iterations))
    spi_task = cocotb.start_soon(spi_sequence(dut, state, iterations))
    i2c_task = cocotb.start_soon(i2c_sequence(dut, state, iterations))
    timer_mon = cocotb.start_soon(timer_monitor(dut, state))
    spi_mon = cocotb.start_soon(spi_monitor(dut, state))
    i2c_mon = cocotb.start_soon(i2c_monitor(dut, state))

    await timer_task
    await spi_task
    await i2c_task
    await RisingEdge(dut.HCLK)
    await timer_mon
    await spi_mon
    await i2c_mon

    wall_ms = (time.perf_counter() - start) * 1000.0
    sim_cycles = int(get_sim_time("ns") // 2)
    print(
        f"COCOTB_PERIPHERAL_RESULT iterations={iterations} checks={state.checks} "
        f"sim_cycles={sim_cycles} wall_ms={wall_ms:.3f} failures={state.failures}"
    )
    assert state.failures == 0
