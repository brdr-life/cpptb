// The legitimate LRM 7.12.1 use of "item": array method with-clause. Must
// keep working after the fix.
module top;
  initial begin : main
    automatic int q[$] = '{1, 4, 2, 7};
    automatic int r[$];
    r = q.find with (item > 3);
    $display("arraymethod: size=%0d first=%0d (want size=2 first=4)", r.size(), r[0]);
    $finish;
  end
endmodule
