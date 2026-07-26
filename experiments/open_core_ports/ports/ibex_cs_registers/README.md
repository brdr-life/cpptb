# CS registers on cpptb

A port of Ibex's `dv/cs_registers` testbench: random CSR transactions driven at
`ibex_cs_registers`, each scored against a C++ model of what the registers
should have done.

**Status: the port works. It stops at transaction 1,119 on a gap in upstream's
reference model**, which it can demonstrate rather than assume. See
[What the port found](#what-the-port-found).

Two things came out of building it. The control -- upstream's own
`tb_cs_registers` -- **drives zero transactions and reports `TEST PASSED` on
Verilator 5.050**, so it could not have found this. And the model does not
implement Ibex's MML write suppression, so it expects a PMP configuration write
that the RTL correctly refuses.

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

## The control does not work on this Verilator

Built from upstream's own core file with the command Ibex's CI uses:

    fusesoc --cores-root=. run --target=sim --tool=verilator \
      lowrisc:ibex:tb_cs_registers

The `build_so` pre-build hook ran and `reg_dpi.so` linked, so the build is as
upstream intends. Running it:

    Simulation timeout of 500000 cycles reached, shutting down simulation.
    [Reg driver] drove: 0 register transactions

    //-------------//
    // TEST PASSED //
    //-------------//

Half a million cycles, **zero transactions, and a pass**. Without `-c` it never
terminates, because it never reaches the 10,000 transactions that would ask it
to stop; it ran for twenty minutes at full CPU before being killed.

The driver object exists -- its `OnFinal` printed that line, so `env_initial`
ran and built the environment -- but `driver_tick`'s lookup in `reg_dpi.cc`
never finds it, so `OnClock` is never called. The root cause is not established
here; what is established is the outcome.

Ibex pins `VERILATOR_VERSION=v4.210` in `ci/vars.env`. This repository uses
5.050, which every other port here runs on unchanged. So the most likely
explanation is that this testbench has not been exercised on Verilator 5, and
something about how it registers its DPI interfaces across the `reg_dpi.so`
boundary no longer works. That has not been confirmed against 4.210.

Two things follow. The obvious one: there is no usable control at this Verilator
version, so the mismatch below stays unexplained. The less obvious one is worth
more -- **a testbench that silently checks nothing and reports success is a
worse failure than one that crashes**, and this port drives 1,119 real
transactions where the original drives none.

## What the port found

The run stops at transaction 1,119 on a `CSR Set` to `PMPCfg0`:

    Write data: a392b5f3   Read data: 9f978f   Expected rdata: 9a9f978f

The observed value is the expected one with its top byte missing -- region 3's
configuration. The model applied that byte; the RTL refused it. The RTL is
right, and the model is missing a rule.

Ibex suppresses a PMP configuration write outright under one condition:

    // When MSECCFG.MML is set cannot add new regions allowing M mode execution
    // unless MSECCFG.RLB is set
    assign pmp_cfg_wr_suppress[i] = pmp_mseccfg_q.mml                   &
                                    ~pmp_mseccfg_q.rlb                  &
                                    is_mml_m_exec_cfg(pmp_cfg_wdata[i]);

where `is_mml_m_exec_cfg` is true when the new byte has `lock` set and
`{read, write, exec}` is one of `001`, `010`, `011`, `101`.

Every part of that condition holds here:

| | |
| --- | --- |
| `mseccfg` reads `0x00000003` throughout the run | `MML=1`, `MMWP=1`, `RLB=0` |
| the byte the model applied is `0x9a` | `L=1`, `R=0`, `W=1`, `X=0` |
| so `{read, write, exec}` is `010` | in the suppressed set |

`mseccfg` was checked by logging every transaction to `0x747`; it reads `3` on
all seven of them, so MML is set and RLB is clear well before transaction 1,119.

Upstream's model has no equivalent. `PmpCfgRegister::RegisterWrite` masks by
`GetLockMask()` alone, which is `lock & !RLB`. The model *is* aware that MML
changes the rules -- `HandleReservedVals` returns early with "No reserved
L/R/W/X values when MML Set" -- so it knows MML relaxes which encodings are
legal, but not that MML also *blocks* writes creating M-mode-executable regions.
The relaxation is implemented and the restriction is not, which is why the model
accepts precisely the byte the RTL rejects.

So this is a gap in upstream's reference model, reachable from its own random
stimulus. It is worth reporting to lowRISC.

Two reasons upstream would not have seen it. Its driver runs during reset and
consumes random draws this port does not, so the two explore different sequences
from the same seed and it may simply never reach this case. And more decisively,
on Verilator 5.050 its testbench drives nothing at all.

The port is left stopping here rather than working around it. Adding the missing
rule to the model would let the run reach 10,000 transactions, but the model
would then be one this port had edited, and it is only worth anything as an
independent reference.
