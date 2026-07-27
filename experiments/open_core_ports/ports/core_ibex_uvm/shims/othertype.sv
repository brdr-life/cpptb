class other_t;   bit [31:0] tag;  endclass
class payload_t; rand bit [31:0] addr; endclass
class seq_t;
  other_t item;
  task run();
    payload_t r1 = new();
    bit ok;
    ok = r1.randomize() with { addr == item.tag; };
    $display("othertype: ok=%0d addr=%08h item.tag=%08h (want 5a5a5a5a)",
             ok, r1.addr, item.tag);
  endtask
endclass
module top;
  initial begin : main
    automatic seq_t s = new();
    s.item = new();
    s.item.tag = 32'h5a5a_5a5a;
    s.run();
    $finish;
  end
endmodule
