/*
 * The command engine's fast paths (LRMM, HMMM, LMMM, LINE) against the general
 * per-pixel code: random commands in every bitmap mode, run in lockstep in
 * random budgets; the command
 * state and the clocks used must match after every call, and VRAM at the
 * end. Half the commands also stop early through v9968_cmd_watch (watching
 * a VRAM word the command writes); the general code then runs the same
 * number of clocks.
 *
 *   usage: test_v9968_cmdfast [commands] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v9968.h"

static uint8_t vram_a[V9968_VRAM_SIZE], vram_b[V9968_VRAM_SIZE];
static v9968_t va, vb;

static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void wr(unsigned reg, uint8_t d) {
    v9968_cmd_write_reg(&va, reg, d);
    v9968_cmd_write_reg(&vb, reg, d);
}

int main(int argc, char **argv) {
    unsigned count = argc > 1 ? (unsigned)atoi(argv[1]) : 3000;
    rng = argc > 2 ? (uint32_t)atoi(argv[2]) : 12345u;
    static const uint8_t mode_r0[4] = { 0x06, 0x08, 0x0A, 0x0E };   /* G4 G5 G6 G7 */
    unsigned long pixels = 0, yields = 0;

    static geo3d_t geo_a, geo_b;
    v9968_init(&va, vram_a, &geo_a);
    v9968_init(&vb, vram_b, &geo_b);
    for (unsigned i = 0; i < V9968_VRAM_SIZE; i++) vram_a[i] = (uint8_t)rnd();

    for (unsigned n = 0; n < count; n++) {
        memcpy(vram_b, vram_a, V9968_VRAM_SIZE);
        unsigned m = rnd() & 3;
        uint8_t r20 = (uint8_t)((rnd() & 1) | ((rnd() & 1) << 3));   /* HS, SP3 */
        va.regs[0] = vb.regs[0] = mode_r0[m];
        va.regs[1] = vb.regs[1] = 0;
        va.regs[20] = vb.regs[20] = r20;
        va.regs[21] = vb.regs[21] = 0;                               /* V9968 mode */

        for (unsigned r = 32; r <= 58; r++) if (r != 45 && r != 46) wr(r, (uint8_t)rnd());
        /* sizes that end in reasonable time; sometimes the 11-bit edges */
        unsigned nx = (rnd() & 7) == 0 ? (rnd() & 0x7FF) : (rnd() % 300);
        unsigned ny = (rnd() & 3) == 0 ? 0 : (rnd() % 40);   /* NY = 0: horizontal LINEs */
        wr(40, (uint8_t)nx); wr(41, (uint8_t)(nx >> 8));
        wr(42, (uint8_t)ny); wr(43, (uint8_t)(ny >> 8));
        if ((rnd() & 7) == 0) { wr(38, 0); wr(39, 0); }  /* DY at the edges */
        if ((rnd() & 7) == 0) {                          /* DX at the edges */
            static const uint16_t ex[4] = { 0, 255, 256, 511 };
            uint16_t e = ex[rnd() & 3];
            wr(36, (uint8_t)e); wr(37, (uint8_t)(e >> 8));
        }
        if (rnd() & 1) {                                 /* a plain window most times */
            wr(51, 0); wr(52, 0); wr(53, 0); wr(54, 0);
            wr(55, 0xFF); wr(56, 1); wr(57, 0xFF); wr(58, 7);
        }
        if (rnd() & 1) {                                 /* small vectors */
            unsigned vx = rnd() & 0x1FF, vy = rnd() & 0x1FF;
            if (rnd() & 1) vx = -vx;
            if (rnd() & 1) vy = -vy;
            wr(47, (uint8_t)vx); wr(48, (uint8_t)(vx >> 8));
            wr(49, (uint8_t)vy); wr(50, (uint8_t)(vy >> 8));
        }
        wr(45, (uint8_t)rnd());                          /* DIX/DIY/XHR/FG4 ... */
        static const uint8_t ops[4] = { 0x30, 0x90, 0xD0, 0x70 };    /* LRMM, LMMM, HMMM, LINE */
        uint8_t op = ops[rnd() % 4];
        wr(46, (uint8_t)(op | ((rnd() & 1) ? 0 : (rnd() & 0x0F))));     /* IMP half the time */

        /* the first destination bytes (layout as G4/G7 without interleave) */
        unsigned dxw = vb.cmd.reg_dx, dyw = vb.cmd.dy;
        uint32_t wa = (m <= 1 || (va.cmd.arg & 0x80)) ? ((dyw & 0x7FFu) << 7 | (dxw >> 1)) :
                                                        ((dyw & 0x3FFu) << 8 | (dxw >> (m == 2)));
        bool watching = rnd() & 1;
        v9968_cmd_watch = watching ? (const volatile uint32_t *)&vram_b[wa & (V9968_VRAM_MASK & ~3u)] : NULL;

        for (unsigned call = 0; v9968_cmd_busy(&va) || v9968_cmd_busy(&vb); call++) {
            unsigned budget = (rnd() & 3) == 0 ? (1u << 20) : 1 + (rnd() % 64);
            if (watching) budget = (rnd() & 1) ? (1u << 20) : 512;
            v9968_cmd_reference = false;
            unsigned ub = v9968_cmd_step(&vb, budget);
            if (ub < budget && v9968_cmd_busy(&vb)) yields++;
            /* the general code runs to the same clock count */
            const volatile uint32_t *w = v9968_cmd_watch;
            v9968_cmd_watch = NULL;
            v9968_cmd_reference = true;
            /* (a command that ends without a pixel, e.g. LINE from DX >= 256 in
             * a 256-pixel mode, uses 0 clocks: give the general code 1) */
            unsigned ua = v9968_cmd_step(&va, watching ? (ub ? ub : 1) : budget);
            v9968_cmd_reference = false;
            v9968_cmd_watch = w;
            if (ua != ub || memcmp(&va.cmd, &vb.cmd, sizeof(va.cmd)) != 0 ||
                va.int_flags != vb.int_flags) {
                printf("FAIL command %u (op %x mode G%u r20 %02x arg %02x) call %u budget %u: "
                       "clocks %u / %u\n", n, va.cmd.op, m + 4, r20, va.cmd.arg, call, budget, ua, ub);
                const uint8_t *pa = (const uint8_t *)&va.cmd, *pb = (const uint8_t *)&vb.cmd;
                for (unsigned o = 0; o < sizeof(va.cmd); o++)
                    if (pa[o] != pb[o]) printf("  cmd byte %u: %02x / %02x\n", o, pa[o], pb[o]);
                printf("  state %u/%u int %02x/%02x\n", va.cmd.state, vb.cmd.state, va.int_flags, vb.int_flags);
                return 1;
            }
            pixels += ua;
        }
        if (memcmp(vram_a, vram_b, V9968_VRAM_SIZE) != 0) {
            printf("FAIL command %u: VRAM differs (op %x mode G%u lop %x arg %02x color %02x dx %u dy %u nx %u ny %u)\n",
                   n, va.cmd.op, m + 4, va.cmd.lop, va.cmd.arg, va.cmd.color, dxw, dyw, nx, ny);
            for (unsigned i = 0, k = 0; i < V9968_VRAM_SIZE && k < 6; i++)
                if (vram_a[i] != vram_b[i]) { printf("  %05x: %02x / %02x\n", i, vram_a[i], vram_b[i]); k++; }
            return 1;
        }
    }
    v9968_cmd_watch = NULL;
    printf("test_v9968_cmdfast: %u commands identical (%lu clocks, %lu early stops)\n",
           count, pixels, yields);
    if (yields == 0) { printf("FAIL: v9968_cmd_watch never stopped a command\n"); return 1; }
    return 0;
}
