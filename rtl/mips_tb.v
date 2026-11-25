module mips_tb;
reg clk, reset;
mips uut (
    .clk(clk),
    .reset(reset)
);

initial begin
    clk = 0;
    reset = 1;
    #5 reset = 0;
end

initial begin
    $dumpfile("wave.vcd");
    $dumpvars(0, uut);
    #80;
    $finish;
end

always #1 clk = ~clk;
endmodule