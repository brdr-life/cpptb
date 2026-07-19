# Four-state values

:::{warning}
End-to-end four-state simulation is not currently available with CPPTB's
Verilator backend. Verilator 5.050 and current upstream development expose an
experimental `--fourstate` option, but do not yet preserve X/Z storage, net
resolution, or DPI `bval` transport. CPPTB rejects unknown writes rather than
silently converting them to zero.
:::

CPPTB separates the four-state **value model** from simulator **execution
capability**. `LogicBits<W>` can represent, compare, format, and inspect any
width of `0`, `1`, `X`, and `Z`. Whether those values may be written to and
propagated through RTL depends on the selected simulator backend.

## Constructing values

`LogicBits<W>` stores the SystemVerilog A and B planes used by DPI. The mapping
for each bit is:

| A | B | Logic state |
|---:|---:|:---|
| 0 | 0 | `0` |
| 1 | 0 | `1` |
| 1 | 1 | `X` |
| 0 | 1 | `Z` |

Use binary text for the clearest mixed-state stimulus:

```cpp
using cpptb::Bits;
using cpptb::LogicBits;
using cpptb::LogicState;

const auto request = LogicBits<8>::from_string("10xz_0011");
const auto known = LogicBits<8>::from_uint(0xa5);
```

Underscores are ignored, while the number of binary digits must exactly match
the width. Lowercase and uppercase `x` and `z` are accepted.

Known packed data can be promoted without copying through an intermediate
integer:

```cpp
Bits<137> payload;
payload.set_word(0, 0x1234'5678u);
const auto logic_payload = LogicBits<137>::from_bits(payload);
```

Code that already has explicit planes can use `from_planes()`:

```cpp
Bits<8> aval = Bits<8>::from_uint(0xa3);
Bits<8> bval = Bits<8>::from_uint(0x30);
const auto request = LogicBits<8>::from_planes(aval, bval);
```

`from_dpi_words()` and `dpi_words<T>()` provide the allocation-free conversion
for generated transport code. Ordinary testbenches normally do not need those
two functions.

## Inspecting values

The value exposes both whole-value and per-bit queries:

```cpp
const auto value = LogicBits<8>::from_string("10xz_0011");

test.expect("contains unknown", !value.is_known());
test.expect("contains X", value.contains_x());
test.expect("contains Z", value.contains_z());
test.expect_eq("bit 5", value.state(5), LogicState::X);

const Bits<8>& aval = value.aval();
const Bits<8>& bval = value.bval();
```

`value_bits()` and `unknown_bits()` are descriptive aliases for `aval()` and
`bval()`. `to_bits()` succeeds only when every bit is known; converting a value
that contains X or Z aborts with a focused `LogicBits` diagnostic.

`expect_eq()` formats `LogicBits` values as width-qualified binary, retaining X
and Z in failure reports.

## Hierarchical signal APIs

Elaborated four-state internal signals expose explicit logic operations:

```cpp
const auto known = LogicBits<8>::from_uint(0xa5);

dut.core.control.deposit_logic(known);
test.expect_eq("deposited control", dut.core.control.get_logic(), known);

dut.core.control.force_logic(known);
test.expect_eq("forced control", dut.core.control.get_logic(), known);
dut.core.control.release();
```

These calls do not advance simulation time. Add `Delay`, an edge trigger, or a
scheduling phase only when the testbench needs RTL to react or settle.

The ordinary `get()`, `deposit()`, and `force()` methods are the explicit
two-state path. On the current Verilator backend, `get_logic()` and logic writes
remain useful with known `0/1` values. A `deposit_logic()` or `force_logic()`
containing X or Z aborts before transport and reports the operation, full
hierarchical path, and simulator limitation.

Top-level ports and interface members currently retain their normal `get()`,
`set()`, `drive()`, and `high_z()` APIs. CPPTB will add `get_logic()`,
`set_logic()`, and `drive_logic()` there only after a backend passes the same
end-to-end capability and conformance requirements.

## Experimental capability gate

The future Verilator path is guarded by an explicit project option:

```sh
cpptb build --experimental-four-state
```

or:

```toml
[build]
experimental_four_state = true
```

This is a capability request, not a promise that the installed Verilator
supports four-state execution. CPPTB builds and runs a cached semantic probe
before compiling the DUT. The probe checks all of the following:

1. A SystemVerilog variable retains literal X and Z values.
2. An undriven net resolves to Z and conflicting drivers resolve to X.
3. DPI transport from SystemVerilog to C++ retains `svLogicVecVal::bval`.
4. A C++ to SystemVerilog to C++ DPI round trip retains both planes.

The current Verilator release fails those checks, so the build exits with a
summary of the unavailable capabilities. A future passing probe still stops
with an integration-pending diagnostic until every conformance item below has
passed and the framework capability is deliberately enabled. Do not put
`--fourstate` in `build.verilator_args`; CPPTB rejects that bypass because
Verilator can accept the flag while still coercing X/Z values. `--rebuild`
refreshes the cached probe after changing or upgrading the simulator.

Normal two-state builds never run this probe and retain their existing
transport and performance path.

## Enablement criteria

Verilator support can be enabled only after all four semantic checks pass and
the CPPTB conformance suite verifies:

- scalar, wide, array, interface-member, and hierarchy reads and writes;
- X/Z propagation through representative combinational and sequential RTL;
- inout high-impedance and contention resolution;
- force, release, deposit, and readback behavior; and
- exact C++ and pure-SystemVerilog peers with the normal performance guard.

Until then, use known values with Verilator and run X/Z-sensitive tests on a
standards-compliant four-state simulator through the portable backend work.
The [hierarchy guide](hierarchy.md) covers generated access, while
[interfaces and inouts](interfaces.md) covers direction and drive intent.

The upstream status is documented in Verilator's
[`--fourstate` option](https://verilator.org/guide/latest/exe_verilator.html#cmdoption-fourstate),
its [unsupported-option diagnostic](https://github.com/verilator/verilator/blob/v5.050/src/V3Options.cpp#L1051-L1057),
and the [DPI conversion helpers](https://github.com/verilator/verilator/blob/v5.050/include/verilated_dpi.h#L69-L110)
that currently discard or clear `bval`.
