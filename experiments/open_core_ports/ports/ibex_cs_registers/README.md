# CS registers on cpptb

A port of Ibex's `dv/cs_registers` testbench: random CSR transactions driven at
`ibex_cs_registers`, each scored against a C++ model of what the registers
should have done.

**Status: not finished.** It builds, runs, and drives 1,119 transactions before
hitting a mismatch that is probably a divergence between upstream's model and
Ibex's RTL rather than a defect in this port -- but that is not established,
because upstream's own testbench has not been built as a control. See below.

## Why this one is different

Every other port here runs a program and checks what it produced. This runs no
program. It is constrained stimulus and a scoreboard, which is a different axis
of the framework entirely, and it is a submodule rather than a system: four RTL
files instead of 141.

What cpptb replaces is the reason the port is interesting:

| upstream | here |
| --- | --- |
| `register_driver`, `reset_driver` | coroutines in `testbench.cpp` |
| `register_environment`, `simctrl` wiring | the test function |
| `env_dpi`, `reg_dpi`, `rst_dpi` (`.cc` and `.sv`) | nothing; cpptb drives ports |
| `tb_cs_registers.sv`'s four `always_ff` DPI ticks | nothing |
| `tb_cs_registers.cc` | nothing |

Kept unmodified, because it is the reference rather than the plumbing:
`model/base_register`, `model/register_model`, `register_transaction`,
`env/simctrl`, `env/register_types`.

`dut.sv` is the one piece of new SystemVerilog, and it exists only to name the
interface: `ibex_cs_registers` has far more ports than this testbench drives, so
something has to select them. Upstream's wrapper does the same and waives
PINMISSING for the same reason.

## Running it

```sh
uv run cpptb build --project experiments/open_core_ports/ports/ibex_cs_registers
.../work/ibex_cs_registers/cpptb/ibex_cs_registers/obj/Vdpi_ibex_cs_registers
```

`CSR_SEED` sets the random seed, defaulting to 0 as upstream's
`+ntb_random_seed` does.

## Three things this port got wrong, and what they cost

Recorded because each was quiet rather than loud, and each took a run-and-stare
cycle to find.

1. **The model was never reset.** Reset is driven before the monitoring loop
   starts, so the loop never observes `rst_ni` low and never calls
   `RegisterReset`. Upstream gets it free: its monitor runs from the first edge.
   Almost every register resets to zero and so does the model's initial state,
   so the first dozen transactions agree; the first mismatch is `mhpmevent9`,
   whose reset value is `0x1 << (9 - 3)`, and it reads as a design bug.

2. **`co_await RisingEdge` resumes before the design evaluates that edge.** So
   it is the right place to *sample* -- every signal still holds the value it
   had during the cycle just ending, which is what `always_ff @(posedge)` sees
   -- and the wrong place to *drive*, because the value is picked up by the very
   edge being awaited. Driving there makes a write commit in the cycle it is
   presented and read back as the value just written:

       Operation: CSR Write   Write data: 4b639620
       Read data: 4b639620    Expected rdata: 0

   This is the hazard `tb_cs_registers.sv` describes in its comment about
   non-blocking assignments. cpptb has no NBA to reach for, so driving moves to
   the falling edge, where it is stable well before the edge that commits it.

3. **The monitor needs upstream's gate.** `monitor_tick` in `reg_dpi.cc` only
   captures when `(csr_access && (csr_op_en || illegal_csr)) || !rst_n`. Without
   it, every idle cycle is captured as a read of CSR 0 and the model expects an
   illegal instruction for an undefined register.

And one in cpptb: **sources listed in `verilator_args` do not reach the
discovery build.** `cpptb build` compiles the testbench a second time,
standalone, to discover its clocks, and that link sees only `[testbench]
sources`. Putting the model there fails at discovery with undefined references
before Verilator has run at all. The co-simulation port does not hit this only
because its calls into extra C++ are behind `#ifdef CPPTB_COSIM`, which the
discovery build does not define.

## What is left

The run stops at transaction 1,119 on a `CSR Set` to `PMPCfg0`:

    Write data: a392b5f3   Read data: 9f978f   Expected rdata: 9a9f978f

The observed value is the expected one with the top byte missing -- region 3's
config, whose lock bit is set. The model handles PMP locking, in
`PmpCfgRegister::GetLockMask`, but bypasses it when `mseccfg.RLB` is set, and
Ibex treats `mseccfg` as an illegal CSR. So the model can be told to ignore a
lock that the RTL still enforces.

That is a plausible divergence between upstream's model and Ibex, reached
because this port's random stream differs from upstream's -- upstream's driver
runs during reset and consumes draws this one does not, so the two explore
different sequences from the same seed.

**It is not established.** Confirming it needs upstream's `tb_cs_registers`
built and run as a control, which is the discipline every other port here
follows and which this one has not had yet. Until then the mismatch could
equally be a fourth thing this port has wrong.
