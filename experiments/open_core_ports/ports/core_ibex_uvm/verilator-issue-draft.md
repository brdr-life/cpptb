# Draft issue for verilator/verilator

Not filed. Reproducer: `shims/verilator_constraint_item_name.sv`.

Fix and tests are pushed to a fork and ready to open as a PR:
`brdr-life/verilator`, branch `fix-randomize-with-item-shadowing`.
The same diff is in `verilator-item-fix.patch`.

---

**Title:** `randomize() with` binds the array-iterator name `item`, shadowing a user variable of that name

In `randomize() with {...}`, any reference to a variable named `item` resolves to
the object being randomized instead of the user's variable. Array methods like
`find with (item > 3)` are supposed to introduce `item` as the default iterator
(LRM 7.12.1). `randomize() with` should not, but it does.

Verilator 5.050 2026-07-01 rev v5.050, Linux x86_64, z3 4.8.12.

### Where it comes from

`V3LinkDot.cpp:2198`, in `visit(AstWithParse*)`:

```cpp
string name = "item";
...
if (funcrefp->name() != "randomize") {   // randomize() never overrides the name
```

then unconditionally at `:2232`:

```cpp
new AstLambdaArgRef{argFl, name, false}
```

So a `randomize() with` clause gets a lambda argument literally called `item`,
which shadows anything of that name in scope.

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
    bit ok;

    ok = r1.randomize() with { addr == item.addr; };
    $display("named item : ok=%0d addr=%08h expected=%08h", ok, r1.addr, item.addr);

    ok = r2.randomize() with { addr == itm.addr; };
    $display("named itm  : ok=%0d addr=%08h expected=%08h", ok, r2.addr, itm.addr);
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
named item : ok=1 addr=21766c9b expected=80000084
named itm  : ok=1 addr=80000084 expected=80000084
```

`item.addr` became `r1.addr`, so the constraint is the tautology `addr == addr`.
It is satisfiable by any value, `randomize()` returns 1, and the field comes out
random. The generated call shows the rewritten constraint with no operand:

```cpp
randomizer.hard("(__Vbv (= addr addr))"s, "t_1.sv", 0xaU,
                "    void'(r1.randomize() with { addr == item.addr; });");
```

The same line for `itm` correctly formats in the runtime value of
`__Vthis->__PVT__itm->__PVT__addr`.

### Expected

Both lines should print `addr=80000084`.

### The same rebinding, made visible

Break the tautology and the rebinding shows up as an unsatisfiable constraint
rather than a wrong value:

```systemverilog
ok = r1.randomize() with { addr == item.addr + 1; };
```

```
%Warning-UNSATCONSTR: m1.sv:12: Unsatisfied constraint:
  'ok = r1.randomize() with { addr == item.addr + 1; };'
m1: randomize returned 0 (addr=f256f70c, item.addr=80000084)
```

It became `addr == addr + 1`. And if the user's `item` has a member the
randomize target does not, it fails at elaboration, naming the wrong class:

```systemverilog
class other_t;   bit [31:0] tag;  endclass
class payload_t; rand bit [31:0] addr; endclass
class seq_t;
  other_t item;                                  // has tag, not addr
  task run(); payload_t r1 = new();
    void'(r1.randomize() with { addr == item.tag; });
```

```
%Error: m2.sv:14:46: Member 'tag' not found in class 'payload_t'
```

A bare, undotted `item` produces C++ that does not compile:

```
error: 'item' was not declared in this scope; did you mean 'tm'?
```

### Scope

| | |
| --- | --- |
| `obj.randomize() with {...}` | affected, class member and plain local alike |
| `std::randomize(v) with {...}` | affected; surfaces as `Unsupported: Member call on object 'LAMBDAARGREF 'item''` |
| `constraint` block declared in a class | not affected, no `with` clause involved |
| `q.find with (item > 3)` and other array methods | correct, `item` is the LRM default iterator there |

Case sensitive and whole token. `ITEM`, `Item`, `item1`, `my_item`, `_item`,
`item_`, `items`, `itemx`, `xitem`, `m_item` all work. Of 39 identifiers tested,
only lowercase `item` fails.

### Workaround

Alias the handle to a differently named local first:

```systemverilog
automatic payload_t alias_h = item;
void'(b.randomize() with { addr == alias_h.addr; });
```

`this.item` does not work, but that looks like a separate problem: `this`
inside `randomize() with` also binds to the randomize target, so `this.itm`
gives `Can't find definition of scope/variable: 'itm'`.

### Suggested fix

`V3LinkDot.cpp` refers to that lambda argument by the literal string `"item"` in
three places. Giving the randomize case a reserved name instead fixes all of the
above and leaves array methods alone. Patch against `v5.050-99-gf8fb1d6` in
`verilator-item-fix.patch`:

```cpp
static const char* const RANDOMIZE_WITH_OBJ_NAME = "__Vrandwith_obj";
...
string name
    = funcrefp->name() == "randomize" ? RANDOMIZE_WITH_OBJ_NAME : "item";
```

plus the two `AstLambdaArgRef{..., "item", false}` constructions in the
randomize-with resolution paths. Nothing downstream matches on the name:
`V3Randomize.cpp` and `V3Width.cpp` dispatch on the `LambdaArgRef` node type.

Built and tested. Before and after, same files:

| Case | v5.050 | patched |
| --- | --- | --- |
| `addr == item.addr` | `addr=21766c9b` | `addr=80000084` |
| `addr == item.addr + 1` | `UNSATCONSTR`, returns 0 | `ok=1 addr=80000085` |
| `item` of another class type | `Member 'tag' not found in class 'payload_t'` | `addr=5a5a5a5a` |
| `q.find with (item > 3)` | `size=2 first=4` | `size=2 first=4` |

### Tests

Two new `test_regress` tests, split because the two failure modes mask each
other in one file:

`t_randomize_with_item_name` covers the cases that go wrong at run time. On an
unpatched build:

```
%Error: t/t_randomize_with_item_name.v:35:  got=5b6f55b9 exp=80000084
```

- member handle named `item`, dereferenced in the constraint
- the same, not a tautology (`item.addr + 1`), which returns 0 unpatched
- a local, rather than a member, also named `item`
- `q.find with (item > 3)` in the same class, which must keep working

`t_randomize_with_item_name_types` covers the cases that fail before you get to
run time. On an unpatched build:

```
%Error: t/t_randomize_with_item_name_types.v:38:45: Member 'tag' not found in class 'Payload'
```

- `item` of a class type with no member of the randomized name
- an undotted scalar named `item`, which emits C++ that does not compile

Both fail on `v5.050-99-gf8fb1d6` and pass with the patch. Together with every
`t_constraint*`, `t_randomize*`, `t_array_query_with` and `t_case_inside_with_x`
test: **181 passed, 0 failed**.

### Why it matters

`item` is a common name in UVM sequences and monitors. I hit this in Ibex's
`dv/uvm/core_ibex` testbench, where a response sequence ties its response to the
monitored request with `addr == item.addr`. That became `addr == addr`, so every
memory response went to a random address, the memory model reported the address
as uninitialised and returned `0x0000`, and the core executed `c.unimp` at its
reset vector. The cosim scoreboard then flagged a mismatch on the first
instruction. Nothing in that chain pointed back at the constraint.

The tautology case is the dangerous one. It returns success, emits no warning,
and the value is plausible.
