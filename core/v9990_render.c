/*
 * Scanline renderer: one V9990 display line -> RGB565 line buffer.
 *
 * Implemented: bitmap modes with BP2/BP4/BP6/BD8/BD16 and X/Y scroll.
 * TODO: P1/P2 pattern modes, sprites/cursors, YJK/YUV, R512/R256 roll,
 *       display adjust (R#16), border color for wide modes.
 */
#include "v9990.h"
#include "v9990_vram.h"

static inline uint16_t bd8_to_rgb565(uint8_t c) {
    /* GGGRRRBB */
    uint16_t g = (c >> 5) & 7, r = (c >> 2) & 7, b = c & 3;
    uint16_t r5 = (uint16_t)((r << 2) | (r >> 1));
    uint16_t g6 = (uint16_t)((g << 3) | g);
    uint16_t b5 = (uint16_t)((b << 3) | (b << 1) | (b >> 1));
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static inline uint16_t bd16_to_rgb565(uint16_t c) {
    /* YS(15) G(14-10) R(9-5) B(4-0) */
    uint16_t g = (c >> 10) & 0x1F, r = (c >> 5) & 0x1F, b = c & 0x1F;
    return (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
}

uint16_t v9990_backdrop_rgb565(const v9990_t *v) {
    return v->palette_rgb565[v->regs[V9990_R_BACKDROP] & 0x3F];
}

static void fill(uint16_t *out, unsigned n, uint16_t c) {
    while (n--) *out++ = c;
}

/* Pick an integer upscale if possible, otherwise squeeze to fit. */
static unsigned scaled_width(unsigned src_w, unsigned out_w) {
    if (src_w > out_w) return out_w;
    return src_w * (out_w / src_w);
}

static void render_bitmap(const v9990_t *v, unsigned line,
                          uint16_t *out, unsigned dst_w, unsigned src_w) {
    const unsigned bpp = v9990_bpp(v);
    const unsigned iw = v9990_image_width(v);
    const unsigned ih_mask = (V9990_VRAM_SIZE * 8u / (iw * bpp)) - 1u;
    const v9990_color_mode_t cm = v9990_color_mode(v);
    const uint8_t r13 = v->regs[V9990_R_PALETTE_CTRL];

    unsigned scy = v->regs[V9990_R_SCROLL_AY_LO] |
                   ((v->regs[V9990_R_SCROLL_AY_HI] & 0x1Fu) << 8);
    unsigned scx = (v->regs[V9990_R_SCROLL_AX_HI] << 3) |
                   (v->regs[V9990_R_SCROLL_AX_LO] & 0x07u);
    unsigned y = (line + scy) & ih_mask;
    uint32_t row_bits = (uint32_t)y * iw * bpp;

    /* 16.16 fixed point step from output pixel to source pixel */
    uint32_t step = (uint32_t)(((uint64_t)src_w << 16) / dst_w);
    uint32_t pos = 0;

    for (unsigned i = 0; i < dst_w; i++, pos += step) {
        unsigned x = ((pos >> 16) + scx) & (iw - 1u);
        uint32_t bit = row_bits + (uint32_t)x * bpp;
        uint32_t addr = bit >> 3;
        uint16_t c;
        switch (cm) {
        case V9990_COLOR_BP2: {
            /* VERIFY: leftmost pixel in the MSBs */
            unsigned p = (v9990_vram_read_bx(v, addr) >> (6 - (bit & 7))) & 3;
            c = v->palette_rgb565[((r13 & 0x0F) << 2) | p];
            break;
        }
        case V9990_COLOR_BP4: {
            unsigned p = (v9990_vram_read_bx(v, addr) >> (4 - (bit & 7))) & 15;
            c = v->palette_rgb565[((r13 & 0x0C) << 2) | p];
            break;
        }
        case V9990_COLOR_BP6:
            c = v->palette_rgb565[v9990_vram_read_bx(v, addr) & 0x3F];
            break;
        case V9990_COLOR_BD16:
            c = bd16_to_rgb565((uint16_t)(v9990_vram_read_bx(v, addr) |
                                          (v9990_vram_read_bx(v, addr + 1) << 8)));
            break;
        case V9990_COLOR_BD8:
        default:    /* TODO: YJK / YUV */
            c = bd8_to_rgb565(v9990_vram_read_bx(v, addr));
            break;
        }
        out[i] = c;
    }
}

V9990_HOT void v9990_render_line(const v9990_t *v, unsigned line,
                                 uint16_t *out, unsigned out_width) {
    const uint16_t backdrop = v9990_backdrop_rgb565(v);
    const v9990_display_mode_t mode = v9990_display_mode(v);

    if (!(v->regs[V9990_R_CONTROL] & V9990_R8_DISP) ||
        mode == V9990_MODE_STANDBY || line >= v9990_display_height(v)) {
        fill(out, out_width, backdrop);
        return;
    }

    unsigned src_w = v9990_display_width(v);
    unsigned dst_w = scaled_width(src_w, out_width);
    unsigned left = (out_width - dst_w) / 2;

    fill(out, left, backdrop);
    if (mode == V9990_MODE_P1 || mode == V9990_MODE_P2) {
        fill(out + left, dst_w, backdrop);      /* TODO: pattern modes */
    } else {
        render_bitmap(v, line, out + left, dst_w, src_w);
    }
    fill(out + left + dst_w, out_width - left - dst_w, backdrop);
}
