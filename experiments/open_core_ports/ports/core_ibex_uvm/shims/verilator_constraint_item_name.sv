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
