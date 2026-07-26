class item_t; rand bit [31:0] addr; endclass

class seq_t;
  item_t item;
  task unqualified();
    item_t r = new(); bit ok;
    ok = r.randomize() with { addr == item.addr; };
    $display("  unqualified item.addr:  ok=%0b addr=%08h  want %08h", ok, r.addr, item.addr);
  endtask
  task with_local_scope();
    item_t r = new(); bit ok;
    ok = r.randomize() with { addr == local::item.addr; };
    $display("  local::item.addr:       ok=%0b addr=%08h  want %08h", ok, r.addr, item.addr);
  endtask
endclass

module top;
  initial begin
    seq_t s = new();
    s.item = new();
    s.item.addr = 32'h8000_0084;
    s.unqualified();
    s.with_local_scope();
  end
endmodule
