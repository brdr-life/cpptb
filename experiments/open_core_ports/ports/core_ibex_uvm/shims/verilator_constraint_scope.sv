// A variable named `item` is silently ignored inside `randomize() with {}`.
//
// Verilator 5.050 drops any constraint term that reads a variable called
// `item`, and randomize() still returns success. Renaming the variable is
// enough to fix it; local versus class member makes no difference, and a
// scalar named `item` does not even produce compilable C++.
//
// `item` is SystemVerilog's implicit iterator argument for the `with` clauses
// of the array-manipulation methods -- `q.find(x) with (item > 3)` -- so the
// likely cause is that scope being shared with randomize()'s `with`.
//
// This is what stopped Ibex's UVM testbench. Its memory response sequence ties
// the response to the monitored request with `addr == item.addr`, where `item`
// is the request. With the constraint dropped, every response went to a random
// address, the model reported that address as uninitialised, and the core was
// answered with 0x0000 -- c.unimp -- at its reset vector.
//
//   verilator --binary --timing -Wno-fatal --top top \
//     verilator_constraint_scope.sv -o t --Mdir obj && ./obj/t
//
// Expected: all four print 80000084. Observed on 5.050:
//
//   local  handle named 'item':   ok=1 addr=5b6f55b9  want 80000084
//   member handle named 'item':   ok=1 addr=dc754b81  want 80000084
//   member handle named 'itm':    ok=1 addr=80000084  want 80000084
//   member handle named 'req':    ok=1 addr=80000084  want 80000084
//
// SPDX-License-Identifier: Apache-2.0

class item_t;
  rand bit [31:0] addr;
endclass

class probe;
  item_t item;
  item_t itm;
  item_t req;

  task local_named_item();
    item_t r = new();
    item_t item = new();
    bit ok;
    item.addr = 32'h8000_0084;
    ok = r.randomize() with { addr == item.addr; };
    $display("  local  handle named 'item':   ok=%0b addr=%08h  want 80000084", ok, r.addr);
  endtask

  task member_named_item();
    item_t r = new();
    bit ok;
    ok = r.randomize() with { addr == item.addr; };
    $display("  member handle named 'item':   ok=%0b addr=%08h  want 80000084", ok, r.addr);
  endtask

  task member_named_itm();
    item_t r = new();
    bit ok;
    ok = r.randomize() with { addr == itm.addr; };
    $display("  member handle named 'itm':    ok=%0b addr=%08h  want 80000084", ok, r.addr);
  endtask

  task member_named_req();
    item_t r = new();
    bit ok;
    ok = r.randomize() with { addr == req.addr; };
    $display("  member handle named 'req':    ok=%0b addr=%08h  want 80000084", ok, r.addr);
  endtask
endclass

module top;
  initial begin
    probe p = new();
    p.item = new(); p.item.addr = 32'h8000_0084;
    p.itm  = new(); p.itm.addr  = 32'h8000_0084;
    p.req  = new(); p.req.addr  = 32'h8000_0084;
    p.local_named_item();
    p.member_named_item();
    p.member_named_itm();
    p.member_named_req();
  end
endmodule
