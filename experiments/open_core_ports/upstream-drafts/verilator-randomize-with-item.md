# Draft: `randomize() with` silently drops constraints naming a variable `item`

**Target:** `verilator/verilator`

**Suggested title:** `randomize() with` constraint referencing a member named
`item` is silently ignored

---

## Body

A `randomize() with { ... }` constraint that references a class member named
`item` is silently dropped — the randomize call succeeds and the constraint
simply does not apply. The name is the trigger, not the scope: renaming the
member to `itm` makes the same constraint work.

Cause: Verilator implements `randomize() with` by reusing the array-method
`with` machinery, whose implicit iterator is named `item` (IEEE 1800-2023
7.12.1). `randomize() with` has no such iterator, but the implicit lambda
argument still takes the name `item`, so it shadows any user variable called
`item` that the constraint references. UVM sequences hit this constantly:
`item` is the conventional name for the sequence item being randomized, e.g.
`ibex_mem_intf_response_seq` in Ibex's core testbench.

## Repro

```systemverilog
class payload_t;
  rand bit [31:0] addr;
endclass

class seq_t;
  payload_t item;   // named "item"
  payload_t itm;    // same thing, different name

  task run();
    payload_t r1 = new();
    payload_t r2 = new();

    void'(r1.randomize() with { addr == item.addr; });
    $display("named item : addr=%08h expected=%08h", r1.addr, item.addr);

    void'(r2.randomize() with { addr == itm.addr; });
    $display("named itm  : addr=%08h expected=%08h", r2.addr, itm.addr);
  endtask
endclass

module top;
  initial begin : main
    automatic seq_t s = new();
    s.item = new();
    s.itm  = new();
    s.item.addr = 32'h8000_0084;
    s.itm.addr  = 32'h8000_0084;
    s.run();
    $finish;
  end
endmodule
```

Observed on 5.050: the `named item` line prints a random `addr`; the
`named itm` line prints `8000_0084`. Expected: both constrained.

## Proposed fix

A patch is attached: in `V3LinkDot.cpp`, give the implicit `randomize() with`
lambda argument a name outside the user identifier space
(`__Vrandwith_obj`) instead of `item`, keeping `item` for array methods where
the LRM defines it. Includes a `test_regress` case
(`t_randomize_with_item_name`). The patch is
`verilator-item-fix.patch` alongside this draft's source; it applies to 5.050
and the constraint then binds correctly in both spellings.
