/* Unit tests for the portable V9968 core and geo3d. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msx_io9968.h"

static int failures;

#define CHECK_EQ(actual, expected) do {                                      \
    long a_ = (long)(actual), e_ = (long)(expected);                          \
    if (a_ != e_) {                                                           \
        printf("  FAIL %s:%d: %s == %ld, expected %ld\n",                     \
               __FILE__, __LINE__, #actual, a_, e_);                          \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t vram[V9968_VRAM_SIZE];
static v9968_t vdp;

static void setup(void) {
    v9968_init(&vdp, vram);
    vdp.gamma = false;
}

static uint8_t g4_pixel(unsigned x, unsigned y) {
    uint8_t b = vram[(y * 128 + x / 2) & V9968_VRAM_MASK];
    return (x & 1) ? (b & 15) : (b >> 4);
}

static uint8_t g7_byte(unsigned x, unsigned y) {
    return vram[v9968_phys(true, y * 256 + x)];
}

static void test_vram_access(void) {
    setup();
    screen5(&vdp);
    vwrite_addr(&vdp, 0x12345);
    vout(&vdp, 0x88, 0xAA);
    vout(&vdp, 0x88, 0xBB);
    CHECK_EQ(vram[0x12345], 0xAA);
    CHECK_EQ(vram[0x12346], 0xBB);
    vread_addr(&vdp, 0x12345);
    CHECK_EQ(vin(&vdp, 0x88), 0xAA);
    CHECK_EQ(vin(&vdp, 0x88), 0xBB);

    /* V9958 mode: A17 does not exist */
    vwrite_addr(&vdp, 0x3FFFF);
    vout(&vdp, 0x88, 0x11);
    CHECK_EQ(vram[0x1FFFF], 0x11);

    /* V9968 mode: 256KB */
    v9968_native(&vdp, 0x00);
    vwrite_addr(&vdp, 0x3FFFF);
    vout(&vdp, 0x88, 0x22);
    vout(&vdp, 0x88, 0x33);
    CHECK_EQ(vram[0x3FFFF], 0x22);
    CHECK_EQ(vram[0x00000], 0x33);

    /* TMS9918 modes: the counter wraps inside 16KB */
    vreg(&vdp, 0, 0x00);
    vwrite_addr(&vdp, 0x07FFF);
    vout(&vdp, 0x88, 0x44);
    vout(&vdp, 0x88, 0x55);
    CHECK_EQ(vram[0x07FFF], 0x44);
    CHECK_EQ(vram[0x04000], 0x55);
}

static void test_registers(void) {
    setup();
    vreg(&vdp, 7, 0x34);
    CHECK_EQ(vdp.regs[7], 0x34);

    /* indirect, auto increment */
    vreg(&vdp, 17, 12);
    vout(&vdp, 0x8B, 0x56);
    vout(&vdp, 0x8B, 0x78);
    CHECK_EQ(vdp.regs[12], 0x56);
    CHECK_EQ(vdp.regs[13], 0x78);

    /* indirect, no increment; R#17 itself cannot be reached */
    vreg(&vdp, 17, 0x80 | 17);
    vout(&vdp, 0x8B, 0x05);
    CHECK_EQ(vdp.reg_ptr, 17);
    vreg(&vdp, 17, 0x80 | 7);
    vout(&vdp, 0x8B, 0x01);
    vout(&vdp, 0x8B, 0x02);
    CHECK_EQ(vdp.regs[7], 0x02);
    CHECK_EQ(vdp.regs[8], 0x08);                   /* reset value */

    /* a read between the two bytes of a pair restarts it */
    vout(&vdp, 0x89, 0x99);
    (void)vin(&vdp, 0x89);
    vout(&vdp, 0x89, 0x0F);
    vout(&vdp, 0x89, 0x87);
    CHECK_EQ(vdp.regs[7], 0x0F);

    /* A17 table bits only in the V9968 mode */
    vreg(&vdp, 2, 0xFF);
    CHECK_EQ(vdp.regs[2], 0x7F);

    /* R#20/R#21 are locked until port 4 bit7 is cleared */
    vreg(&vdp, 20, 0x10);
    CHECK_EQ(vdp.regs[20], 0x00);
    CHECK_EQ(vin(&vdp, 0x8C) & 0x80, 0x80);
    CHECK_EQ(vstatus(&vdp, 1) & 0x3E, 0x04);       /* V9958 ID */
    v9968_native(&vdp, 0x10);
    CHECK_EQ(vdp.regs[20], 0x10);
    CHECK_EQ(vstatus(&vdp, 1) & 0x3E, 0x06);       /* V9968 ID */
    vreg(&vdp, 2, 0xFF);
    CHECK_EQ(vdp.regs[2], 0xFF);
    vreg(&vdp, 21, 0x01);                          /* back to V9958: A17 cleared */
    CHECK_EQ(vdp.regs[2], 0x7F);
}

static void test_palette(void) {
    setup();
    vreg(&vdp, 16, 3);
    vout(&vdp, 0x8A, 0x70);         /* R=7, B=0 */
    vout(&vdp, 0x8A, 0x05);         /* G=5 */
    vout(&vdp, 0x8A, 0x07);         /* entry 4: B=7 */
    vout(&vdp, 0x8A, 0x00);
    CHECK_EQ(vdp.palette[3], (31 << 10) | (22 << 5) | 0);
    CHECK_EQ(vdp.palette[4], 31);

    v9968_native(&vdp, 0x10);       /* EPAL */
    vreg(&vdp, 16, 200);
    vout(&vdp, 0x8A, 1);
    vout(&vdp, 0x8A, 2);
    vout(&vdp, 0x8A, 3);
    vout(&vdp, 0x8A, 31);
    CHECK_EQ(vdp.palette[200], (1 << 10) | (2 << 5) | 3);
    CHECK_EQ(vdp.pal_index, 201);
    CHECK_EQ(vdp.pal_phase, 1);
}

static void test_interrupts(void) {
    setup();
    screen5(&vdp);
    vreg(&vdp, 0, 0x16);            /* IE1 */
    vreg(&vdp, 1, 0x60);            /* IE0 */
    vreg(&vdp, 19, 100);
    v9968_video_vblank_end(&vdp);
    for (unsigned y = 0; y <= 101; y++) v9968_video_line(&vdp, y);
    CHECK_EQ(vdp.int_flags & V9968_INT_LINE, V9968_INT_LINE);
    CHECK_EQ(v9968_irq_pending(&vdp), 1);
    CHECK_EQ(vstatus(&vdp, 1) & 1, 1);
    CHECK_EQ(vdp.int_flags & V9968_INT_LINE, 0);
    for (unsigned y = 102; y < 212; y++) v9968_video_line(&vdp, y);
    CHECK_EQ(vdp.int_flags & V9968_INT_FRAME, V9968_INT_FRAME);
    vout(&vdp, 0x8C, 0x81);         /* clear F through port 4, stay locked */
    CHECK_EQ(vdp.int_flags & V9968_INT_FRAME, 0);
    CHECK_EQ(v9968_irq_pending(&vdp), 0);
}

/* ---- commands ---- */

static void cmd(v9968_t *v, unsigned sx, unsigned sy, unsigned dx, unsigned dy,
                unsigned nx, unsigned ny, uint8_t clr, uint8_t arg, uint8_t op) {
    vreg(v, 17, 32);
    uint8_t b[15] = { (uint8_t)sx, (uint8_t)(sx >> 8), (uint8_t)sy, (uint8_t)(sy >> 8),
                      (uint8_t)dx, (uint8_t)(dx >> 8), (uint8_t)dy, (uint8_t)(dy >> 8),
                      (uint8_t)nx, (uint8_t)(nx >> 8), (uint8_t)ny, (uint8_t)(ny >> 8),
                      clr, arg, op };
    for (int i = 0; i < 15; i++) vout(v, 0x8B, b[i]);
}

static void test_hmmv_lmmv(void) {
    setup();
    screen5(&vdp);
    cmd(&vdp, 0, 0, 10, 20, 8, 3, 0x5A, 0, 0xC0);          /* HMMV */
    run9968(&vdp);
    CHECK_EQ(v9968_cmd_busy(&vdp), 0);
    CHECK_EQ(vram[20 * 128 + 5], 0x5A);
    CHECK_EQ(vram[22 * 128 + 8], 0x5A);
    CHECK_EQ(vram[22 * 128 + 9], 0x00);
    CHECK_EQ(vram[23 * 128 + 5], 0x00);
    CHECK_EQ(vdp.int_flags & V9968_INT_CMD_END, V9968_INT_CMD_END);

    cmd(&vdp, 0, 0, 11, 20, 2, 1, 0x03, 0, 0x82);           /* LMMV OR */
    run9968(&vdp);
    CHECK_EQ(g4_pixel(10, 20), 5);
    CHECK_EQ(g4_pixel(11, 20), 0xA | 3);
    CHECK_EQ(g4_pixel(12, 20), 5 | 3);
    CHECK_EQ(g4_pixel(13, 20), 0xA);
    CHECK_EQ(vdp.cmd.dy, 21);                               /* DY advances */

    /* TIMP with a source of 0 leaves the destination */
    cmd(&vdp, 0, 0, 10, 20, 4, 1, 0x00, 0, 0x88);
    run9968(&vdp);
    CHECK_EQ(g4_pixel(10, 20), 5);
}

static void test_line_pset_point(void) {
    setup();
    screen5(&vdp);
    /* LINE (10,10) -> (14,12): NX = 4 (major X), NY = 2 */
    cmd(&vdp, 0, 0, 10, 10, 4, 2, 0x07, 0x00, 0x70);
    run9968(&vdp);
    CHECK_EQ(g4_pixel(10, 10), 7);
    CHECK_EQ(g4_pixel(14, 12), 7);
    int n = 0;
    for (unsigned y = 9; y < 14; y++)
        for (unsigned x = 9; x < 16; x++) n += g4_pixel(x, y) == 7;
    CHECK_EQ(n, 5);

    cmd(&vdp, 0, 0, 100, 50, 0, 0, 0x09, 0, 0x50);           /* PSET */
    run9968(&vdp);
    CHECK_EQ(g4_pixel(100, 50), 9);
    cmd(&vdp, 100, 50, 0, 0, 0, 0, 0, 0, 0x40);              /* POINT */
    run9968(&vdp);
    CHECK_EQ(vstatus(&vdp, 7), 9);
}

static void test_srch(void) {
    setup();
    screen5(&vdp);
    cmd(&vdp, 0, 0, 30, 40, 1, 1, 0x0C, 0, 0x50);            /* PSET (30,40) */
    run9968(&vdp);
    cmd(&vdp, 10, 40, 0, 0, 0, 0, 0x0C, 0x00, 0x60);         /* SRCH right for 12 */
    run9968(&vdp);
    CHECK_EQ(vstatus(&vdp, 2) & V9968_S2_BD, V9968_S2_BD);
    CHECK_EQ(vstatus(&vdp, 8), 30);
    CHECK_EQ(vstatus(&vdp, 9) & 1, 0);
    CHECK_EQ(vstatus(&vdp, 2) & V9968_S2_BD, 0);             /* S#9 read clears */
    cmd(&vdp, 20, 40, 0, 0, 0, 0, 0x0D, 0x00, 0x60);         /* not found */
    run9968(&vdp);
    CHECK_EQ(vstatus(&vdp, 2) & V9968_S2_BD, 0);
}

static void test_lmmm_ymmm(void) {
    setup();
    screen5(&vdp);
    for (unsigned i = 0; i < 4; i++) vram[0 * 128 + i] = (uint8_t)(0x12 + i * 0x22);
    cmd(&vdp, 0, 0, 50, 60, 8, 1, 0, 0, 0x90);                /* LMMM */
    run9968(&vdp);
    for (unsigned x = 0; x < 8; x++) CHECK_EQ(g4_pixel(50 + x, 60), g4_pixel(x, 0));

    /* YMMM: from DX to the right edge */
    vram[5 * 128 + 127] = 0x9C;
    cmd(&vdp, 0, 5, 252, 70, 0, 1, 0, 0, 0xE0);
    run9968(&vdp);
    CHECK_EQ(vram[70 * 128 + 127], 0x9C);
}

static void test_lmmc_lmcm(void) {
    setup();
    screen5(&vdp);
    cmd(&vdp, 0, 0, 4, 8, 2, 2, 0x01, 0, 0xB0);               /* LMMC, first pixel 1 */
    run9968(&vdp);
    CHECK_EQ(vstatus(&vdp, 2) & V9968_S2_TR, V9968_S2_TR);
    uint8_t px[3] = { 2, 3, 4 };
    for (int i = 0; i < 3; i++) {
        vreg(&vdp, 44, px[i]);
        run9968(&vdp);
    }
    CHECK_EQ(v9968_cmd_busy(&vdp), 0);
    CHECK_EQ(g4_pixel(4, 8), 1);
    CHECK_EQ(g4_pixel(5, 8), 2);
    CHECK_EQ(g4_pixel(4, 9), 3);
    CHECK_EQ(g4_pixel(5, 9), 4);

    cmd(&vdp, 4, 8, 0, 0, 2, 2, 0, 0, 0xA0);                  /* LMCM */
    uint8_t got[4];
    for (int i = 0; i < 4; i++) {
        run9968(&vdp);
        CHECK_EQ(vstatus(&vdp, 2) & V9968_S2_TR, V9968_S2_TR);
        got[i] = vstatus(&vdp, 7);
    }
    run9968(&vdp);
    CHECK_EQ(v9968_cmd_busy(&vdp), 0);
    CHECK_EQ(got[0], 1);
    CHECK_EQ(got[1], 2);
    CHECK_EQ(got[2], 3);
    CHECK_EQ(got[3], 4);
}

/* LRMM against the Python model of geo3d/sim/hra/check_lrmm.py */
static void test_lrmm(void) {
    setup();
    screen5(&vdp);
    v9968_native(&vdp, 0x01);
    for (unsigned x = 0; x < 16; x++)                        /* texture row y = 512 */
        vram[512 * 128 + x / 2] = (uint8_t)((x & 1) ? (vram[512 * 128 + x / 2] | x)
                                                    : (x << 4));
    /* copy 16 texels at half speed (VX = 0.5): each texel twice */
    vreg(&vdp, 17, 32);
    uint8_t b[18] = { 0, 0, 0, 2, 20, 0, 30, 0, 16, 0, 1, 0, 0x0F, 0x00, 0x80, 0x00, 0, 0 };
    for (int i = 0; i < 14; i++) vout(&vdp, 0x8B, b[i]);
    vreg(&vdp, 17, 47);
    for (int i = 14; i < 18; i++) vout(&vdp, 0x8B, b[i]);
    vreg(&vdp, 46, 0x30);
    run9968(&vdp);
    for (unsigned k = 0; k < 16; k++) CHECK_EQ(g4_pixel(20 + k, 30), k / 2);

    /* outside the window: the colour register is written */
    vreg(&vdp, 53, 0); vreg(&vdp, 54, 2);                     /* WSY = 512 */
    vreg(&vdp, 55, 3); vreg(&vdp, 56, 0);                     /* WEX = 3 */
    vreg(&vdp, 57, 0); vreg(&vdp, 58, 2);                     /* WEY = 512 */
    vreg(&vdp, 17, 32);
    for (int i = 0; i < 14; i++) vout(&vdp, 0x8B, i == 6 ? 31 : b[i]);
    vreg(&vdp, 17, 47);
    b[14] = 0x00; b[15] = 0x01;                               /* VX = 1.0 */
    for (int i = 14; i < 18; i++) vout(&vdp, 0x8B, b[i]);
    vreg(&vdp, 46, 0x30);
    run9968(&vdp);
    CHECK_EQ(g4_pixel(23, 31), 3);
    CHECK_EQ(g4_pixel(24, 31), 15);
}

static void test_lfmc(void) {
    setup();
    screen8(&vdp);
    v9968_native(&vdp, 0x00);
    vreg(&vdp, 12, 0x11);                                     /* background */
    cmd(&vdp, 0, 0, 8, 4, 8, 2, 0xEE, 0, 0x20);               /* LFMC, fore 0xEE */
    vreg(&vdp, 44, 0xA5);
    run9968(&vdp);
    vreg(&vdp, 44, 0x0F);
    run9968(&vdp);
    CHECK_EQ(v9968_cmd_busy(&vdp), 0);
    CHECK_EQ(g7_byte(8, 4), 0xEE);
    CHECK_EQ(g7_byte(9, 4), 0x11);
    CHECK_EQ(g7_byte(15, 4), 0xEE);
    CHECK_EQ(g7_byte(8, 5), 0x11);
    CHECK_EQ(g7_byte(15, 5), 0xEE);
}

/* ---- rendering ---- */

static uint16_t line[640];

static uint16_t c565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
}

/* palette entry as the renderer outputs it without gamma */
static uint16_t pal(unsigned i) {
    uint16_t c = vdp.palette[i];
    return c565((c >> 10) & 31, (c >> 5) & 31, c & 31);
}

static void test_render_screen5(void) {
    setup();
    screen5(&vdp);
    vreg(&vdp, 7, 0x04);                                      /* border: colour 4 */
    vram[10 * 128 + 0] = 0x2F;
    v9968_render_line(&vdp, 10, line, 640);
    CHECK_EQ(line[0], pal(4));
    CHECK_EQ(line[64], pal(2));
    CHECK_EQ(line[65], pal(2));
    CHECK_EQ(line[66], pal(15));
    CHECK_EQ(line[68], pal(4));               /* colour 0: border */
    vreg(&vdp, 8, 0x2A);                                      /* TP */
    v9968_render_line(&vdp, 10, line, 640);
    CHECK_EQ(line[68], pal(0));

    /* horizontal scroll: R#26 = 1 shows dot 8 at the left */
    vram[10 * 128 + 4] = 0xE0;
    vreg(&vdp, 26, 1);
    v9968_render_line(&vdp, 10, line, 640);
    CHECK_EQ(line[64], pal(14));
}

static void test_render_g7(void) {
    setup();
    screen8(&vdp);
    vreg(&vdp, 6, 0x00);
    v9968_interleave(&vdp);
    vwrite_addr(&vdp, 5 * 256);
    vout(&vdp, 0x88, 0xFF);                                   /* white */
    vout(&vdp, 0x88, 0x1C);                                   /* red */
    CHECK_EQ(vram[(5 * 256) >> 1], 0xFF);                     /* bank 0 */
    CHECK_EQ(vram[0x10000 | ((5 * 256) >> 1)], 0x1C);         /* bank 1 */
    v9968_render_line(&vdp, 5, line, 640);
    CHECK_EQ(line[64], c565(31, 31, 31));
    CHECK_EQ(line[66], c565(31, 0, 0));
}

static void test_render_yjk(void) {
    setup();
    screen8(&vdp);
    vreg(&vdp, 25, 0x08);                                     /* SCREEN 12 */
    /* Y = 16, J = K = 0 */
    vwrite_addr(&vdp, 0);
    for (int i = 0; i < 4; i++) vout(&vdp, 0x88, 16 << 3);
    v9968_render_line(&vdp, 0, line, 640);
    unsigned b = (5 * 16 + 2) >> 2;
    CHECK_EQ(line[64], pal(0));                               /* 4 dots late: border */
    CHECK_EQ(line[64 + 8], c565(16, 16, b));
}

static void test_sprites_mode2(void) {
    setup();
    screen5(&vdp);
    vreg(&vdp, 5, 0xEF);            /* SAT at 0x7600 */
    vreg(&vdp, 11, 0x00);
    vreg(&vdp, 6, 0x0F);            /* SPG at 0x7800 */
    vreg(&vdp, 1, 0x40);            /* 8x8 */
    uint32_t sat = 0x7600, spg = 0x7800, sct = 0x7400;
    vram[spg + 0] = 0xFF;
    for (int p = 0; p < 32; p++) vram[sat + p * 4] = 216;     /* end of list */
    vram[sat + 0] = 49; vram[sat + 1] = 100; vram[sat + 2] = 0; /* shown on line 50 */
    vram[sct + 0] = 0x0A;
    v9968_render_line(&vdp, 50, line, 640);
    CHECK_EQ(line[64 + 200], pal(10));
    CHECK_EQ(line[64 + 215], pal(10));
    CHECK_EQ(line[64 + 216], pal(0));

    /* nine sprites on a line: 5S and the ninth plane */
    for (int p = 0; p < 9; p++) {
        vram[sat + p * 4] = 49; vram[sat + p * 4 + 1] = (uint8_t)(p * 10);
    }
    v9968_render_line(&vdp, 50, line, 640);
    uint8_t s0 = vstatus(&vdp, 0);
    CHECK_EQ(s0 & 0x40, 0x40);
    CHECK_EQ(s0 & 0x1F, 8);
    /* two overlapping sprites collide */
    vram[sat + 4 + 1] = 4;                                    /* plane 1 over plane 0 */
    vram[sct + 16] = 0x03;
    v9968_render_line(&vdp, 50, line, 640);
    CHECK_EQ(vstatus(&vdp, 0) & 0x20, 0x20);
}

static void test_sprites_mode3(void) {
    setup();
    screen5(&vdp);
    v9968_native(&vdp, 0x08);       /* SP3 */
    vreg(&vdp, 5, 0xFF);            /* SAT at 0x1FE00 - use 0x7E00 */
    vreg(&vdp, 5, 0xFC);            /* SAT 0x7E00 */
    vreg(&vdp, 11, 0x00);
    vreg(&vdp, 6, 0x20);            /* SPG 0x10000 (page 2 of SCREEN 5) */
    uint32_t sat = 0x7E00, spg = 0x10000;
    for (int p = 0; p < 64; p++) { vram[sat + p * 8] = 216; vram[sat + p * 8 + 1] = 0; }
    /* plane 0: Y = 9 (shown from line 10), 16x16 source, 32 dots wide and high */
    uint8_t a[8] = { 9, 0x00, 32, 0x10 | 0x01, 40, 0, 32, 0x00 };   /* RVX, set 1 */
    memcpy(&vram[sat], a, 8);
    for (unsigned x = 0; x < 16; x++) {                       /* row 0 of the pattern */
        uint8_t *b = &vram[spg + x / 2];
        *b = (x & 1) ? (uint8_t)((*b & 0xF0) | (x == 15 ? 1 : 2)) : (uint8_t)((*b & 0x0F) | ((x == 0 ? 0 : 3) << 4));
    }
    vdp.palette[0x10 | 3] = (31 << 10);
    vdp.palette[0x10 | 2] = (31 << 5);
    vdp.palette[0x10 | 1] = 31;
    vdp.pal_gen++;                  /* written behind the ports' back */
    v9968_render_line(&vdp, 10, line, 640);
    /* RVX: dot 0 of the source is shown at the right end */
    CHECK_EQ(line[64 + 2 * 40], c565(0, 0, 31));
    CHECK_EQ(line[64 + 2 * 41], c565(0, 0, 31));
    CHECK_EQ(line[64 + 2 * 42], c565(31, 0, 0));
    CHECK_EQ(line[64 + 2 * 44], c565(0, 31, 0));
    CHECK_EQ(line[64 + 2 * 70], line[0]);                     /* source dot 0: clear */
    CHECK_EQ(line[64 + 2 * 72], line[0]);                     /* beyond MGX */

    /* 50% transparency over a red background */
    vram[10 * 128 + 20] = 0x33;
    vdp.palette[3] = (31 << 10);
    vdp.pal_gen++;
    vram[sat + 3] = 0x80 | 0x10 | 0x01;
    v9968_render_line(&vdp, 10, line, 640);
    CHECK_EQ(line[64 + 2 * 40], c565(15, 0, 15));
}

/* ---- geo3d ---- */

static void gw(v9968_t *v, int sel, uint8_t b) { vout(v, sel ? 0x8F : 0x8D, b); }

static void gwords(v9968_t *v, uint8_t idx, const int *w, int n) {
    gw(v, 0, idx);
    for (int i = 0; i < n; i++) { gw(v, 1, (uint8_t)w[i]); gw(v, 1, (uint8_t)(w[i] >> 8)); }
}

static void test_geo3d_immediate(void) {
    setup();
    /* identity, translation (0, 0, 1000), F = 256, centre (128, 106) */
    int cfg[18] = { 16384, 0, 0, 0, 16384, 0, 0, 0, 16384, 0, 0, 1000,
                    256, 128, 106, 16, 256, 212 };
    gwords(&vdp, 0x00, cfg, 18);
    int vtx[3] = { 100, 50, 0 };
    gwords(&vdp, 0x24, vtx, 3);
    gw(&vdp, 0, 0x30);
    uint8_t r[7];
    for (int i = 0; i < 7; i++) r[i] = vin(&vdp, 0x8F);
    CHECK_EQ(r[0] | (r[1] << 8), 128 + 25);
    CHECK_EQ(r[2] | (r[3] << 8), 106 - 12);
    CHECK_EQ(r[4] | (r[5] << 8), 1000);
    CHECK_EQ(r[6], 0);
    gw(&vdp, 0, 0x49);
    CHECK_EQ(vin(&vdp, 0x8F), GEO3D_ID);
    gw(&vdp, 0, 0x6F);
    CHECK_EQ(vin(&vdp, 0x8F), 0xA0);
}

static void test_geo3d_run(void) {
    setup();
    screen5(&vdp);
    int cfg[18] = { 16384, 0, 0, 0, 16384, 0, 0, 0, 16384, 0, 0, 1000,
                    256, 128, 106, 16, 256, 212 };
    gwords(&vdp, 0x00, cfg, 18);
    gw(&vdp, 0, 0x40);
    uint8_t setup_b[8] = { 0, 0, 2, 1, 15, 0, 0, 0 };         /* 2 vertices, 1 edge */
    for (int i = 0; i < 8; i++) gw(&vdp, 1, setup_b[i]);
    int v2[6] = { -400, 0, 0, 400, 0, 0 };
    gw(&vdp, 0, 0x50);
    for (int i = 0; i < 6; i++) { gw(&vdp, 1, (uint8_t)v2[i]); gw(&vdp, 1, (uint8_t)(v2[i] >> 8)); }
    gw(&vdp, 0, 0x51);
    gw(&vdp, 1, 0);
    gw(&vdp, 1, 1);
    gw(&vdp, 0, 0x48);
    gw(&vdp, 1, 0x01);                                        /* RUN */
    CHECK_EQ(vin(&vdp, 0x8D) & 1, 1);
    run9968(&vdp);
    CHECK_EQ(vin(&vdp, 0x8D) & 1, 0);
    /* x = 128 +- 102, y = 106 */
    CHECK_EQ(g4_pixel(26, 106), 15);
    CHECK_EQ(g4_pixel(230, 106), 15);
    CHECK_EQ(g4_pixel(25, 106), 0);
    CHECK_EQ(g4_pixel(231, 106), 0);
    gw(&vdp, 0, 0x4C);
    CHECK_EQ(vin(&vdp, 0x8F), 1);                             /* DRAWN */
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "vram_access", test_vram_access },
        { "registers", test_registers },
        { "palette", test_palette },
        { "interrupts", test_interrupts },
        { "hmmv_lmmv", test_hmmv_lmmv },
        { "line_pset_point", test_line_pset_point },
        { "srch", test_srch },
        { "lmmm_ymmm", test_lmmm_ymmm },
        { "lmmc_lmcm", test_lmmc_lmcm },
        { "lrmm", test_lrmm },
        { "lfmc", test_lfmc },
        { "render_screen5", test_render_screen5 },
        { "render_g7", test_render_g7 },
        { "render_yjk", test_render_yjk },
        { "sprites_mode2", test_sprites_mode2 },
        { "sprites_mode3", test_sprites_mode3 },
        { "geo3d_immediate", test_geo3d_immediate },
        { "geo3d_run", test_geo3d_run },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("%-20s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
