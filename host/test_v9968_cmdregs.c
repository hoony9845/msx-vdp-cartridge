/*
 * geo3d's command registers written in one go (write_line_regs and
 * write_lrmm_regs, static in v9968_cmd.c, included here) against
 * v9968_cmd_write_reg register by register, from random VDP states: the
 * whole VDP state must match.
 *
 *   usage: test_v9968_cmdregs [count] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v9968_cmd.c"

static uint32_t rs = 7;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(int argc, char **argv) {
    unsigned count = argc > 1 ? (unsigned)atoi(argv[1]) : 500000;
    rs = argc > 2 ? (uint32_t)atoi(argv[2]) : 7;
    static uint8_t vram[V9968_VRAM_SIZE];
    static geo3d_t geo;
    static v9968_t a, b;
    v9968_init(&a, vram, &geo);
    for (unsigned n = 0; n < count; n++) {
        uint8_t *pa = (uint8_t *)&a.cmd;
        for (unsigned i = 0; i < sizeof a.cmd; i++) pa[i] = (uint8_t)rnd();
        a.cmd.state = ST_IDLE;
        for (unsigned i = 0; i < 64; i++) a.regs[i] = (uint8_t)rnd();
        a.regs[21] = rnd() & 1;                                  /* V58 or V9968 */
        b = a;
        uint8_t by[GEO3D_CMD_MAX];
        for (unsigned i = 0; i < GEO3D_CMD_MAX; i++) by[i] = (uint8_t)rnd();
        unsigned len = (rnd() & 1) ? 11 : GEO3D_CMD_MAX;
        if (len == 11) { by[10] = (uint8_t)(0x70 | (by[10] & 15)); write_line_regs(&b, by); }
        else           { by[18] = (uint8_t)(0x30 | (by[18] & 15)); write_lrmm_regs(&b, by); }
        for (unsigned k = 0; k < len; k++) v9968_cmd_write_reg(&a, geo3d_command_reg(len, k), by[k]);
        if (memcmp(&a, &b, sizeof a)) { printf("FAIL %u: %s\n", n, len == 11 ? "LINE" : "LRMM"); return 1; }
    }
    printf("test_v9968_cmdregs: %u register sets written in one go identical\n", count);
    return 0;
}
