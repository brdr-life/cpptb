`ifndef OPEN_CORE_WORKLOAD
`define OPEN_CORE_WORKLOAD 0
`endif

module open_cores_benchmark_dut #(
  parameter int WORKLOAD = `OPEN_CORE_WORKLOAD
) (
  input  logic        clk,
  input  logic        rst_n,

  input  logic        cpu_prog_we,
  input  logic [31:0] cpu_prog_addr,
  input  logic [31:0] cpu_prog_data,
  output logic        cpu_done,
  output logic        cpu_trap,
  output logic [31:0] cpu_result,
  output logic [31:0] cpu_instruction_count,

  input  logic        aes_cs,
  input  logic        aes_we,
  input  logic [7:0]  aes_address,
  input  logic [31:0] aes_write_data,
  output logic [31:0] aes_read_data,

  input  bit   [63:0] fcs_tdata,
  input  logic [7:0]  fcs_tkeep,
  input  logic        fcs_tvalid,
  output logic        fcs_tready,
  input  logic        fcs_tlast,
  output logic [31:0] fcs_result,
  output logic        fcs_valid
);
  if (WORKLOAD == 0) begin : g_picorv32
    localparam logic [31:0] RESULT_ADDRESS = 32'h1000_0000;

    logic        mem_valid;
    logic        mem_instr;
    logic        mem_ready;
    logic [31:0] mem_addr;
    logic [31:0] mem_wdata;
    logic [3:0]  mem_wstrb;
    logic [31:0] mem_rdata;
    logic [31:0] memory [0:255];

    logic        unused_mem_la_read;
    logic        unused_mem_la_write;
    logic [31:0] unused_mem_la_addr;
    logic [31:0] unused_mem_la_wdata;
    logic [3:0]  unused_mem_la_wstrb;
    logic        unused_pcpi_valid;
    logic [31:0] unused_pcpi_insn;
    logic [31:0] unused_pcpi_rs1;
    logic [31:0] unused_pcpi_rs2;
    logic [31:0] unused_eoi;
    logic        unused_trace_valid;
    logic [35:0] unused_trace_data;

    assign aes_read_data = '0;
    assign fcs_tready = 1'b0;
    assign fcs_result = '0;
    assign fcs_valid = 1'b0;

    picorv32 #(
      .ENABLE_COUNTERS(0),
      .ENABLE_COUNTERS64(0),
      .ENABLE_IRQ(0),
      .CATCH_MISALIGN(1),
      .CATCH_ILLINSN(1)
    ) core (
      .clk(clk),
      .resetn(rst_n),
      .trap(cpu_trap),
      .mem_valid(mem_valid),
      .mem_instr(mem_instr),
      .mem_ready(mem_ready),
      .mem_addr(mem_addr),
      .mem_wdata(mem_wdata),
      .mem_wstrb(mem_wstrb),
      .mem_rdata(mem_rdata),
      .mem_la_read(unused_mem_la_read),
      .mem_la_write(unused_mem_la_write),
      .mem_la_addr(unused_mem_la_addr),
      .mem_la_wdata(unused_mem_la_wdata),
      .mem_la_wstrb(unused_mem_la_wstrb),
      .pcpi_valid(unused_pcpi_valid),
      .pcpi_insn(unused_pcpi_insn),
      .pcpi_rs1(unused_pcpi_rs1),
      .pcpi_rs2(unused_pcpi_rs2),
      .pcpi_wr(1'b0),
      .pcpi_rd(32'b0),
      .pcpi_wait(1'b0),
      .pcpi_ready(1'b0),
      .irq(32'b0),
      .eoi(unused_eoi),
      .trace_valid(unused_trace_valid),
      .trace_data(unused_trace_data)
    );

    always_ff @(posedge clk) begin
      if (!rst_n) begin
        mem_ready <= 1'b0;
        mem_rdata <= '0;
        cpu_done <= 1'b0;
        cpu_result <= '0;
        cpu_instruction_count <= '0;
      end else begin
        mem_ready <= 1'b0;
        if (mem_valid && !mem_ready) begin
          mem_ready <= 1'b1;
          if (mem_addr[31:10] == 0)
            mem_rdata <= memory[mem_addr[9:2]];
          else
            mem_rdata <= '0;

          if ((mem_addr == RESULT_ADDRESS) && (mem_wstrb != 0)) begin
            cpu_result <= mem_wdata;
            cpu_done <= 1'b1;
          end
        end
        if (mem_valid && mem_ready && mem_instr)
          cpu_instruction_count <= cpu_instruction_count + 1'b1;
      end

      if (cpu_prog_we)
        memory[cpu_prog_addr[9:2]] <= cpu_prog_data;
    end
  end else if (WORKLOAD == 1) begin : g_aes
    assign cpu_done = 1'b0;
    assign cpu_trap = 1'b0;
    assign cpu_result = '0;
    assign cpu_instruction_count = '0;
    assign fcs_tready = 1'b0;
    assign fcs_result = '0;
    assign fcs_valid = 1'b0;

    aes core (
      .clk(clk),
      .reset_n(rst_n),
      .cs(aes_cs),
      .we(aes_we),
      .address(aes_address),
      .write_data(aes_write_data),
      .read_data(aes_read_data)
    );
  end else begin : g_fcs
    assign cpu_done = 1'b0;
    assign cpu_trap = 1'b0;
    assign cpu_result = '0;
    assign cpu_instruction_count = '0;
    assign aes_read_data = '0;

    axis_eth_fcs #(
      .DATA_WIDTH(64),
      .KEEP_ENABLE(1),
      .KEEP_WIDTH(8)
    ) core (
      .clk(clk),
      .rst(!rst_n),
      .s_axis_tdata(fcs_tdata),
      .s_axis_tkeep(fcs_tkeep),
      .s_axis_tvalid(fcs_tvalid),
      .s_axis_tready(fcs_tready),
      .s_axis_tlast(fcs_tlast),
      .s_axis_tuser(1'b0),
      .output_fcs(fcs_result),
      .output_fcs_valid(fcs_valid)
    );
  end
endmodule
