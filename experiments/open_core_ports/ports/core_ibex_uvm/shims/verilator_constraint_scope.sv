class item_t; rand bit [31:0] addr; endclass

class seq_t;
  item_t item;
  task broken();
    item_t r = new(); bit ok;
    ok = r.randomize() with { addr == item.addr; };
    $display("  member handle direct:   ok=%0b addr=%08h  want %08h", ok, r.addr, item.addr);
  endtask
  task workaround_handle();
    item_t r = new(); item_t l = item; bit ok;
    ok = r.randomize() with { addr == l.addr; };
    $display("  copied to local handle: ok=%0b addr=%08h  want %08h", ok, r.addr, item.addr);
  endtask
  task workaround_value();
    item_t r = new(); bit [31:0] a; bit ok;
    a = item.addr;
    ok = r.randomize() with { addr == a; };
    $display("  copied to local value:  ok=%0b addr=%08h  want %08h", ok, r.addr, item.addr);
  endtask
endclass

module top;
  initial begin
    seq_t s = new();
    s.item = new();
    s.item.addr = 32'h8000_0084;
    s.broken();
    s.workaround_handle();
    s.workaround_value();
  end
endmodule
