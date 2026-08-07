# Static hierarchy discovery — scoping report (roadmap milestone 10)

Status: scoping complete. Untracked working document; not part of the docs tree.

Everything below is labeled **[measured]** (reproduced on this machine during this
scoping) or **[inferred]** (read from code, not executed). Prototype commands and
numbers are in §6.

---

## 1. The current mechanism, mapped

### 1.1 Pipeline

`cpptb build` (`tools/codegen/cpptb_codegen/build.py:312-368`) on a cache miss:

1. **Pass 1 codegen** — `generate_sources(...)` without `clock_config`/`access_config`:
   emits the typed `<target>_dut.hpp`, the binding include, and
   `discover_<target>_clocks.cpp` (a `main` for discovery).
2. **Discovery compile** — `c++ -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY
   discover_<target>_clocks.cpp <testbench sources> -o discover_design`
   (`build.py:344-355`). This is a full compile *and link* of the user testbench.
3. **Discovery run** — `discover_design clocks.json access.json`
   (`build.py:356-360`), with **no timeout** (`_CommandLog.run` passes none).
4. **Pass 2 codegen** — `generate_sources(..., clock_config=clocks.json,
   access_config=access.json)` regenerates the SV wrapper, DPI adapter, and
   `dut.hpp` with clocks baked and transport pruned.
5. **Verilator build** of the regenerated sources.

`cpptb list` forces the same build first (`cli.py:146-156`), so a hanging
discovery also hangs `list`.

### 1.2 What the discovery binary actually executes — and what it records

This is the central finding of the scoping: **the access set is already produced
without executing test bodies; only the clock configuration comes from
execution.**

- Every signal accessor under `CPPTB_HIERARCHY_DISCOVERY` calls
  `detail::mark_access<Path, Operation>()` (`include/cpptb/hierarchy.hpp:148-163`)
  or `discovery::mark_port_edge<Id>()`
  (`include/cpptb/access_discovery.hpp:16-30`). Both take the address of an
  `inline static Registration` member of a template — a **static-initialization
  side effect of template instantiation**. Merely compiling `dut.foo.get()`
  anywhere, including in a branch that never runs, registers the access when the
  process starts, before `main`.
- Runtime-indexed element access (`Memory::Element`, `SelectedSignal`,
  `SelectedMemory[ND]`) is generated with `AccessPathSet<AllCandidatePaths...>`
  (`generate_dpi_bindings.py:1860-1897`), which fans out to *all* candidate paths
  at static init and ignores the runtime index. The genuinely-runtime
  `RuntimeAccessPaths::mark` is only the *default* template argument; **no
  generated header instantiates it** ([measured] — 69/69 `Selected*`
  instantiations in `benchmarks/peripheral_suite/.../peripheral_suite_dut.hpp`
  carry `AccessPathSet`; 0 uses of `RuntimeAccessPaths` anywhere outside
  `hierarchy.hpp`).
- Therefore `access.json` content is fully determined at static-init time.
  Executing the tests adds **nothing** to it.
- The generated `main` (`render_cpp_clock_discovery`,
  `generate_dpi_bindings.py:3926-3961`) calls
  `discover_registered_clocks` (`include/cpptb/clock_discovery.hpp:285-311`),
  which loops over **every registered test** and calls `run_registered_test`
  against a fake binding. `run_registered_test` spawns the test coroutine, which
  runs **up to its first suspension point** (the scheduler is never pumped).
  Clock discovery therefore observes exactly the `start_clock` calls in each
  test's prologue — which matches the runtime contract, since the registration
  window closes when `init()` returns: `"clocks must be started before the
  test's first await"` (`include/cpptb/dpi_runtime.hpp:662-670`).
  Only after all prologues run does `main` write `access.json` from the
  already-populated static-init vector.

### 1.3 The failure modes, characterized [measured]

Probed with a throwaway project (copy of `examples/counter`, since deleted):

- **`co_await RisingEdge{...}` on an undriven signal before `start_clock` does
  NOT hang the build.** The coroutine suspends at the first await and discovery
  moves on. The result is worse in a way the roadmap doesn't mention: a
  **silently empty `clocks.json`** (`{"clocks": []}` [measured]) — the final
  binary has no clock driver at all, and the failure surfaces at test runtime.
- **A value-dependent poll loop before the first `co_await` DOES hang the
  build** [measured]: `while (dut.count.get() != 3u) {}` spins forever because
  discovery `get()` fabricates `{}` (zero). The build stalled until an external
  90 s kill; the last `build.log` line is the *discovery compiler command*, so
  the stall visually attributes to the compiler — exactly the roadmap's
  "reads as a compiler problem".
- The same class covers any non-terminating or blocking prologue work: file
  loads, sockets, cosim ELF loading, and user **static initializers** (which
  also run in the discovery binary).

### 1.4 File formats

`clocks.json` (writer: `clock_discovery.hpp:186-209`):

```json
{"schema_version": 1, "clocks": [
  {"port": "write_clk", "signal_id": 1, "period_fs": 4000000,
   "phase_fs": 0, "initial_value": 0, "primary": true}, ...]}
```

`primary` = first clock registered by the first test. `initial_value` = the
value the test wrote to the port before registering (observed from the fake
binding). Conflicting configs across tests abort discovery
(`"clock '%s' has conflicting configurations across tests"`).

`access.json` (writer: `hierarchy.hpp:253-278`):

```json
{"schema_version": 1,
 "accesses": [{"path": "resolved_value", "operation": "force"}, ...],
 "port_edges": [0]}
```

Operations: get, deposit, force, release, rising_edge, falling_edge, any_edge,
get_logic, deposit_logic, force_logic. `port_edges` are top-level-port signal
IDs whose `operator coro::Signal()` was instantiated.

Neither format is documented in the docs tree; `docs/running-tests.md:149`
describes them as produced "by compiling and inspecting the authored C++
testbench" — which, notably, is what milestone 10 would make true.

### 1.5 All consumers [inferred, verified against code by a dedicated sweep]

| Output | Consumer | Effect |
|---|---|---|
| `accesses` | `render_cpp_hierarchy_transport` + SV export generation | Only listed path/operation pairs get DPI export functions and transport `switch` cases. Unlisted internals produce **zero** SV and C++ — they are absent, not "wide". |
| `accesses` (`*_edge`) | `kEdgeObserverSignalIds` / `kTransportlessEdgeSignalIds` | Hierarchy edge observers. |
| `port_edges` | `auto_edge_observers` (`generate_dpi_bindings.py:1284-1318`) | Only awaited ports get `always @(port)` observer tasks. **Without an access plan, every one-bit output port gets one** — this is the only port-level pruning that exists. |
| `clocks.json` | SV wrapper, registered mode | Clock driver task per clock with the **half-period baked as an SV literal** (`next_edge = $realtime + 5ns;`, `generate_dpi_bindings.py:5924-5955`); no `clock_config` DPI import at all in this mode. Primary-clock literal drives `sim_cycles++`, which feeds `timeout_cycles`. |
| `clocks.json` | Port direction sets | A fully clock-owned port is dropped from the driven transport (`driven_port_names`, `:1174-1183`) but stays `Writable=true, Driven=false` in the binding spec. |
| `clocks.json` | `kRegisteredClockConfigs` in the adapter | Runtime validation: every discovered clock must be started; period/phase/initial-value drift aborts with `"clock timing changed after clock discovery; regenerate the testbench wrapper"` (`dpi_runtime.hpp:672-767`). |
| neither | `runner.py`, `project.py`, `cpptb list` output | No dependency on the file contents. `list` depends on the build completing, nothing more. Runner timeouts are wall-clock; the cycle watchdog is baked into the adapter. |
| both | `Makefile` benchmark flows (`:58-59, :939-963, :1230-1347`) | Reproduce the same two-phase build by hand; checked-in generated artifacts embed `kRegisteredClockConfigs`. |

**Failure shape of an under-approximated access set** [inferred, message text
verified]: the typed member compiles, and the transport `switch` falls through
to `cpptb: hierarchy <op> was not selected for signal <id>` + `std::abort()` at
runtime (`generate_dpi_bindings.py:2476-2484`). A missing port edge observer
degrades to starvation/deadlock diagnostics rather than a clean message.

### 1.6 The "measured pruning benefit" does not exist as a number

A dedicated sweep of docs, benchmarks, OPTIMIZATION_NOTES.md, and tracked result
JSONs found **no recorded measurement** of usage-pruned transport vs unpruned.
The contract is qualitative (`docs/hierarchy.md:180-185`: "An unused
hierarchical object … adds no simulator process, callback, or runtime transport
cost"). Two structural data points stand in for it:

- `benchmarks/peripheral_suite`: 484 hierarchy proxies in the typed view, **0**
  exports in the wrapper (nothing accessed).
- Top-level **port value transport was never usage-pruned** — it is sized by
  direction only (`input_word_count`/`output_word_count`,
  `generate_dpi_bindings.py:5224-5234`). Discovery prunes ports only in the
  edge-observer dimension.

Consequence: the roadmap's "keep its measured pruning quality" is best
discharged by **bit-identical `access.json`**, which the recommended approach
provides by construction (§3).

### 1.7 Scale data points [measured]

| | discovery compile | discovery run | outputs |
|---|---|---|---|
| `examples/counter` | ~2 s (inside a 15.7 s full build) | ms | 0 accesses, edges `[0]`, 1 clock |
| `examples/multiclock` | similar | ms | 0 accesses, edges `[1,6,13]`, 2 clocks (4 ns / 6 ns + 1 ns phase) |
| `examples/fault_injection` | 2.5 s (compile-only TU) | ms | 9 accesses, edges `[0]` |
| `core_ibex_cpptb` (2,516-line TB, 81k-line dut.hpp) | **74.9 s** | **0.012 s** | **0 accesses**, edges `[0]`, 1 clock (20 ns) |

The stress case's entire discovery run produces three facts (one clock, one
edge port, empty access set) in 12 ms after a 75-second compile. The compile
is the cost; the run is the hazard. Note also that `core_ibex` had to wrap its
Spike cosim in `#ifndef CPPTB_HIERARCHY_DISCOVERY` (testbench.cpp:86-100,227)
purely so the discovery **link** succeeds — evidence that the link step already
leaks into user sources.

---

## 2. Approach comparison

### A1. Compile-time registration through the typed accessors — **recommended**

This is not a redesign; it is **finishing what the code already does**. The
markers are instantiation-driven today; the only reason a process runs is to
execute the static initializers and serialize the vector. Replace "run static
initializers, then dump" with "read the records out of the object files".

Two extraction mechanisms were prototyped [measured]:

1. **Symbol scan (zero header changes).** The marker registrations are already
   visible as symbols whose mangled names carry the full path and operation:
   `nm` on the fault_injection discovery objects yields
   `AccessMarker<FixedString<15>{"resolved_value"...}, Operation::2>::registration`
   etc. — decoded, they reproduce the committed `access.json` **exactly**
   (9 access entries + 1 port edge; verified on `counter`, `fault_injection`,
   `core_ibex`). Works on compile-only `.o` files — **no link needed**.
2. **Dedicated section records (recommended implementation).** Under
   `CPPTB_HIERARCHY_DISCOVERY` only, each marker additionally instantiates a
   `[[gnu::used]] __attribute__((section("cpptb_access")))` constexpr char
   array `"CPPTB-ACCESS-v1;op<N>;<path>"`. A standalone prototype survives GCC
   `-O0` and `-O2` including a dead-branch instantiation, and extraction is a
   raw byte scan (`strings`-grade), independent of mangling schemes, demanglers
   and object formats. (Clang untested on this box — it supports the same
   attributes; macOS needs `section("__DATA,cpptb_access")`.)

New pipeline: pass-1 codegen (unchanged) → compile each testbench TU with
`-DCPPTB_HIERARCHY_DISCOVERY -c` (no link, no generated `main`, no execution)
→ scan the `.o` bytes → pass-2 codegen → Verilator build.

- **Access set + port edges: exact relative to today** — bit-identical output,
  because today's output comes from the same instantiations. The pre-existing
  over-approximation relative to executed truth (dead-code accesses are
  recorded) is unchanged, and its cost is the same as today's: an extra
  hierarchy export inhibits some Verilator optimization on that one signal, an
  extra edge observer costs a DPI crossing per toggle of that signal —
  only for signals the source code *mentions*.
- **Clock config: not produced by this mechanism** (periods/phases are runtime
  function arguments). See A6.
- Consumers: transport pruning unchanged; `dut.hpp` unchanged; `cpptb list`
  unchanged; clock ownership handled by A6.
- Migration: none. Existing `#ifndef CPPTB_HIERARCHY_DISCOVERY` guards keep
  working; dropping the link actually *relaxes* what testbenches must satisfy
  (link-time deps like Spike no longer need stubbing — compile guards still do).
- Toolchain deps: none added (attributes supported by GCC/Clang/MSVC-equivalents;
  fallback is the symbol scan). One caveat: a user `-flto` in `cxx_flags` turns
  `.o` into bitcode — the discovery compile is our command line, so append
  `-fno-lto` to it.
- **T-shirt: M.** Main risk: extraction robustness across compilers — mitigated
  by a parity gate (§4 step 1c).

### A2. Static analysis of the test TUs (libclang) — reject

The compiler already performs the exact analysis needed — template
instantiation *is* the alias-and-helper-robust "does this code mention this
signal" oracle, and A1 harvests its output for free. A libclang reimplementation
would have to re-derive instantiations through `auto&` aliases, helpers taking
`Dut`, and user templates, adding a heavyweight, version-sensitive dependency to
get strictly less accuracy. No consumer needs anything A1 cannot provide.

### A3. Bounded instrumented dry run (fuel) — reject

- It does not remove the objection; it contains it. User code still executes at
  build time (static initializers, prologues, fabricated-value test bodies) —
  the verbatim commitment is "produced without executing user test code".
- It buys nothing for the access set: that is static-init data; running bodies
  further adds zero entries (§1.2).
- It buys almost nothing for clocks: the runtime already rejects `start_clock`
  after the first await, so legal clock registrations are prologue-only, which
  the current non-advancing scheduler already covers. Fuel's only marginal gain
  is discovering clocks in *illegal* testbenches (my edge-wait probe), which
  then abort at runtime anyway.
- Under-approximation on value-dependent prologues remains (fabricated reads
  pick branches). On the core_ibex port specifically the question is moot: its
  access set is empty and its clock is in the prologue — fuel and full
  execution produce identical output there [measured].

### A4. Explicit declaration — keep, as the (mostly existing) escape hatch

Already 80 % built: `--clock PORT=PERIOD[,phase=...]` + `--primary-clock`
(mutually exclusive with `--clock-config`), `edge_observers` in the manifest,
and `manifest["hierarchy_accesses"]` as a direct input. Missing only a
`cpptb.toml` surface and docs. Correctness class: whatever the author declares —
validated by `validate_hierarchy_accesses`/`validate_clock_ports` at codegen
time with good errors. Right default for generated/ported benches (the Ibex
port tooling could emit it); tedious for hand-written ones, so it must not be
the primary mechanism. **T-shirt: S** on top of A1/A6.

### A5. Over-approximate everything (no pruning) — reject as default

For hierarchy internals this is not "a wider transport": every internal would
need a DPI export and Verilator public/forceable visibility, which blocks
module-level optimization — at peripheral_suite scale that is 484 exports where
today there are 0, and at core_ibex scale the 81k-line catalog makes it
absurd. For ports it means an edge-observer task (a DPI crossing per toggle) on
every one-bit output. And it is unnecessary: A1 achieves exactness for free.
The one worthwhile by-product of investigating A5: since no pruned-vs-unpruned
number was ever recorded, the milestone should **define** pruning quality as
"access.json parity", not chase a phantom benchmark.

### A6. Clocks: promote the already-implemented dynamic-clock runtime path

Not on the roadmap's candidate list, but it is the piece that actually
eliminates execution, and it **already exists**: when codegen runs without
`--clock-config`, it emits `dynamic_clocks` mode (`generate_dpi_bindings.py:
6639, 6759`) — every scalar driven port gets an SV driver task that asks the
C++ runtime at time zero via one DPI call (`cpptb_dpi_clock_config(id, 0/1/2)`,
`dpi_runtime.hpp:526-544, 1600-1604`) whether the selected test registered it
as a clock, with what half-period/phase, and whether it is primary. Non-clock
ports get `0` and the task exits immediately. `sim_cycles`/`timeout_cycles`
work through the runtime `primary_clock[]` array; output application is guarded
by `registered_clock[]` so testbench writes and the driver do not fight.

Making this the default for `cpptb build` deletes clock discovery entirely:
no `discover_*_clocks.cpp`, no `clocks.json`, no `kRegisteredClockConfigs`
drift validation (nothing can be stale anymore).

Semantic deltas vs registered mode (all [inferred] from the generated-SV sweep):

| | registered (today) | dynamic (proposed) |
|---|---|---|
| period/phase in SV | baked literals `#(5ns)` | fetched once at t=0, `#(half_period)` variable |
| clock port in driven transport | removed | kept, write guarded by `registered_clock[]` |
| primary clock | codegen literal | runtime flag (first `start_clock` wins — same rule) |
| clocks differing across tests | build-time abort in discovery | each run uses its own test's clocks (strictly more capable) |
| stale-wrapper validation | abort with "regenerate" | not needed — nothing baked |
| `start_clock` after first await | runtime abort | same runtime abort (unchanged) |
| initial value | baked from discovery observation | the test's own pre-init `set()` through the normal transport |

Costs: one DPI query per scalar driven port at t=0 (negligible), a variable
instead of literal delay in each clock task, and the clock port words staying in
the per-step output transport. All three land squarely in what the benchmark
suite's 1.10x hard guard exists to adjudicate — that re-qualification is the
real work item. **T-shirt: M** (config-level flip + benchmark/test requalification),
**L** if a guard fails and per-project baked-clock fallback via A4 must be
productized in the same milestone.

---

## 3. Recommendation

**A1 (section-record extraction from compile-only objects) for the access set
and port edges, A6 (dynamic clocks) for the clock configuration, A4 documented
as the explicit override for both.** All four roadmap commitments are met
simultaneously — no constraint needs to be relaxed:

- *No execution of user test code*: nothing is linked or run; discovery is a
  compile plus a byte scan.
- *Hang-the-build gone structurally*: the only surviving build step is the
  compiler itself; the poll-loop testbench that hangs today's build [measured]
  would build fine and hang (or time out) in the user's own test run, where a
  wall-clock timeout and correct attribution already exist.
- *Builds today ⇒ builds identically*: no source changes; existing
  `#ifndef CPPTB_HIERARCHY_DISCOVERY` guards still compile; link-only guards
  become unnecessary but harmless.
- *Pruning quality*: bit-identical `access.json` by construction, enforced by a
  parity gate during migration. (Ports were never value-pruned; nothing to lose.)

Bonus fixed en route: the silently-empty-`clocks.json` failure (§1.3) becomes
impossible — clock config is no longer guessed at build time at all.

### Step breakdown for roadmap milestone 10

1. **Access plan without execution.**
   a. Under `CPPTB_HIERARCHY_DISCOVERY`, have `mark_access`/`mark_port_edge`
      additionally instantiate a `used`+`section("cpptb_access")` constexpr
      record `"CPPTB-ACCESS-v1;<op>;<path>"` / `"...;edge;<id>"`
      (`hierarchy.hpp`, `access_discovery.hpp`; ~40 lines).
   b. In `build.py`: compile each testbench TU with `-c` (drop the generated
      discovery `main` and the link), scan the `.o` bytes in
      `cpptb_codegen`, write `access.json` in the existing schema. Force
      `-fno-lto` on this compile.
   c. **Parity gate:** temporary CI/dev flag that also builds and runs the old
      discovery binary and asserts byte-identical `access.json` across all
      examples, `tests/codegen` fixtures, benchmarks, and the Ibex ports —
      on GCC and Clang. Delete the executed path once green.
2. **Clocks without execution.**
   a. `build.py` stops producing/consuming `clocks.json`; final builds use
      `dynamic_clocks` mode (today's no-`--clock-config` path).
   b. Re-qualify the benchmark pairs under the existing 1.10x hard guard
      (registered-baked vs dynamic wrappers), and waveform-compare
      `examples/multiclock` (phase, initial value, primary/timeout counting).
   c. Remove `clock_discovery.hpp`'s execution path, `render_cpp_clock_discovery`,
      `kRegisteredClockConfigs` staleness errors; update
      `tests/codegen/test_build.py` and the Makefile benchmark flows.
   d. If (and only if) 2b fails the guard: keep dynamic as default and expose
      baked clocks via `cpptb.toml` `[clocks]` → the existing `--clock`
      "generated" path (A4) for the affected projects.
3. **Surface and docs.** `cpptb.toml` `[discovery]` overrides (declared accesses,
   edge observers, clocks); document the `access.json` schema and the new
   discovery contract; update `docs/running-tests.md` (its "compiling and
   inspecting" wording becomes literally true), `docs/hierarchy.md`,
   `docs/clocking.md`; record the parity result as the milestone's
   pruning-quality evidence.

Estimates: step 1 **M**, step 2 **M** (L with 2d), step 3 **S**.

---

## 4. Top three risks, with early detection

1. **Dynamic-clock mode fails the 1.10x performance guard** (variable delays,
   clock port left in output transport, t=0 DPI queries). *Detect early:* run
   step 2b's benchmark comparison **first**, before deleting anything — it needs
   only existing codegen flags (`--clock-config` vs not) on the checked-in
   benchmark manifests. *Contain:* 2d fallback (declared baked clocks) is
   bounded and already mostly implemented.
2. **Object-record extraction gaps on some toolchain** (records dropped by an
   optimizer, LTO/bitcode objects, macOS section naming, exotic mangling if the
   symbol-scan fallback is used). *Detect early:* the step 1c parity gate runs
   both mechanisms on every example/benchmark on GCC and Clang from day one;
   any divergence is a hard failure with a diff of two small JSON files.
   [Measured so far: GCC 13 `-O0`/`-O2` keep the records; Clang untested here.]
3. **Hidden reliance on discovery-run side effects** — testbenches that count on
   the build aborting for cross-test clock conflicts, or on the
   "clock timing changed after discovery" staleness check; both checks disappear
   and per-test clock configs become legal. *Detect early:* full
   `tests/` + examples + Ibex-ports sweep in step 2b explicitly greps for the
   removed diagnostics in expectations; release notes call out the behavior
   change (cross-test clock conflicts stop failing the build and become
   per-test configurations).

---

## 5. What was measured vs inferred

**Measured on this machine:** counter/multiclock/fault_injection builds and
their metadata; core_ibex discovery compile (74.9 s) and run (0.012 s) with
output equality against committed metadata; symbol-scan parity with
`access.json` on fault_injection (9+1 records) and core_ibex (1 record);
compile-only `.o` marker visibility (2.5 s TU compile); section-record
prototype under GCC `-O0`/`-O2` including a dead-branch instantiation; the
edge-wait-first probe (no hang, empty `clocks.json`); the poll-loop probe
(build hang, killed externally at 90 s, last log line a compiler command).

**Inferred from code (not executed):** the registered-vs-dynamic SV differences
(from `generate_dpi_bindings.py`); runtime clock-API behavior and validation
messages (from `dpi_runtime.hpp`); the unpruned-access runtime failure text;
Makefile benchmark-flow consumption; Clang/macOS attribute behavior.

## 6. Reproduction notes

- Counter/multiclock/fault_injection: `cpptb build --project examples/<x>
  --build-dir build`; metadata under `build/cpptb/<target>/metadata/`.
- core_ibex timing: re-ran the exact discovery compile command from
  `experiments/open_core_ports/work/core_ibex_cpptb/cpptb/core_ibex_cpptb/build.log`
  against a tmp output; ran the committed `discover_design` under
  `timeout 120` writing to tmp paths and diffed against committed metadata.
- Symbol scan: `nm -C <obj-or-binary> | grep -E 'AccessMarker|PortEdgeMarker'`;
  path characters appear literally in the mangled NTTP
  (`...FixedString<15>{...(char)114,(char)101,...}` = "resolved_value").
- Probes were built in `/tmp/cpptb-hangprobe` (removed) and `mktemp -d` dirs;
  no tracked file was modified and no repo build directory was polluted beyond
  normal example builds.
