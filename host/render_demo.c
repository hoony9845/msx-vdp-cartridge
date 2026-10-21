/*
 * Draws a test picture through the port interface (like an MSX program
 * would) and writes the 640x480 output frame to a PPM file.
 *
 *   usage: render_demo [out.ppm]
 */
#include <stdio.h>
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

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "render_demo.ppm";
    v9990_init(&vdp, vram);

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
