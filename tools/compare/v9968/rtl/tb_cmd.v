`timescale 1ns/1ps
// Golden reference for core/v9968_cmd.c: HRA!'s vdp_command.v + its shared
// cache, any screen mode. Stimulus (+cmds=):
//   W <reg> <hex>  write command register
//   T <hex>        wait for TR = 1 (or CE = 0), then write R#44 (LMMC/HMMC/LFMC)
//   S              wait for TR = 1 (or CE = 0), log S#7, pulse read_color (LMCM)
//   E              wait for CE = 0, log S#7, BD, S#8/9
// plusargs: mode=<0..9> (g1..g7,t1,t2,gm) il=<0|1> v256 hs tbc=<R#12>
module tb_cmd;
    reg clk = 0;
    reg reset_n = 0;
    always #5.82 clk = ~clk;

    wire [17:0] a;
    wire        v, w;
    wire [31:0] wd;
    wire [3:0]  wm;
    reg  [31:0] rd;
    reg         rde;
    reg         rw = 0;
    reg  [5:0]  rn = 0;
    reg  [7:0]  rdat = 0;
    reg         rcol = 0;
    wire        ce, bd, tr;
    wire [7:0]  scol;
    wire [8:0]  bpos;
    reg         hs, v256, il;
    integer     mode, tbc;
    reg  [9:0]  mode1h;

    wire [17:0] ca;
    wire        cv, cr, cw, crde, cfs, cfe;
    wire [7:0]  cwd, crd;

    vdp_command dut (
        .reset_n(reset_n), .clk(clk),
        .cache_vram_address(ca), .cache_vram_valid(cv), .cache_vram_ready(cr),
        .cache_vram_write(cw), .cache_vram_wdata(cwd), .cache_vram_rdata(crd),
        .cache_vram_rdata_en(crde), .cache_flush_start(cfs), .cache_flush_end(cfe),
        .register_write(rw), .register_num(rn), .register_data(rdat),
        .clear_border_detect(1'b0), .read_color(rcol),
        .status_command_execute(ce), .status_border_detect(bd), .status_transfer_ready(tr),
        .status_color(scol), .status_border_position(bpos),
        .screen_mode(mode1h),
        .vram_interleave(il), .reg_text_back_color(tbc[7:0]),
        .reg_command_enable(1'b1), .reg_command_high_speed_mode(hs),
        .reg_ext_command_mode(v256), .reg_vram256k_mode(v256),
        .vram_access_mask(), .intr_command_end()
    );

    vdp_command_cache u_cache (
        .reset_n(reset_n), .clk(clk), .start(1'b0),
        .cache_vram_address(ca), .cache_vram_valid(cv), .cache_vram_ready(cr),
        .cache_vram_write(cw), .cache_vram_wdata(cwd), .cache_vram_rdata(crd),
        .cache_vram_rdata_en(crde), .cache_flush_start(cfs), .cache_flush_end(cfe),
        .cpu_vram_address(18'd0), .cpu_vram_valid(1'b0), .cpu_vram_ready(),
        .cpu_vram_write(1'b0), .cpu_vram_wdata(8'd0), .cpu_vram_rdata(), .cpu_vram_rdata_en(),
        .command_vram_address(a), .command_vram_valid(v), .command_vram_ready(1'b1),
        .command_vram_write(w), .command_vram_wdata(wd), .command_vram_wdata_mask(wm),
        .command_vram_rdata(rd), .command_vram_rdata_en(rde)
    );

    reg [31:0] mem [0:65535];
    integer i;
    always @(posedge clk) begin
        rde <= 1'b0;
        if (v) begin
            if (w) begin
                if (!wm[0]) mem[a[17:2]][ 7: 0] <= wd[ 7: 0];
                if (!wm[1]) mem[a[17:2]][15: 8] <= wd[15: 8];
                if (!wm[2]) mem[a[17:2]][23:16] <= wd[23:16];
                if (!wm[3]) mem[a[17:2]][31:24] <= wd[31:24];
            end else begin
                rd  <= mem[a[17:2]];
                rde <= 1'b1;
            end
        end
    end

    reg [8*200-1:0] fcmd, fdump, finit, flog;
    integer fi, fo, fl, r, reg_n, val, cyc;
    reg [8*4-1:0] op;
    reg [7:0] bytes [0:262143];

    task wreg(input [5:0] n, input [7:0] d);
        begin
            @(negedge clk); rw = 1; rn = n; rdat = d;
            @(negedge clk); rw = 0;
        end
    endtask

    task wait_tr;
        begin
            cyc = 0;
            @(negedge clk);
            while (!tr && ce && cyc < 2000000) begin @(negedge clk); cyc = cyc + 1; end
        end
    endtask

    initial begin
        if (!$value$plusargs("cmds=%s", fcmd))  fcmd  = "cmds.txt";
        if (!$value$plusargs("init=%s", finit)) finit = "init.hex";
        if (!$value$plusargs("dump=%s", fdump)) fdump = "rtl_dump.hex";
        if (!$value$plusargs("log=%s",  flog))  flog  = "rtl_log.txt";
        if (!$value$plusargs("hs=%d",   hs))    hs    = 1'b1;
        if (!$value$plusargs("v256=%d", v256))  v256  = 1'b1;
        if (!$value$plusargs("il=%d",   il))    il    = 1'b0;
        if (!$value$plusargs("mode=%d", mode))  mode  = 3;
        if (!$value$plusargs("tbc=%d",  tbc))   tbc   = 0;
        mode1h = 10'd1 << mode;
        for (i = 0; i < 65536; i = i + 1) mem[i] = 32'd0;
        $readmemh(finit, bytes);
        for (i = 0; i < 262144; i = i + 1)
            if (bytes[i] !== 8'hxx) mem[i >> 2][8 * (i & 3) +: 8] = bytes[i];
        repeat (4) @(negedge clk);
        reset_n = 1;
        repeat (4) @(negedge clk);

        fi = $fopen(fcmd, "r");
        fl = $fopen(flog, "w");
        while (!$feof(fi)) begin
            r = $fscanf(fi, "%s", op);
            if (r == 1) begin
                if (op[7:0] == "W") begin
                    r = $fscanf(fi, "%d %h", reg_n, val);
                    wreg(reg_n, val);
                end else if (op[7:0] == "T") begin
                    r = $fscanf(fi, "%h", val);
                    wait_tr;
                    if (ce) wreg(44, val);
                end else if (op[7:0] == "S") begin
                    wait_tr;
                    $fwrite(fl, "S %02x\n", ce ? scol : 8'hFF);
                    @(negedge clk); rcol = 1;
                    @(negedge clk); rcol = 0;
                end else if (op[7:0] == "E") begin
                    cyc = 0;
                    @(negedge clk);
                    while (!ce && cyc < 8) begin @(negedge clk); cyc = cyc + 1; end
                    while (ce && cyc < 20000000) begin @(negedge clk); cyc = cyc + 1; end
                    $fwrite(fl, "E %02x %0d %03x\n", scol, bd, bpos);
                end
            end
        end
        $fclose(fl);
        fo = $fopen(fdump, "w");
        for (i = 0; i < 262144; i = i + 1)
            $fwrite(fo, "%02x\n", mem[i >> 2][8 * (i & 3) +: 8]);
        $fclose(fo);
        $finish;
    end
endmodule
