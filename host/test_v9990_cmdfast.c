/*
 * The V9990 command engine's fast paths (bitmap LMMV, LMMM) against the
 * per-dot code (v9990_cmd_reference): random commands in every bitmap
 * depth and image width, run in lockstep with random budgets; the command
 * state, the status/interrupt flags and VRAM must match after every call.
 *
 *   usage: test_v9990_cmdfast [commands] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v9990.h"

static uint8_t vram_a[V9990_VRAM_SIZE], vram_b[V9990_VRAM_SIZE];
static v9990_t va, vb;
static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void reg(unsigned r, uint8_t d) {
    v9990_write_port(&va, 4, (uint8_t)r); v9990_write_port(&va, 3, d);
    v9990_write_port(&vb, 4, (uint8_t)r); v9990_write_port(&vb, 3, d);
}

static int same(void) {
    return !memcmp(&va.cmd, &vb.cmd, sizeof va.cmd) && va.status == vb.status &&
           va.int_flags == vb.int_flags && !memcmp(vram_a, vram_b, V9990_VRAM_SIZE);
}

int main(int argc, char **argv) {
    unsigned count = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    rng = argc > 2 ? (uint32_t)atoi(argv[2]) : 12345u;
    v9990_init(&va, vram_a);
    v9990_init(&vb, vram_b);
    for (unsigned i = 0; i < V9990_VRAM_SIZE; i++) vram_a[i] = vram_b[i] = (uint8_t)rnd();
    unsigned long pixels = 0;
    for (unsigned n = 0; n < count; n++) {
        /* bitmap, random DCKM / image width / depth */
        reg(6, (uint8_t)(0x80 | (rnd() & 0x3F)));
        reg(7, (uint8_t)(rnd() & 0x41));
        static const uint8_t lops[8] = { 0x0C, 0x0C, 0x1C, 0x03, 0x06, 0x0E, 0x18, 0x1A };
        unsigned nx = (rnd() & 3) ? 1 + rnd() % 70 : rnd() % 2048;
        unsigned ny = (rnd() & 3) ? 1 + rnd() % 20 : rnd() % 64;
        if ((uint64_t)(nx ? nx : 2048) * (ny ? ny : 4096) > 300000) { nx = 1 + rnd() % 300; ny = 1 + rnd() % 30; }
        unsigned p[20] = { rnd() & 255, rnd() & 7, rnd() & 255, rnd() & 15, rnd() & 255, rnd() & 7,
                           rnd() & 255, rnd() & 15, nx & 255, (nx >> 8) & 7, ny & 255, (ny >> 8) & 15,
                           rnd() & 0x0C, lops[rnd() & 7], (rnd() & 3) ? 0xFF : rnd() & 255,
                           (rnd() & 3) ? 0xFF : rnd() & 255, rnd() & 255, rnd() & 255, rnd() & 255, rnd() & 255 };
        if (rnd() & 1) p[16] = p[17] = 0;               /* FC 0 with TP */
        v9990_write_port(&va, 4, 32); v9990_write_port(&vb, 4, 32);
        for (unsigned i = 0; i < 20; i++) { v9990_write_port(&va, 3, (uint8_t)p[i]); v9990_write_port(&vb, 3, (uint8_t)p[i]); }
        uint8_t op = (rnd() & 1) ? (V9990_CMD_LMMV << 4) : (V9990_CMD_LMMM << 4);
        reg(52, op);
        for (unsigned guard = 0; guard < 2000000; guard++) {
            unsigned budget = (rnd() & 1) ? 1 + rnd() % 8 : 1 + rnd() % 3000;
            v9990_cmd_reference = true;
            bool ba = v9990_cmd_step(&va, budget);
            v9990_cmd_reference = false;
            bool bb = v9990_cmd_step(&vb, budget);
            if (ba != bb || !same()) {
                printf("FAIL command %u (op %X R#6 %02X lop %02X wm %02X%02X nx %u ny %u budget %u)\n",
                       n, op >> 4, va.regs[6], p[13], p[15], p[14], nx, ny, budget);
                return 1;
            }
            if (!ba) break;
        }
        pixels += (unsigned long)(nx ? nx : 2048) * (ny ? ny : 4096);
    }
    printf("test_v9990_cmdfast: %u LMMV/LMMM identical (%lu dots)\n", count, pixels);
    return 0;
}
