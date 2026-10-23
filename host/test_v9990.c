/* Unit tests for the portable V9990 core. */
#include <stdio.h>
#include <stdlib.h>
#include "msx_io.h"
#include "v9990_vram.h"

static int failures;

#define CHECK_EQ(actual, expected) do {                                      \
    long a_ = (long)(actual), e_ = (long)(expected);                          \
    if (a_ != e_) {                                                           \
        printf("  FAIL %s:%d: %s == %ld, expected %ld\n",                     \
               __FILE__, __LINE__, #actual, a_, e_);                          \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t vram[V9990_VRAM_SIZE];
static v9990_t vdp;

static void setup(void) {
    v9990_init(&vdp, vram);
}

static void test_register_auto_increment(void) {
    setup();
    out(&vdp, 0x64, 13);
    out(&vdp, 0x63, 0x11);
    out(&vdp, 0x63, 0x22);
    CHECK_EQ(vdp.regs[13], 0x11);
    CHECK_EQ(vdp.regs[14], 0x22);

    /* WII: register number does not advance */
    out(&vdp, 0x64, 0x80 | 15);
    out(&vdp, 0x63, 0x05);
    out(&vdp, 0x63, 0x07);
    CHECK_EQ(vdp.regs[15], 0x07);
    CHECK_EQ(vdp.regs[16], 0x00);

    /* read back with auto increment */
    out(&vdp, 0x64, 13);
    CHECK_EQ(in(&vdp, 0x63), 0x11);
    CHECK_EQ(in(&vdp, 0x63), 0x22);
}

static void test_vram_write_read(void) {
    setup();
    set_reg(&vdp, 6, 0x80);           /* bitmap mode */
    set_vram_write_addr(&vdp, 0x1234);
    out(&vdp, 0x60, 0xAA);
    out(&vdp, 0x60, 0xBB);
    CHECK_EQ(vdp.vram_write_ptr, 0x1236);

    /* bitmap modes interleave the two banks */
    CHECK_EQ(vram[0x1234 >> 1], 0xAA);
    CHECK_EQ(vram[(1u << 18) | (0x1235 >> 1)], 0xBB);

    set_vram_read_addr(&vdp, 0x1234);
    CHECK_EQ(v9990_peek_port(&vdp, 0), 0xAA);  /* prefetched */
    CHECK_EQ(in(&vdp, 0x60), 0xAA);
    CHECK_EQ(in(&vdp, 0x60), 0xBB);
}

static void test_vram_write_wraps(void) {
    setup();
    set_vram_write_addr(&vdp, 0x7FFFF);
    out(&vdp, 0x60, 1);
    CHECK_EQ(vdp.vram_write_ptr, 0);
}

static void test_palette(void) {
    setup();
    set_palette(&vdp, 1, 31, 0, 0);
    set_palette(&vdp, 2, 0, 31, 0);
    CHECK_EQ(vdp.palette[1].r, 31);
    CHECK_EQ(vdp.palette_rgb565[1], 0xF800);
    CHECK_EQ(vdp.palette_rgb565[2], 0x07E0);
    /* pointer skips the unused 4th byte of each entry */
    CHECK_EQ(vdp.regs[14], 3 << 2);
}

static void test_interrupts(void) {
    setup();
    set_reg(&vdp, 9, V9990_INT_VI);
    CHECK_EQ(v9990_irq_pending(&vdp), 0);
    v9990_video_vblank_start(&vdp);
    CHECK_EQ(v9990_irq_pending(&vdp), 1);
    CHECK_EQ(in(&vdp, 0x65) & V9990_ST_VR, V9990_ST_VR);
    out(&vdp, 0x66, V9990_INT_VI);    /* acknowledge */
    CHECK_EQ(v9990_irq_pending(&vdp), 0);
    v9990_video_vblank_end(&vdp);
    CHECK_EQ(in(&vdp, 0x65) & V9990_ST_VR, 0);
}

static void test_lmmv_bd16(void) {
    setup();
    set_reg(&vdp, 6, 0x80 | (0 << 2) | 3);     /* bitmap, 256 wide, 16bpp */
    out(&vdp, 0x64, 36);                       /* DX..NY, auto increment */
    out(&vdp, 0x63, 10); out(&vdp, 0x63, 0);   /* DX = 10 */
    out(&vdp, 0x63, 5);  out(&vdp, 0x63, 0);   /* DY = 5 */
    out(&vdp, 0x63, 3);  out(&vdp, 0x63, 0);   /* NX = 3 */
    out(&vdp, 0x63, 2);  out(&vdp, 0x63, 0);   /* NY = 2 */
    set_reg(&vdp, 44, 0);                      /* ARG */
    set_reg(&vdp, 45, 0x0C);                   /* LOP = IMP */
    set_reg(&vdp, 46, 0xFF); set_reg(&vdp, 47, 0xFF);
    set_reg(&vdp, 48, 0x34); set_reg(&vdp, 49, 0x12);
    set_reg(&vdp, 52, V9990_CMD_LMMV << 4);
    CHECK_EQ(in(&vdp, 0x65) & V9990_ST_CE, V9990_ST_CE);
    run_cmd(&vdp);
    CHECK_EQ(in(&vdp, 0x65) & V9990_ST_CE, 0);
    CHECK_EQ(vdp.int_flags & V9990_INT_CE, V9990_INT_CE);

    for (unsigned y = 4; y <= 7; y++) {
        for (unsigned x = 9; x <= 13; x++) {
            uint32_t a = (y * 256 + x) * 2;
            unsigned px = v9990_vram_read_bx(&vdp, a) |
                          (v9990_vram_read_bx(&vdp, a + 1) << 8);
            int inside = (x >= 10 && x <= 12 && y >= 5 && y <= 6);
            CHECK_EQ(px, inside ? 0x1234 : 0);
        }
    }
}

static void test_render_bp4(void) {
    setup();
    set_reg(&vdp, 6, 0x80 | (0 << 2) | 1);     /* bitmap 256 wide, 4bpp: B1 */
    set_reg(&vdp, 8, V9990_R8_DISP);
    set_palette(&vdp, 3, 31, 31, 31);
    set_vram_write_addr(&vdp, 0);
    out(&vdp, 0x60, 0x30);                     /* pixel0 = 3, pixel1 = 0 */

    uint16_t line[640];
    v9990_render_line(&vdp, 0, line, 640);
    /* 256 px scaled x2 = 512, centered: 64 px border each side */
    CHECK_EQ(line[63], 0);
    CHECK_EQ(line[64], 0xFFFF);
    CHECK_EQ(line[65], 0xFFFF);
    CHECK_EQ(line[66], 0);
}

static void vram_poke(v9990_t *v, uint32_t addr, uint8_t data) {
    set_vram_write_addr(v, addr);
    out(v, 0x60, data);
}

static void test_display_modes(void) {
    /* manual 8.3: MCS, R#6, R#7 per mode */
    struct { uint8_t mcs, r6, r7; v9990_display_mode_t mode; unsigned w; } t[] = {
        { 0, 0x05, 0x00, V9990_MODE_P1, 256 },
        { 0, 0x59, 0x00, V9990_MODE_P2, 512 },
        { 0, 0x81, 0x00, V9990_MODE_B1, 256 },
        { 1, 0x95, 0x00, V9990_MODE_B2, 384 },
        { 0, 0x95, 0x00, V9990_MODE_B3, 512 },
        { 1, 0xA8, 0x00, V9990_MODE_B4, 768 },
        { 0, 0xA8, 0x01, V9990_MODE_B5, 640 },
        { 0, 0xA8, 0x41, V9990_MODE_B6, 640 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        setup();
        out(&vdp, 0x67, t[i].mcs);
        set_reg(&vdp, 6, t[i].r6);
        set_reg(&vdp, 7, t[i].r7);
        CHECK_EQ(v9990_display_mode(&vdp), t[i].mode);
        CHECK_EQ(v9990_display_width(&vdp), t[i].w);
    }
}

static void test_vram_pointer_latch(void) {
    setup();
    set_vram_write_addr(&vdp, 0x100);
    out(&vdp, 0x64, 0);
    out(&vdp, 0x63, 0x55);              /* R#0 alone: pointer unchanged */
    CHECK_EQ(vdp.vram_write_ptr, 0x100);
    out(&vdp, 0x64, 2);
    out(&vdp, 0x63, 0x00);              /* R#2 commits R#0-R#2 */
    CHECK_EQ(vdp.vram_write_ptr, 0x155);
}

/* P1 test scene: palette, one pattern per layer, display on */
static void p1_setup(void) {
    setup();
    set_reg(&vdp, 6, 0x05);                         /* P1 */
    set_reg(&vdp, 8, V9990_R8_DISP);
    set_reg(&vdp, 13, 0x01 | (0x02 << 2));          /* layer A palette 16.., B 32.. */
    set_palette(&vdp, 17, 31, 0, 0);                /* A colour 1: red */
    set_palette(&vdp, 34, 0, 31, 0);                /* B colour 2: green */
    set_palette(&vdp, 51, 0, 0, 31);                /* sprite palette 3 colour 3: blue */
    /* pattern 1 in both PGTs: line 0 = 1,1,0,0,... (A) / 2,2,2,2,... (B) */
    vram_poke(&vdp, V9990_P1_PGT_A + 4, 0x11);
    for (unsigned i = 0; i < 4; i++) vram_poke(&vdp, V9990_P1_PGT_B + 4 + i, 0x22);
    vram_poke(&vdp, V9990_P1_PNT_A, 1);             /* A(0,0) = pattern 1 */
    vram_poke(&vdp, V9990_P1_PNT_B, 1);             /* B(0,0) = pattern 1 */
    v9990_video_vblank_end(&vdp);
}

#define RED   0xF800
#define GREEN 0x07E0
#define BLUE  0x001F
#define X1(x) (64 + 2 * (x))      /* P1/B1 pixel x -> output pixel (x2, centred) */

static void test_render_p1_layers(void) {
    uint16_t line[640];
    p1_setup();
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(0)], RED);       /* A in front */
    CHECK_EQ(line[X1(1)], RED);
    CHECK_EQ(line[X1(2)], GREEN);     /* A transparent: B shows */
    CHECK_EQ(line[X1(8)], 0);         /* pattern 0 everywhere else: backdrop */

    set_reg(&vdp, 27, 0x0C);          /* PRY=3: B in front from line 192 */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(0)], RED);
    set_reg(&vdp, 27, 0x03);          /* PRX=3: B in front from x=192 */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(0)], RED);
    set_reg(&vdp, 27, 0x00);

    /* scroll layer A left by 2: A pixels 2.. at x 0 -> transparent -> B */
    set_reg(&vdp, 19, 2);
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(0)], GREEN);
}

static void put_sprite(unsigned n, uint8_t y, uint16_t x, uint8_t pat, uint8_t attr) {
    uint32_t a = V9990_P1_SPAT + 4 * n;
    vram_poke(&vdp, a + 0, y);
    vram_poke(&vdp, a + 1, pat);
    vram_poke(&vdp, a + 2, x & 0xFF);
    vram_poke(&vdp, a + 3, (uint8_t)(attr | ((x >> 8) & 3)));
}

static void test_render_p1_sprites(void) {
    uint16_t line[640];
    p1_setup();
    set_reg(&vdp, 25, 0x02);                       /* SGT at 0x08000 */
    vram_poke(&vdp, 0x08000, 0x30);                /* sprite pattern 0, line 0: pixel 0 = 3 */
    for (unsigned n = 0; n < 125; n++) put_sprite(n, 200, 0, 0, V9990_SPR_PR0);  /* off */

    put_sprite(0, 255, 1, 0, 0xC0);                /* Y+1 = 0, x = 1, palette 3 */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(1)], BLUE);                   /* over layer A */
    CHECK_EQ(line[X1(0)], RED);

    put_sprite(0, 255, 1, 0, 0xC0 | V9990_SPR_PR1);  /* behind the front layer */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(1)], RED);
    put_sprite(0, 255, 3, 0, 0xC0 | V9990_SPR_PR1);  /* where A is transparent */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(3)], BLUE);                   /* in front of layer B */

    set_reg(&vdp, 8, V9990_R8_DISP | V9990_R8_SPD);  /* sprites disabled */
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(3)], GREEN);
    set_reg(&vdp, 8, V9990_R8_DISP);

    /* 16 sprites per line: sprite 16 (17th) is dropped */
    for (unsigned n = 0; n < 17; n++) put_sprite(n, 255, 100 + 2 * n, 0, 0xC0);
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(100 + 2 * 15)], BLUE);
    CHECK_EQ(line[X1(100 + 2 * 16)], 0);

    /* x > 1008 wraps to the left edge: x = 1023 -> -1, pixel 1 lands on x 0 */
    for (unsigned n = 0; n < 17; n++) put_sprite(n, 200, 0, 0, V9990_SPR_PR0);
    vram_poke(&vdp, 0x08000, 0x03);                /* pixel 1 = 3 */
    put_sprite(0, 255, 1023, 0, 0xC0);
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[X1(0)], BLUE);
}

static void test_render_p2(void) {
    uint16_t line[640];
    setup();
    set_reg(&vdp, 6, 0x59);                        /* P2 */
    set_reg(&vdp, 8, V9990_R8_DISP | V9990_R8_SPD);
    set_reg(&vdp, 13, 0x01 | (0x02 << 2));         /* even bytes 16.., odd bytes 32.. */
    set_palette(&vdp, 17, 31, 0, 0);
    set_palette(&vdp, 33, 0, 31, 0);
    vram_poke(&vdp, V9990_P2_PGT + 4, 0x11);       /* pattern 1, line 0, even byte */
    vram_poke(&vdp, V9990_P2_PGT + 5, 0x11);       /* odd byte */
    vram_poke(&vdp, V9990_P2_PNT, 1);
    v9990_video_vblank_end(&vdp);
    v9990_render_line(&vdp, 0, line, 640);
    CHECK_EQ(line[64 + 0], RED);                   /* 512 wide: 1:1, 64 px border */
    CHECK_EQ(line[64 + 1], RED);
    CHECK_EQ(line[64 + 2], GREEN);
    CHECK_EQ(line[64 + 3], GREEN);
}

static void test_scroll_split(void) {
    uint16_t line[640];
    setup();
    set_reg(&vdp, 6, 0x80 | 2);                    /* B1, 256 wide, 8bpp */
    set_reg(&vdp, 8, V9990_R8_DISP);
    set_reg(&vdp, 13, 0x40);                       /* BD8 direct colour */
    vram_poke(&vdp, 10 * 256, 0xE0);               /* row 10, x 0: green */
    vram_poke(&vdp, 60 * 256, 0x1C);               /* row 60, x 0: red */
    v9990_video_vblank_end(&vdp);
    v9990_render_line(&vdp, 60, line, 640);
    CHECK_EQ(line[X1(0)], RED);
    v9990_video_line_start(&vdp, 50);
    set_reg(&vdp, 17, 0);                          /* rewrite SCAY low at line 50 */
    v9990_render_line(&vdp, 60, line, 640);
    CHECK_EQ(line[X1(0)], GREEN);                  /* line 60 now shows row 10 */
}

static void test_lmmm(void) {
    setup();
    set_reg(&vdp, 6, 0x80 | 3);                    /* 256 wide, 16bpp */
    set_vram_write_addr(&vdp, (2 * 256 + 3) * 2);  /* source (3,2) .. (4,2) */
    out(&vdp, 0x60, 0x34); out(&vdp, 0x60, 0x12);
    out(&vdp, 0x60, 0x00); out(&vdp, 0x60, 0x00);  /* transparent pixel */
    set_vram_write_addr(&vdp, (20 * 256 + 41) * 2);
    out(&vdp, 0x60, 0xAA); out(&vdp, 0x60, 0x55);  /* existing destination pixel */
    out(&vdp, 0x64, 32);
    unsigned p[] = { 3, 0, 2, 0, 40, 0, 20, 0, 2, 0, 1, 0, 0, 0x1C, 0xFF, 0xFF };
    for (unsigned i = 0; i < 16; i++) out(&vdp, 0x63, (uint8_t)p[i]);   /* R#32-R#47, TP */
    set_reg(&vdp, 52, V9990_CMD_LMMM << 4);
    run_cmd(&vdp);
    CHECK_EQ(v9990_vram_read_bx(&vdp, (20 * 256 + 40) * 2), 0x34);
    CHECK_EQ(v9990_vram_read_bx(&vdp, (20 * 256 + 40) * 2 + 1), 0x12);
    CHECK_EQ(v9990_vram_read_bx(&vdp, (20 * 256 + 41) * 2), 0xAA);   /* TP kept it */
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "register_auto_increment", test_register_auto_increment },
        { "vram_write_read",         test_vram_write_read },
        { "vram_write_wraps",        test_vram_write_wraps },
        { "palette",                 test_palette },
        { "interrupts",              test_interrupts },
        { "lmmv_bd16",               test_lmmv_bd16 },
        { "render_bp4",              test_render_bp4 },
        { "display_modes",           test_display_modes },
        { "vram_pointer_latch",      test_vram_pointer_latch },
        { "render_p1_layers",        test_render_p1_layers },
        { "render_p1_sprites",       test_render_p1_sprites },
        { "render_p2",               test_render_p2 },
        { "scroll_split",            test_scroll_split },
        { "lmmm",                    test_lmmm },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("%d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
