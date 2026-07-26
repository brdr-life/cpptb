# Draft issue for verilator/verilator

Not filed. Reproducer lives in `shims/verilator_constraint_item_name.sv`.

---

**Title:** Inline constraint is silently ignored when it references a variable named `item`

An inline constraint that references a variable called `item` is dropped.
`randomize()` still returns 1 and the target gets a random value. Renaming the
variable to anything else makes the same constraint work.

Verilator 5.050 2026-07-01 rev v5.050, Linux x86_64, z3 4.8.12.

### Reproducer

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

```
verilator --binary --timing --top top mcve.sv -o mcve --Mdir objm
./objm/mcve
```

### Actual

```
named item : addr=21766c9b expected=80000084
named itm  : addr=80000084 expected=80000084
```

The first value is random and changes between builds. No warning or error is
printed, and `randomize()` returns 1.

### Expected

Both lines should print `addr=80000084`.

### Other things I tried

- `local::item.addr` behaves the same way. So it is not the unqualified name
  falling back to the wrong scope.
- It happens for a local variable named `item` as well as a class member.
- Renaming to `itm`, `req`, `rsp`, `m_item`, `member_item` or `data` all work.
  Only `item` fails.
- Renaming the class type makes no difference. Only the variable name matters.
- With a plain scalar named `item` instead of a class handle, the generated C++
  does not compile:

  ```
  Vtop___024unit__03a__03aitem_t__Vclpkg__0.cpp:36:45:
    error: 'item' was not declared in this scope; did you mean 'tm'?
  ```

  That looks like the same problem showing up at compile time rather than
  silently.

### Why it matters

`item` is a common name in UVM sequences and monitors. I hit this in Ibex's
`dv/uvm/core_ibex` testbench, where a response sequence ties its response to the
monitored request with `addr == item.addr`. The constraint was dropped, so every
memory response went to a random address, the memory model reported the address
as uninitialised and returned `0x0000`, and the core executed `c.unimp` at its
reset vector. The cosim scoreboard then flagged a mismatch on the first
instruction. Nothing in that chain pointed back at the constraint.

Silently returning success with an unconstrained value is the hard part. A
warning or an error would have made this quick to find.
