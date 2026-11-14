/*
 * Runs a command-engine stimulus through core/v9968_cmd.c and writes the
 * final VRAM and a log, in the format of the RTL testbench used to verify
 * it against HRA!'s vdp_command.v:
 *
 *   W <reg> <hex>  write command register
 *   T <hex>        wait for TR (or the end), then write R#44
 *   S              wait for TR (or the end), log S#7, read it (LMCM)
 *   E              wait for the end, log S#7, BD and S#8/S#9
 *
 * Usage: v9968_cmd_replay <mode 0-9> <v256> <hs> <R#12> init.hex cmds.txt dump.hex log.txt
 *   mode: 0-6 GRAPHIC1-7, 7 TEXT1, 8 TEXT2, 9 MULTICOLOR
 */
#include <stdio.h>
#include <stdlib.h>
#include "v9968.h"

static uint8_t vram[V9968_VRAM_SIZE];
static v9968_t vdp;

static const uint8_t mode_r0[10] = { 0x00, 0x02, 0x04, 0x06, 0x08, 0x0A, 0x0E, 0x00, 0x04, 0x00 };
static const uint8_t mode_r1[10] = { 0, 0, 0, 0, 0, 0, 0, 0x10, 0x10, 0x08 };

/* Run until the command ends or waits for the CPU. */
static void run(void) {
    for (int i = 0; i < 10000000 && v9968_cmd_busy(&vdp); i++) {
        if (!v9968_cmd_step(&vdp, 1u << 16)) break;      /* waiting for the CPU */
    }
}

int main(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s mode v256 hs r12 init.hex cmds.txt dump.hex log.txt\n", argv[0]);
        return 2;
    }
    int mode = atoi(argv[1]);
    static geo3d_t geo;
    v9968_init(&vdp, vram, &geo);
    vdp.cmd.rtl_lmcm = getenv("V9968_V9938_LMCM") == NULL;
    vdp.regs[0] = mode_r0[mode];
    vdp.regs[1] = mode_r1[mode];
    vdp.regs[21] = atoi(argv[2]) ? 0 : 1;
    vdp.regs[20] = atoi(argv[3]) ? 1 : 0;
    vdp.regs[12] = (uint8_t)atoi(argv[4]);

    FILE *f = fopen(argv[5], "r");
    if (!f) { perror(argv[5]); return 1; }
    unsigned b;
    for (unsigned i = 0; i < V9968_VRAM_SIZE && fscanf(f, "%x", &b) == 1; i++) vram[i] = (uint8_t)b;
    fclose(f);

    FILE *in = fopen(argv[6], "r"), *log = fopen(argv[8], "w");
    if (!in || !log) { perror("open"); return 1; }
    setvbuf(log, NULL, _IOLBF, 0);
    char op[8];
    while (fscanf(in, "%7s", op) == 1) {
        unsigned reg, val;
        switch (op[0]) {
        case 'W':
            if (fscanf(in, "%u %x", &reg, &val) != 2) break;
            {
                /* the RTL keeps running while the next command's registers
                 * are written: a command still waiting for CPU data takes
                 * that R#44 as data at once */
                unsigned op = vdp.cmd.op;
                bool waiting = v9968_cmd_busy(&vdp) && vdp.cmd.transfer_ready &&
                               (op == 0xB || op == 0xF || op == 0x2);   /* LMMC HMMC LFMC */
                v9968_cmd_write_reg(&vdp, reg, (uint8_t)val);
                if (reg == 44 && waiting) run();
            }
            break;
        case 'T':
            if (fscanf(in, "%x", &val) != 1) break;
            run();
            if (v9968_cmd_busy(&vdp)) v9968_cmd_write_reg(&vdp, 44, (uint8_t)val);
            break;
        case 'S':
            run();
            fprintf(log, "S %02x\n", v9968_cmd_busy(&vdp) ? vdp.cmd.status_color : 0xFF);
            v9968_cmd_read_color(&vdp);
            break;
        case 'E':
            run();
            fprintf(log, "E %02x %d %03x\n", vdp.cmd.status_color, vdp.cmd.border_detect,
                    (unsigned)((vdp.cmd.sx >> 8) & 0x1FF));
            break;
        default:
            break;
        }
    }
    fclose(in);
    fclose(log);

    FILE *out = fopen(argv[7], "w");
    if (!out) { perror(argv[7]); return 1; }
    for (unsigned i = 0; i < V9968_VRAM_SIZE; i++) fprintf(out, "%02x\n", vram[i]);
    fclose(out);
    return 0;
}
