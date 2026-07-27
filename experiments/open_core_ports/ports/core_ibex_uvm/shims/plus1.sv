class payload_t; rand bit [31:0] addr; endclass
class seq_t;
  payload_t item;
  task run();
    payload_t r1 = new();
    bit ok;
    ok = r1.randomize() with { addr == item.addr + 1; };
    $display("plus1: ok=%0d addr=%08h item.addr=%08h (want ok=1, addr=80000085)",
             ok, r1.addr, item.addr);
  endtask
endclass
module top;
  initial begin : main
    automatic seq_t s = new();
    s.item = new();
    s.item.addr = 32'h8000_0084;
    s.run();
    $finish;
  end
endmodule
