module multistage_jump_tb;
    reg clk = 0;
    reg reset = 0;
    mips uut(.clk(clk), .reset(reset));

    localparam BRANCH_PC = 32'h3010, SLOT_PC = 32'h3014, FALLTHROUGH = 32'h3018;
    localparam SLOT_INS = 32'h340a0001;  // ori $10, $0, 1

    task step;
        begin
            #1 clk = 1;
            #1 clk = 0;
        end
    endtask

    task run_case;
        input [8*32-1:0] label;
        input [31:0] branch, next_pc;
        input integer raw_cycles, control_cycles;
        input annul, writes_after_jump;
        integer i;
        begin
            reset = 1;
            step;
            reset = 0;
            for (i = 0; i < 24; i = i + 1)
                uut.instruction_memory.codes[i] = 0;
            uut.instruction_memory.codes[0] = 32'h34013030;  // ori $1, $0, 0x3030
            uut.instruction_memory.codes[1] = 32'h34020001;  // ori $2, $0, 1
            uut.instruction_memory.codes[2] = 32'hac010000;  // sw  $1, 0($0)
            if (raw_cycles != 0)
                uut.instruction_memory.codes[3] = 32'h8c030000;  // lw $3, 0($0)
            uut.instruction_memory.codes[4] = branch;
            uut.instruction_memory.codes[5] = SLOT_INS;
            uut.instruction_memory.codes[6] = 32'h340b0002;  // ori $11, $0, 2
            uut.instruction_memory.codes[7] = 32'h08000c10;  // j 0x3040

            repeat (5) step;
            if (uut.pc !== SLOT_PC || uut.D_PC !== BRANCH_PC || uut.D_IR !== branch)
                $fatal(1, "%0s: branch did not reach D with its slot in fetch", label);

            for (i = 0; i < raw_cycles; i = i + 1) begin
                if (uut.stall !== 1'b1 || uut.control_wait !== 1'b0)
                    $fatal(1, "%0s: expected a RAW stall", label);
                step;
                if (uut.pc !== SLOT_PC || uut.D_PC !== BRANCH_PC || uut.E_valid !== 1'b0)
                    $fatal(1, "%0s: RAW stall must hold D and bubble E", label);
            end
            for (i = 0; i < control_cycles; i = i + 1) begin
                if (uut.stall !== 1'b0 || uut.control_wait !== 1'b1)
                    $fatal(1, "%0s: expected control wait %0d", label, i);
                step;
                if (uut.pc !== SLOT_PC || uut.D_valid !== 1'b0)
                    $fatal(1, "%0s: control wait must hold the fetch PC and bubble D", label);
                if (i == 0 && (uut.E_valid !== 1'b1 || uut.E_IR !== branch))
                    $fatal(1, "%0s: branch did not advance to E", label);
                if (i == 1 && (uut.M_valid !== 1'b1 || uut.M_IR !== branch || uut.E_valid !== 1'b0))
                    $fatal(1, "%0s: expected branch in M with a bubble in E", label);
            end
            if (uut.stall !== 1'b0 || uut.control_wait !== 1'b0)
                $fatal(1, "%0s: unexpected extra wait at the resolution boundary", label);
            step;
            if (uut.pc !== next_pc || uut.D_valid !== !annul)
                $fatal(1, "%0s: unexpected PC %h or slot valid %b", label, uut.pc, uut.D_valid);
            if (!annul && (uut.D_PC !== SLOT_PC || uut.D_IR !== SLOT_INS))
                $fatal(1, "%0s: the wrong instruction entered the delay slot", label);

            if (next_pc != FALLTHROUGH) begin
                step;
                if (uut.pc !== next_pc + 32'd4)
                    $fatal(1, "%0s: a later stage redirected the selected target", label);
                step;
                if (uut.pc !== next_pc + 32'd8)
                    $fatal(1, "%0s: a later stage redirected the selected target", label);
            end
            repeat (6) step;
            if (uut.registers.r[10] !== (annul ? 32'd0 : 32'd1))
                $fatal(1, "%0s: wrong delay slot register value", label);
            if (uut.registers.r[11] !== (next_pc == FALLTHROUGH ? 32'd2 : 32'd0))
                $fatal(1, "%0s: wrong fallthrough register value", label);
            if (uut.registers.r[12] !== (writes_after_jump ? 32'd42 : 32'd0))
                $fatal(1, "%0s: wrong M-stage writeback", label);
        end
    endtask

    initial begin
        #1;
        run_case("D taken", 32'h10000007, 32'h3030, 0, 0, 0, 0);
        run_case("D not taken", 32'h10020007, FALLTHROUGH, 0, 0, 0, 0);
        run_case("E taken with RAW", {6'h3c, 5'd3, 5'd1, 16'd7}, 32'h3030, 1, 1, 0, 0);
        run_case("E not taken", {6'h3c, 5'd0, 5'd2, 16'd7}, FALLTHROUGH, 0, 1, 0, 0);
        run_case("M memory target", {6'h3d, 5'd0, 5'd1, 16'd0}, 32'h3030, 0, 2, 0, 0);
        run_case("M not taken", {6'h3d, 5'd0, 5'd2, 16'd0}, FALLTHROUGH, 0, 2, 0, 0);
        run_case("bdem D wins", {6'h3e, 5'd2, 5'd2, 5'd2, 11'd0}, 32'h3024, 0, 0, 0, 0);
        run_case("bdem E wins", {6'h3e, 5'd0, 5'd2, 5'd2, 11'd0}, 32'h3028, 0, 1, 0, 0);
        run_case("bdem M wins", {6'h3e, 5'd0, 5'd0, 5'd2, 11'd0}, 32'h302c, 0, 2, 0, 0);
        run_case("bdem all false", {6'h3e, 26'd0}, FALLTHROUGH, 0, 2, 0, 0);
        run_case("D clears, M jumps", {6'h3f, 5'd0, 5'd2, 16'd1}, 32'h302c, 0, 2, 1, 0);
        run_case("E clears, M jumps", {6'h3f, 5'd0, 5'd2, 16'd2}, 32'h302c, 0, 2, 1, 0);
        run_case("M clears, M jumps", {6'h3f, 5'd0, 5'd2, 16'd4}, 32'h302c, 0, 2, 1, 0);
        run_case("M clears without jump", {6'h3f, 5'd0, 5'd0, 16'd4}, FALLTHROUGH, 0, 2, 1, 0);
        run_case("D clears without jump", {6'h3f, 5'd0, 5'd0, 16'd1}, FALLTHROUGH, 0, 2, 1, 0);
        run_case("known false later jump", {6'h3a, 26'd0}, FALLTHROUGH, 0, 0, 0, 1);
        run_case("E wins with M write", {6'h3a, 26'd3}, 32'h3024, 0, 1, 0, 1);
        run_case("M direct memory target", {6'h3a, 26'd1}, 32'h3030, 0, 2, 0, 1);
        run_case("first jump in D wins", {6'h3b, 26'd3}, 32'h3024, 0, 0, 0, 0);
        $display("Multistage jump tests passed");
        $finish;
    end
endmodule
