/*
 * Draws a test picture through the port interface (like an MSX program
 * would) and writes the 640x480 output frame to a PPM file.
 *
 *   usage: render_demo [out.ppm] [bitmap|p1]
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "msx_io.h"

#define OUT_W 640
#define OUT_H 480

static uint8_t vram[V9990_VRAM_SIZE];
static v9990_t vdp;

static void lmmv(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    out(&vdp, 0x64, 36);
    out(&vdp, 0x63, x & 0xFF); out(&vdp, 0x63, x >> 8);
    out(&vdp, 0x63, y & 0xFF); out(&vdp, 0x63, y >> 8);
    out(&vdp, 0x63, w & 0xFF); out(&vdp, 0x63, w >> 8);
    out(&vdp, 0x63, h & 0xFF); out(&vdp, 0x63, h >> 8);
    out(&vdp, 0x63, 0);        /* ARG */
    out(&vdp, 0x63, 0x0C);     /* LOP = IMP */
    out(&vdp, 0x63, 0xFF); out(&vdp, 0x63, 0xFF);           /* WM */
    out(&vdp, 0x63, color & 0xFF); out(&vdp, 0x63, color >> 8); /* FC */
    set_reg(&vdp, 52, V9990_CMD_LMMV << 4);
    run_cmd(&vdp);
}

static uint16_t grb555(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)((g << 10) | (r << 5) | b);
}

static void poke(uint32_t addr, uint8_t data) {
    set_vram_write_addr(&vdp, addr);
    out(&vdp, 0x60, data);
}

/* P1: checkerboard tiles on layer B, a framed window on layer A scrolled
 * a bit, and a row of sprites (some behind layer A). */
static void p1_scene(void) {
    set_reg(&vdp, 6, 0x05);                         /* P1 */
    set_reg(&vdp, 8, V9990_R8_DISP);
    set_reg(&vdp, 13, 0x00 | (0x01 << 2));          /* A: palette 0.., B: 16.. */
    for (unsigned i = 1; i < 16; i++) {
        set_palette(&vdp, (uint8_t)i, (uint8_t)(i * 2), (uint8_t)(31 - i * 2), 20);   /* A */
        set_palette(&vdp, (uint8_t)(16 + i), 4, (uint8_t)(i), 10);                  /* B */
        set_palette(&vdp, (uint8_t)(48 + i), 31, 31, (uint8_t)(i * 2));             /* sprites */
    }
    set_palette(&vdp, 0, 0, 0, 6);
    /* patterns: A1 = frame, A2 = solid; B1/B2 = checker shades */
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned b = 0; b < 4; b++) {
            uint8_t frame = (y == 0 || y == 7) ? 0xFF : (b == 0 ? 0xF0 : (b == 3 ? 0x0F : 0x00));
            poke(V9990_P1_PGT_A + y * 128 + 1 * 4 + b, frame);
            poke(V9990_P1_PGT_A + y * 128 + 2 * 4 + b, 0x88);
            poke(V9990_P1_PGT_B + y * 128 + 1 * 4 + b, 0x33);
            poke(V9990_P1_PGT_B + y * 128 + 2 * 4 + b, 0x99);
        }
    }
    for (unsigned ty = 0; ty < 64; ty++) {
        for (unsigned tx = 0; tx < 64; tx++) {
            uint32_t a = (ty * 64 + tx) * 2;
            poke(V9990_P1_PNT_B + a, (uint8_t)(((tx ^ ty) & 1) ? 1 : 2));
            uint8_t pa = 0;
            if (tx >= 6 && tx < 22 && ty >= 6 && ty < 18)
                pa = (tx == 6 || tx == 21 || ty == 6 || ty == 17) ? 1 : (((tx + ty) % 5) ? 0 : 2);
            poke(V9990_P1_PNT_A + a, pa);
        }
    }
    set_reg(&vdp, 19, 4);                            /* scroll A by 4 dots */
    /* sprites: a 16x16 ball at SGT 0x08000 */
    set_reg(&vdp, 25, 0x02);
    for (unsigned y = 0; y < 16; y++) {
        for (unsigned x = 0; x < 16; x += 2) {
            int dx0 = (int)x - 8, dx1 = (int)x + 1 - 8, dy = (int)y - 8;
            uint8_t c0 = (uint8_t)((dx0 * dx0 + dy * dy < 56) ? 1 + (y + x) % 15 : 0);
            uint8_t c1 = (uint8_t)((dx1 * dx1 + dy * dy < 56) ? 1 + (y + x + 1) % 15 : 0);
            poke(0x08000 + y * 128 + x / 2, (uint8_t)((c0 << 4) | c1));
        }
    }
    for (unsigned n = 0; n < 125; n++) {
        uint32_t a = V9990_P1_SPAT + 4 * n;
        bool on = n < 12;
        poke(a + 0, (uint8_t)(on ? 70 + (n % 3) * 20 : 230));
        poke(a + 1, 0);
        poke(a + 2, (uint8_t)(30 + n * 18));
        poke(a + 3, (uint8_t)(0xC0 | (n & 1 ? V9990_SPR_PR1 : 0) | (on ? 0 : V9990_SPR_PR0)));
    }
    v9990_video_vblank_end(&vdp);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "render_demo.ppm";
    const char *scene = argc > 2 ? argv[2] : "bitmap";
    v9990_init(&vdp, vram);
    if (strcmp(scene, "p1") == 0) {
        p1_scene();
        goto write;
    }

    /* B1, 256 wide image, 16bpp (BD16), display on */
    set_reg(&vdp, 6, 0x80 | (0 << 2) | 3);
    set_reg(&vdp, 8, V9990_R8_DISP);
    set_palette(&vdp, 0, 4, 4, 8);
    set_reg(&vdp, 15, 0);

    /* gradient written directly through the VRAM data port */
    set_vram_write_addr(&vdp, 0);
    for (unsigned y = 0; y < 212; y++) {
        for (unsigned x = 0; x < 256; x++) {
            uint16_t c = grb555(x >> 3, y * 31 / 211, 31 - (x >> 3));
            out(&vdp, 0x60, c & 0xFF);
            out(&vdp, 0x60, c >> 8);
        }
    }
    /* rectangles through the command engine */
    lmmv(20, 20, 60, 40, grb555(31, 31, 31));
    lmmv(100, 80, 80, 60, grb555(31, 0, 0));
    lmmv(160, 140, 70, 50, grb555(0, 31, 0));

write:;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);

    unsigned h = v9990_display_height(&vdp);
    unsigned scale = h <= OUT_H / 2 ? 2 : 1;
    unsigned top = (OUT_H - h * scale) / 2;
    uint16_t line[OUT_W];
    for (unsigned y = 0; y < OUT_H; y++) {
        if (y < top || y >= top + h * scale) {
            for (unsigned x = 0; x < OUT_W; x++) line[x] = v9990_backdrop_rgb565(&vdp);
        } else {
            v9990_render_line(&vdp, (y - top) / scale, line, OUT_W);
        }
        for (unsigned x = 0; x < OUT_W; x++) {
            uint16_t c = line[x];
            unsigned char rgb[3] = {
                (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                (unsigned char)((c & 0x1F) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("wrote %s (%ux%u display, scale %u)\n", path, v9990_display_width(&vdp), h, scale);
    return 0;
}
