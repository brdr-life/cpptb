// Two register stages, for pinning the deferred-write contract: a value
// written after a rising edge must appear in q ONE edge later, never on the
// edge just awaited.
module pipeline (
  input  logic       clk,
  input  logic [7:0] d,
  output logic [7:0] q,
  output logic [7:0] q2
);
  always_ff @(posedge clk) begin
    q  <= d;
    q2 <= q;
  end
endmodule
