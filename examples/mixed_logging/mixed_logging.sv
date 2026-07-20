`ifdef CPPTB_ENABLE_SV_LOGGING
`include "cpptb/sv/cpptb_log.svh"
`endif

module mixed_logging (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        valid,
    input  logic [31:0] data,
    output logic [31:0] accepted_count
);
`ifdef CPPTB_ENABLE_SV_LOGGING
  int unsigned cpptb_log_message_factories = 0;

  function automatic string accepted_message(input logic [31:0] accepted_data);
    ++cpptb_log_message_factories;
    return $sformatf("accepted data=0x%08x", accepted_data);
  endfunction

  final begin
    $display("CPPTB_MIXED_LOGGING_SV_MESSAGE_FACTORIES=%0d",
             cpptb_log_message_factories);
  end
`endif

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      accepted_count <= '0;
    end else if (valid) begin
      accepted_count <= accepted_count + 1'b1;
`ifdef CPPTB_ENABLE_SV_LOGGING
      `cpptb_info(accepted_message(data), "request_monitor")
`endif
    end
  end
endmodule
