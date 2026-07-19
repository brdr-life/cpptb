interface stream_if #(parameter int WIDTH = 8) (input logic clk);
  logic reset_n;
  logic valid;
  logic ready;
  logic [WIDTH-1:0] data;
  logic [WIDTH-1:0] observed;
  wire sideband;

  modport target(
      input clk, reset_n, valid, data,
      output ready, observed,
      inout sideband
  );
endinterface

module stream_interfaces (
    stream_if.target links [2],
    input  logic       gpio_drive,
    inout  wire  [3:0] gpio,
    output logic [3:0] gpio_seen
);
  for (genvar index = 0; index < 2; ++index) begin : lanes
    assign links[index].ready = links[index].reset_n;
    assign links[index].observed = links[index].data + index;
    assign links[index].sideband =
        links[index].valid ? links[index].data[0] : 1'bz;
  end

  assign gpio = gpio_drive ? 4'ha : 4'hz;
  assign gpio_seen = gpio;
endmodule
