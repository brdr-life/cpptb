# Ibex's icache testbench on cpptb

`ports/ibex_icache_uvm` runs Ibex's `dv/uvm/icache` unmodified on Verilator 5,
and all ten of its tests pass. This is the same block-level testbench written
against cpptb: the same design, the same RTL sources from the same fusesoc
description, the same stimulus generator, and the same scoreboard.

Three of the ten tests are ported. Measurements and the side-by-side comparison
are in [RESULTS.md](RESULTS.md).

## Building and running

```sh
export PATH="$HOME/.local/bin:$PATH"          # fusesoc 2.4.3
python3 fusesoc_setup.py --check              # cpptb.toml matches the graph
uv run --frozen cpptb build --project .
python3 run_tests.py                          # three tests, three seeds
python3 run_tests.py --compare                # and the UVM baseline beside it
```

`--compare` runs `ports/ibex_icache_uvm` as well, which needs a built baseline
and z3 on `PATH`; `run_tests.py` puts z3 there itself through `local_deps.py`,
and refuses to report anything if it cannot find one. A UVM_HIGH log is about
40 MB per run and lands in `ports/ibex_icache_uvm/build/results`.

The build is about a minute from empty and the three tests take under half a
second between them.

## The design under test

`ibex_icache` cannot be elaborated on its own. It has tag and data RAM ports
that something has to answer and a scrambling-key port that something has to
service, so upstream's `dv/uvm/icache/dv/tb/tb.sv` wraps it in two
`prim_ram_1p_scr` instances per way plus a key latch.
`ibex_icache_tb_top.sv` here is that module with the UVM removed: the same DUT
instantiation, the same key logic, the same RAMs, and the core-side,
memory-side and key pins lifted to the boundary so the cpptb testbench drives
them where the UVM agents drive interfaces.

Three deliberate differences from `tb.sv`:

* the parameter override is spelled `TweakInfection`. `tb.sv` writes
  `.ICacheTweakInfection`, which `ibex_icache` does not have, so `tb.sv` cannot
  elaborate as vendored on any simulator. `ports/ibex_icache_uvm` patches the
  same thing.
* `ibex_icache_ram_if` is not instantiated. It exists to corrupt RAM read data
  for `ibex_icache_ecc`, which is not one of the three ported tests, and its
  clocking block and SVA belong to the UVM environment.
* the key, nonce and valid bit are inputs rather than fields of a
  `push_pull_if`. The key device is a coroutine in `testbench.cpp`.

The RTL list comes from fusesoc, as it does for the UVM baseline.
`ibex_icache_cpptb.core` is a CAPI=2 core depending on
`lowrisc:ibex:ibex_icache` and the primitives the wrapper instantiates, and
`fusesoc_setup.py` resolves it and writes `cpptb.toml`. That file is committed
so `cpptb build` needs no wrapper script, and `--check` fails if it no longer
matches what fusesoc resolves. The resolution is **77 sources, one include
directory and five Verilator control files**.

Two dependencies had to be named that upstream does not name:
`lowrisc:prim:ram_1p_adv` declares neither `lowrisc:prim:mubi` nor
`lowrisc:prim:flop`, although `prim_ram_1p_adv.sv` imports `prim_mubi_pkg` and
`prim_ram_1p_scr.sv` instantiates `prim_flop`. Nothing notices upstream because
every flow that reaches `ram_1p_scr` also reaches a core that does depend on
them.

## What is ported

| in `testbench.cpp` | upstream |
| --- | --- |
| `core_stimulus` | `ibex_icache_core_base_seq`, and the three virtual sequences' knobs |
| `drive_branch`, `drive_req`, `read_insn`, `lower_req`, `maybe_invalidate` | `ibex_icache_core_driver` and `ibex_icache_core_if` |
| `core_monitor` | `ibex_icache_core_monitor` |
| `mem_monitor` | `ibex_icache_mem_monitor` and `ibex_icache_mem_resp_seq` |
| `mem_grant_driver`, `mem_responder` | `ibex_icache_mem_driver` and `ibex_icache_mem_if` |
| `key_device` | `push_pull_agent` in Pull/Device mode |
| `hash32`, `read_data`, `is_mem_error` | `ibex_icache_mem_model` |
| `Scoreboard` | `ibex_icache_scoreboard` |

The scoreboard is the point of the exercise, and it is ported whole:

* **`check_compatible`.** Every returned fetch is checked against every memory
  seed the model still holds, with `is_fetch_compatible_1` for aligned,
  errored and compressed fetches and `is_fetch_compatible_2` over every pair of
  seeds for a misaligned uncompressed one. The seed list is grown on every new
  seed and truncated at the first branch after an invalidation, and `no_cache`
  narrows the search to the seed in force at the last branch whenever the cache
  has been disabled since before it.
* **the address sequence.** Every fetch must arrive at the address the model
  expected, which advances by 2 or 4 according to the returned opcode bits and
  is reset by a branch.
* **the busy line.** Checked whenever busy falls and just before a pending
  memory request is answered, which is where upstream checks it and for the
  reason its comment gives.
* **the caching ratio.** 850 fetches inside a 250-word range with the cache
  enabled, no error and no invalidation completes a window, and the window then
  requires `(reads * 53/32) / insns <= 67%`. Same window length, same width
  bound, same arithmetic.
* **old-value tracking.** `possible_old` and `actual_old` are counted, which is
  what `ibex_icache_oldval_test::check_phase` would need.

The stimulus draws are taken from `ports/ibex_icache_uvm/build_tb.py` rather
than from upstream's constraint code, because that is the stimulus the baseline
actually runs: Verilator gets `dist` wrong in three separate ways, so every
distribution in that environment is already drawn directly there, with the same
buckets and the same weights. `base_addr`, the branch target, `enable`,
`invalidate`, `new_seed`, `num_insns`, `req_low_cycles`, the invalidate pulse
length, the grant delay and the response delay are all reproduced from it.

## Timing

The design samples on the rising edge. `co_await RisingEdge` resumes before the
design has evaluated that edge, so it yields the value `@(posedge clk)` reads
in the Active region and is where the monitors read. Driving there would be
wrong, because `set()` is immediate and the value would be captured by the edge
being awaited.

Everything that drives a pin therefore works from a **drive point**: the
instant just after a falling edge. A value written there is captured by the
next rising edge and by no earlier one, which is exactly what upstream's
`default output negedge` clocking blocks produce. Every driving task is entered
at a drive point and returns at a drive point, and none of them opens with a
wait; a task that re-anchored itself would place its first write one cycle
later than the UVM driver does. Each of them was traced against its upstream
counterpart edge by edge, and `run_tests.py`'s `cycles/item` is what would
notice if one of them were off.

## What is not ported, and what each would need

Seven of the ten tests.

| test | what it needs |
| --- | --- |
| `ibex_icache_invalidation` | `gap_between_invalidations = 4`. A few lines. |
| `ibex_icache_many_errors` | `mem_err_shift = 1` and `gap_between_seeds` handling. A few lines. |
| `ibex_icache_back_line` | `ibex_icache_core_back_line_seq`, a second core sequence that walks to the end of a cache line. Small. |
| `ibex_icache_oldval` | the sequence knobs plus `ibex_icache_oldval_test::check_phase`, which requires 1000 possible-old fetches and at least 5% actual. The counters are already there. |
| `ibex_icache_ecc` | `ibex_icache_ram_if` in the wrapper, so RAM read data can be corrupted a way at a time. The wrapper would grow real logic, and cpptb would have to reach 78-bit arrays. |
| `ibex_icache_stress_all` | `ibex_icache_combo_vseq`: several child sequences back to back, each handing its state to the next. |
| `ibex_icache_stress_all_with_reset` | the above plus mid-test reset, which is the one thing the ported drivers deliberately leave out. |

Mid-test reset is the real boundary. Upstream's `wait_clks`, `read_insn` and
`drive_responses` all carry a stop-on-reset fork, and the scoreboard has
`start_reset` and `reset` hooks. None of the three ported tests resets the DUT
after `dut_init`, so those paths are omitted rather than written and left
untested. Adding them is the first thing the two stress tests would need.

## Divergences from the UVM environment

Stated plainly, because a port that quietly checks less is not a comparison.

1. **The scrambling key device is simpler.** Upstream is a `push_pull_agent`
   whose 194-bit `d_data` is randomised per transaction, so the key, the nonce
   and the valid bit are all random and a request is answered only when the
   valid bit happens to come up set. `key_device` answers every request with a
   valid key after a randomised 0 to 10 cycle delay. The key and nonce are
   random either way. What is lost is the occasional long wait for a key, which
   lengthens an invalidation.
2. **An errored memory response drives zero rather than `'X`.** Verilator has
   no X, so the baseline drives zero too; this is a statement about both
   harnesses rather than a difference between them.
3. **No mid-test reset**, as above.
4. **No functional coverage.** Neither side collects any: the baseline compiles
   `ibex_icache_fcov_if.sv` and the agents' covergroups but does not pass
   `--coverage`. Neither has covergroups that run.
5. **No assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
   design's SVA compiles away on both sides, and the two protocol checker
   modules do nothing in the baseline. This port does not instantiate them at
   all, which is the same amount of checking and less pretence.
6. **The chatty second pass is gone.** On a `check_compatible` failure upstream
   re-runs the search with logging on to say why each seed did not match. Here
   the failure message carries the address, the data, the two error flags and
   the number of seeds tried, and there is no second pass.
7. **`ibex_icache_core_protocol_checker` and `ibex_icache_mem_protocol_checker`
   are not ported.** They are SVA-only and compile to nothing under Verilator,
   so nothing is lost against the baseline, but they are real checks on a
   simulator that runs assertions.

Nothing else in the scoreboard is missing.

## What a failure reports

`ICACHE_CORRUPT_GRANT=N` flips one bit of the memory response for the Nth
granted request, which is how the checking above was shown to be live rather
than merely quiet:

```
$ ICACHE_CORRUPT_GRANT=1 CPPTB_TEST=ibex_icache_caching CPPTB_RANDOM_SEED=124 ./Vdpi_ibex_icache_cpptb
cpptb: testbench.cpp:393: fetch at 0xd3da30b2 returned 0x076cfe05 (err 0/1),
  which is compatible with none of the 1 available seeds
```

One line, before a debugger: the source location of the check, the fetch
address, what came back, both error flags, and how many seeds were tried. Over
grants 1 to 450 of that run, corruption is caught whenever the corrupted word
reaches the core -- once as 182 separate failures, because the corrupted line
stayed in the cache and was returned by every later hit -- and is silently
correct when the fill buffer holding it is discarded before it does, which is
the cache behaving properly.

## Files

```
ibex_icache_cpptb.core       CAPI=2 description of the RTL half
ibex_icache_tb_top.sv        tb.sv without the UVM
fusesoc_setup.py             resolves the graph, writes and checks cpptb.toml
cpptb.toml                   generated; do not edit
testbench.cpp                agents, memory model, scoreboard, three tests
run_tests.py                 runs the port, and the baseline beside it
shims/                       one reduced Verilator case, see RESULTS.md
```
