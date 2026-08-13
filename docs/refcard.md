# Reference card

Every common operation, one line each. Details:
[Library reference](library/awaitables.md) for signatures and semantics,
[Scheduling](scheduling.md) for exactly when waits resume.

## Signals

| Operation | Syntax |
|---|---|
| Read a port | `dut.count.get()` |
| Write a port (queued, lands next edge) | `dut.enable.set(1)` |
| Write immediately | `dut.clk.set_now(0)` |
| Wide values (> 64 bits) | `dut.payload.set(Bits<256>::from_hex("0x..."))` |
| Unpacked array element (declared bounds) | `dut.lanes.at(1).set(v)` |
| Inout: drive / release the bus | `dut.sda.drive(0);` `dut.sda.high_z();` |
| Read inside the hierarchy | `dut.core.alu.state.get()` |
| Deposit inside the hierarchy (immediate) | `dut.core.regs.at(3).deposit(v)` |
| Force / release an internal signal | `dut.core.stall.force(1);` `dut.core.stall.release();` |

## Waiting

| Operation | Syntax |
|---|---|
| One clock edge | `co_await RisingEdge{dut.clk};` (also `FallingEdge`, `Edge`) |
| N clock cycles | `co_await clock_cycles(dut.clk, 8);` |
| Fixed time | `co_await Delay{10_ns};` (`_fs _ps _ns _us _ms`) |
| Post-evaluation settle (cocotb's edge instant) | `co_await RisingEdge{dut.clk}; co_await ReadWrite{};` |
| End of timestep (sample-only) | `co_await ReadOnly{};` |
| Next timestep | `co_await NextTimeStep{};` |
| Until a condition, sampled on a clock | `co_await wait_until(dut.valid, [](auto v) { return v != 0; }, dut.clk);` |

## Concurrency

| Operation | Syntax |
|---|---|
| Start a concurrent process | `auto p = test.spawn(monitor_bus(dut));` |
| Start without keeping a handle | `test.spawn_detached(background(dut));` |
| Wait for a process | `co_await p;` |
| Cancel a process | `p.cancel();` |
| Run tasks to completion together | `co_await Join{drive(dut), monitor(dut)};` |
| Race triggers, learn the winner | `size_t i = co_await First{RisingEdge{dut.irq}, Delay{1_us}};` |
| Edge wait with a deadline | `co_await with_timeout(RisingEdge{dut.done}, 3_ns)` → `TimeoutOutcome` |
| Task with a deadline | `auto r = co_await with_timeout(read_word(dut), 100_ns);` `r.timed_out()` / `r.value()` |

## Queues, events, locks

| Operation | Syntax |
|---|---|
| Bounded typed FIFO | `Queue<Packet> q{4};` (`{0}` or default = unbounded) |
| Blocking transfer | `co_await q.put(p);` `Packet p = co_await q.get();` |
| Non-blocking transfer | `q.put_nowait(p)` → `bool`; `q.get_nowait()` → `optional` |
| Broadcast notification (latching) | `Event e; e.set(); e.clear();` `co_await e.wait();` |
| Mutual exclusion (FIFO handoff) | `Lock l; co_await l.acquire(); l.release();` |
| Counting credits (starts at 0) | `Semaphore s{4}; co_await s.acquire(); s.release();` |

## Clocks

| Operation | Syntax |
|---|---|
| Initialize the pin, then start | `dut.clk.set_now(0); test.start_clock(dut.clk, 10_ns);` |
| Phase-shifted second clock | `test.start_clock(dut.read_clk, 6_ns, 1_ns);` |
| DUT-produced clock | never started — just `co_await RisingEdge{dut.div_clk};` |

## Checks and test control

| Operation | Syntax |
|---|---|
| Check and continue | `test.expect_eq("count", dut.count.get(), 42u);` `test.expect("ok", cond);` |
| Check and stop the test | `test.require_eq(...)`, `test.require(...)` |
| Warn without failing | `test.warn("saturated early");` |
| Skip from inside the test | `test.skip("needs four-state");` |
| Current simulation time | `test.now()` |

## Registering tests

| Operation | Syntax |
|---|---|
| A test | `Task<void> my_test(Dut dut, TestContext& test) { ... }` `CPPTB_REGISTER_TEST(my_test);` |
| With options | `CPPTB_REGISTER_TEST_WITH_OPTIONS(t, (::cpptb::TestOptions{.simulation_timeout = 20_us}));` |
| Parametrized case | `CPPTB_REGISTER_TEST_CASE(t, small, (Config{8}));` → runs as `t[small]` |
| Expected failure | `TestOptions{.expected_failure = true, .expected_failure_reason = "RTL-241"}` |

## Randomization

| Operation | Syntax |
|---|---|
| This process's stream | `auto& random = test.random();` |
| Uniform integer (inclusive) | `random.randint<uint32_t>(0, 99)` |
| Wide random value | `random.randbits<128>()` |
| Pick / shuffle | `random.choice(modes)`, `random.shuffle(order)` |
| Solve a constrained transaction | `test.randomize(packet);` |
| With an extra constraint | `test.randomize_with(packet, packet.length == 0);` |

## Running

| Operation | Syntax |
|---|---|
| Build, list, run | `cpptb build` · `cpptb list` · `cpptb test [NAME]` |
| One test, direct binary | `CPPTB_TEST=my_test ./build/cpptb/TARGET/obj/Vdpi_TARGET` |
| Set the seed | `CPPTB_RANDOM_SEED=0x1234 cpptb test my_test` |
| Waveforms | `cpptb test --wave` (FST; `--wave vcd` for VCD) |
| Log level | `CPPTB_LOG_LEVEL=debug` |

Full option and key tables: [cpptb command line](cli.md) ·
[cpptb.toml](cpptb-toml.md).
