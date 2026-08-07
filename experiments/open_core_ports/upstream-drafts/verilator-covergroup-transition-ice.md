# Draft: two internal errors on covergroup transition bins over enum items

**Target:** `verilator/verilator`

**Suggested title:** Internal error on covergroup transition bins naming enum
items; casting the items faults a second way

---

## Body

A covergroup transition bin whose states are enum items dies with an internal
error at parse-elaboration; rewriting the same bin with the items cast to
their enum type gets past that error and hits a second, unlocated internal
fault. Found compiling Ibex's functional-coverage interface
(`core_ibex_fcov_if.sv`), which declares 19 such transition bins over its
controller FSM enum; both faults reproduce from the ~20 lines below.

## Repro (fault 1)

```systemverilog
interface fcov_if(input logic clk);
  typedef enum logic [3:0] { RESET, BOOT_SET, FIRST_FETCH, DECODE } ctrl_fsm_e;
  ctrl_fsm_e ctrl_fsm_cs;

  covergroup fsm_cg @(posedge clk);
    cp_controller_fsm: coverpoint ctrl_fsm_cs {
      bins out_of_reset = (RESET => BOOT_SET);
      bins out_of_boot_set = (BOOT_SET => FIRST_FETCH);
    }
  endgroup

  fsm_cg cg = new();
endinterface

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  fcov_if u_if(clk);
  initial begin
    #100 $finish;
  end
endmodule
```

```
$ verilator --binary --coverage t_enum_transition.sv
%Error: Internal Error: t_enum_transition.sv:7:28: ../V3Ast.h:1061:
    AstNode is not of expected type, but instead has type 'ENUMITEMREF'
    7 |       bins out_of_reset = (RESET => BOOT_SET);
      |                            ^~~~~
```

## Repro (fault 2)

Casting each item to the enum type — the natural workaround — trades the typed
error for an unlocated fault:

```systemverilog
      bins out_of_reset = (ctrl_fsm_e'(RESET) => ctrl_fsm_e'(BOOT_SET));
      bins out_of_boot_set = (ctrl_fsm_e'(BOOT_SET) => ctrl_fsm_e'(FIRST_FETCH));
```

```
$ verilator --binary --coverage t_enum_transition_cast.sv
%Error: Verilator internal fault, sorry. Suggest trying --debug --gdbbt
```

The fault persists under `verilator_bin_dbg` (no further location is
reported). Integer states in the same shape compile, so it is the enum-typed
transition list specifically.

## Environment

Verilator 5.050 2026-07-01 rev v5.050, Ubuntu x86_64, from source.

## Context

Ibex's `dv_fcov_macros.svh` currently guards all covergroups out under
Verilator; enabling them surfaces these two faults plus 456 individually
discarded constructs. The transition bins are the only hard stop — everything
else degrades to warnings.
