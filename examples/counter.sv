module counter (
    input  logic       clk,
    input  logic       rst,
    input  logic       en,
    output logic [7:0] count
);
    always_ff @(posedge clk) begin
        if (rst) begin
            count <= 8'd0;
        end else if (en) begin
            count <= count + 8'd1;
        end
    end
endmodule
