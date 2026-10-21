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

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "register_auto_increment", test_register_auto_increment },
        { "vram_write_read",         test_vram_write_read },
        { "vram_write_wraps",        test_vram_write_wraps },
        { "palette",                 test_palette },
        { "interrupts",              test_interrupts },
        { "lmmv_bd16",               test_lmmv_bd16 },
        { "render_bp4",              test_render_bp4 },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("%d failure(s)\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
