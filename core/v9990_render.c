/*
 * Scanline renderer: one V9990 display line -> RGB565 line buffer.
 *
 * Implemented:
 *   - P1 (2 pattern layers, front/back split by R#27) and P2 pattern modes
 *   - P1/P2 sprites: 125 entries, 16 per line, front/back priority
 *   - bitmap modes BP2/BP4/BP6/BD8/BD16
 *   - X/Y scroll, R512/R256 roll, vertical scroll splits (writing the low
 *     byte of a Y scroll register restarts the layer at that line)
 * TODO: bitmap cursors, YJK/YUV, display adjust (R#16), hi-res BP2/BP4
 *       palette quirks, interlace (EO) line selection.
 *
 * References: Yamaha V9990 Application Manual chapters 8 and 10, and the
 * behaviour of openMSX (src/video/v9990) where the manual is silent.
 */
#include <stdbool.h>
#include <string.h>
#include "v9990.h"
#include "v9990_vram.h"

#define TRANSPARENT 0xFF          /* "no pixel" in a palette-index line */

/* Work buffers for the pattern modes. Only one core renders. */
static uint8_t line_a[512];       /* layer A / image plane, palette index */
static uint8_t line_b[512];       /* layer B (P1 only) */
static uint8_t line_pix[512];     /* composed line, palette index */
static uint8_t line_info[512];    /* 0 = backdrop, 1 = front layer, 2 = sprite */
static uint16_t line_rgb[1024];   /* source-resolution RGB565 */

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

static void scale_copy(const uint16_t *src, unsigned src_w, uint16_t *dst, unsigned dst_w) {
    uint32_t step = (uint32_t)(((uint64_t)src_w << 16) / dst_w);
    uint32_t pos = 0;
    for (unsigned i = 0; i < dst_w; i++, pos += step)
        dst[i] = src[pos >> 16];
}

/* ------------------------------------------------------------------ */
/* Vertical scroll                                                     */
/* ------------------------------------------------------------------ */

/* Roll mask from R#18 bits 7-6 (R512/R256); 0 = roll at the image height. */
static unsigned roll_mask(const v9990_t *v, unsigned image_mask) {
    switch (v->regs[V9990_R_SCROLL_AY_HI] >> 6) {
    case 0:  return image_mask;
    case 2:  return 0x1FF;
    default: return 0x0FF;     /* 1 = 256 lines; 3 undocumented */
    }
}

/* Image Y of layer A for a display line. */
static unsigned image_y_a(const v9990_t *v, unsigned line, unsigned image_mask) {
    unsigned scroll = v->regs[V9990_R_SCROLL_AY_LO] | ((v->scroll_ay_high & 0x1Fu) << 8);
    unsigned mask = roll_mask(v, image_mask);
    unsigned base = scroll & ~mask & image_mask;
    unsigned y = (line - v->line_offset_a) & 0xFFFF;
    return (base + ((y + scroll) & mask)) & image_mask;
}

/* ------------------------------------------------------------------ */
/* Pattern modes                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t name_base;     /* pattern name table */
    uint32_t pgt_base;      /* pattern generator table */
    unsigned names_per_row; /* 64 (P1) / 128 (P2) */
    unsigned pgt_pitch;     /* bytes per PGT line: 128 (P1) / 256 (P2) */
    unsigned x_mask;        /* image width - 1 */
    uint8_t  pal_even, pal_odd;  /* palette offsets for even/odd PGT bytes */
} pattern_layer_t;

/* One line of a pattern layer into palette indices (TRANSPARENT for 0). */
static void render_pattern_layer(const v9990_t *v, const pattern_layer_t *l,
                                 unsigned img_x, unsigned img_y,
                                 uint8_t *out, unsigned width) {
    const unsigned pats_per_row = l->pgt_pitch * 2 / 8;
    uint32_t row_addr = 0;
    for (unsigned i = 0; i < width; i++) {
        unsigned x = (img_x + i) & l->x_mask;
        if (i == 0 || (x & 7) == 0) {
            uint32_t na = l->name_base + ((img_y >> 3) * l->names_per_row + (x >> 3)) * 2;
            unsigned pat = (v9990_vram_read(v, na) | (v9990_vram_read(v, na + 1) << 8)) & 0x1FFF;
            row_addr = l->pgt_base
                     + ((pat / pats_per_row) * 8 + (img_y & 7)) * l->pgt_pitch
                     + (pat % pats_per_row) * 4;
        }
        uint32_t a = row_addr + ((x & 7) >> 1);
        uint8_t byte = v9990_vram_read(v, a);
        uint8_t c = (x & 1) ? (byte & 0x0F) : (byte >> 4);     /* left pixel in the MSBs */
        out[i] = c ? (uint8_t)(((a & 1) ? l->pal_odd : l->pal_even) + c) : TRANSPARENT;
    }
}

/* Sprites of the pattern modes on top of line_pix / line_info. */
static void render_sprites(const v9990_t *v, bool p2, unsigned line, unsigned width) {
    if (v->regs[V9990_R_CONTROL] & V9990_R8_SPD) return;

    const uint32_t spat = p2 ? V9990_P2_SPAT : V9990_P1_SPAT;
    const uint8_t r25 = v->regs[V9990_R_SPRITE_PGT];
    const uint32_t sgt = p2 ? ((uint32_t)(r25 & 0x0F) << 15) : ((uint32_t)(r25 & 0x0E) << 14);

    /* Up to 16 sprites per line; hidden (PR0) ones still count. */
    uint8_t visible[16];
    unsigned count = 0, limit = 16;
    for (unsigned s = 0; s < 125 && count < limit; s++) {
        uint32_t a = spat + 4 * s;
        uint8_t row = (uint8_t)(line - (uint8_t)(v9990_vram_read(v, a) + 1));
        if (row >= 16) continue;
        if (v9990_vram_read(v, a + 3) & V9990_SPR_PR0)
            limit--;
        else
            visible[count++] = (uint8_t)s;
    }

    /* Lower sprite numbers win: draw in order and never overwrite a sprite. */
    for (unsigned k = 0; k < count; k++) {
        uint32_t a = spat + 4 * visible[k];
        uint8_t attr = v9990_vram_read(v, a + 3);
        uint8_t pat  = v9990_vram_read(v, a + 1);
        uint8_t row  = (uint8_t)(line - (uint8_t)(v9990_vram_read(v, a) + 1));
        int x = v9990_vram_read(v, a + 2) | ((attr & 0x03) << 8);
        if (x > 1008) x -= 1024;                 /* partly off the left edge */
        uint8_t level = (attr & V9990_SPR_PR1) ? 1 : 2;   /* behind / in front of layer */
        uint8_t pal = (attr >> 2) & 0x30;        /* SC5-SC4 */

        uint32_t pa = p2 ? sgt + 256u * (((pat & 0xE0) >> 1) + row) + 8u * (pat & 0x1F)
                         : sgt + 128u * ((pat & 0xF0) + row) + 8u * (pat & 0x0F);
        for (unsigned i = 0; i < 8; i++) {
            uint8_t byte = v9990_vram_read(v, pa + i);
            for (unsigned h = 0; h < 2; h++) {
                int px = x + (int)(2 * i + h);
                uint8_t c = h ? (byte & 0x0F) : (byte >> 4);
                if (!c || px < 0 || px >= (int)width) continue;
                if (line_info[px] < level) line_pix[px] = (uint8_t)(pal + c);
                line_info[px] = 2;   /* also when hidden behind the front layer */
            }
        }
    }
}

static void render_pattern_mode(const v9990_t *v, bool p2, unsigned line, unsigned width) {
    const uint8_t r13 = v->regs[V9990_R_PALETTE_CTRL];
    const uint8_t pal_a = (uint8_t)((r13 & 0x03) << 4);
    const uint8_t pal_b = (uint8_t)((r13 & 0x0C) << 2);
    const unsigned scax = (v->regs[V9990_R_SCROLL_AX_HI] << 3) | (v->regs[V9990_R_SCROLL_AX_LO] & 7);

    if (p2) {
        /* single 1024x512 image plane; even/odd PGT bytes use two palettes */
        pattern_layer_t l = { V9990_P2_PNT, V9990_P2_PGT, 128, 256, 1023, pal_a, pal_b };
        render_pattern_layer(v, &l, scax, image_y_a(v, line, 0x1FF), line_a, width);
        for (unsigned i = 0; i < width; i++) {
            line_pix[i] = line_a[i];
            line_info[i] = line_a[i] != TRANSPARENT;
        }
    } else {
        pattern_layer_t la = { V9990_P1_PNT_A, V9990_P1_PGT_A, 64, 128, 511, pal_a, pal_a };
        pattern_layer_t lb = { V9990_P1_PNT_B, V9990_P1_PGT_B, 64, 128, 511, pal_b, pal_b };
        unsigned scbx = ((v->regs[V9990_R_SCROLL_BX_HI] & 0x3F) << 3) |
                        (v->regs[V9990_R_SCROLL_BX_LO] & 7);
        unsigned scby = v->regs[V9990_R_SCROLL_BY_LO] | ((v->scroll_by_high & 1u) << 8);
        unsigned yb = ((line - v->line_offset_b) + scby) & 0x1FF;   /* B always rolls at 512 */
        render_pattern_layer(v, &la, scax, image_y_a(v, line, 0x1FF), line_a, width);
        render_pattern_layer(v, &lb, scbx, yb, line_b, width);

        /* Layer B is in front right of PRX and from PRY down (R#27). */
        uint8_t r27 = v->regs[V9990_R_PRIORITY_CTRL];
        unsigned prx = (r27 & 0x03) ? (r27 & 0x03) * 64u : 256u;
        unsigned pry = (r27 & 0x0C) ? ((r27 >> 2) & 3) * 64u : 256u;
        if (line >= pry) prx = 0;
        for (unsigned i = 0; i < width; i++) {
            bool b_front = i >= prx;
            uint8_t front = b_front ? line_b[i] : line_a[i];
            uint8_t rear  = b_front ? line_a[i] : line_b[i];
            line_pix[i] = front != TRANSPARENT ? front : rear;
            line_info[i] = front != TRANSPARENT;
        }
    }

    render_sprites(v, p2, line, width);

    const uint16_t backdrop = v9990_backdrop_rgb565(v);
    for (unsigned i = 0; i < width; i++)
        line_rgb[i] = line_pix[i] == TRANSPARENT ? backdrop : v->palette_rgb565[line_pix[i] & 0x3F];
}

/* ------------------------------------------------------------------ */
/* Bitmap modes                                                        */
/* ------------------------------------------------------------------ */

/* One bitmap row, fetched in physical order: the even bytes (bank 0)
 * then the odd bytes (bank 1). Largest row: 4096 bytes (2048 x 16bpp). */
static uint8_t row_even[2048] __attribute__((aligned(4)));
static uint8_t row_odd[2048] __attribute__((aligned(4)));

static void fetch_row(const v9990_t *v, uint32_t row_start, uint32_t row_bytes) {
    /* row_start is a multiple of row_bytes (>= 64), so both halves are
     * contiguous and word aligned in physical VRAM */
    uint32_t half = row_bytes / 2u, phys = (row_start & V9990_VRAM_MASK) >> 1;
    if (v->vram_fetch) {
        v->vram_fetch(v, phys, row_even, half);
        v->vram_fetch(v, phys | 0x40000u, row_odd, half);
    } else {
        memcpy(row_even, v->vram + phys, half);
        memcpy(row_odd, v->vram + (phys | 0x40000u), half);
    }
}

/* byte at offset `off` of the fetched row */
#define ROW_BYTE(off) (((off) & 1u) ? row_odd[(off) >> 1] : row_even[(off) >> 1])

static uint16_t src_px[2048] __attribute__((aligned(4)));   /* decoded source pixels */
static uint16_t bd8_lut[256];
static bool bd8_lut_ready;

/* Byte-per-pixel modes (BP6, BD8): the bytes of x and x + 1 (x even) are
 * row_even[x / 2] and row_odd[x / 2], so pixels are decoded in pairs, and
 * where the row does not wrap, four pairs per 32-bit load. */
static uint16_t *decode_bytes(uint16_t *dst, const uint16_t *lut, uint32_t mask,
                              unsigned scx, unsigned iw, unsigned src_w) {
    const unsigned hmask = iw / 2u - 1u;
    unsigned x = scx & (iw - 1u), i = 0;
    if (x & 1u) {                               /* odd start: one pixel */
        dst[i++] = lut[row_odd[x >> 1] & mask];
        x = (x + 1u) & (iw - 1u);
    }
    unsigned h = x >> 1;
    for (; i + 1u < src_w; i += 2u, h = (h + 1u) & hmask) {
        if ((h & 3u) == 0 && h + 4u <= iw / 2u && i + 8u <= src_w &&
            ((uintptr_t)(dst + i) & 3u) == 0)
            break;                              /* aligned: go fast */
        dst[i]      = lut[row_even[h] & mask];
        dst[i + 1u] = lut[row_odd[h] & mask];
    }
    /* fast part: 8 pixels per pass while the row does not wrap */
    while (i + 8u <= src_w && h + 4u <= iw / 2u && (h & 3u) == 0 &&
           ((uintptr_t)(dst + i) & 3u) == 0) {
        uint32_t e = *(const uint32_t *)&row_even[h];
        uint32_t o = *(const uint32_t *)&row_odd[h];
        uint32_t *d = (uint32_t *)(dst + i);
        d[0] = lut[e & mask]         | ((uint32_t)lut[o & mask] << 16);
        d[1] = lut[(e >> 8) & mask]  | ((uint32_t)lut[(o >> 8) & mask] << 16);
        d[2] = lut[(e >> 16) & mask] | ((uint32_t)lut[(o >> 16) & mask] << 16);
        d[3] = lut[(e >> 24) & mask] | ((uint32_t)lut[(o >> 24) & mask] << 16);
        i += 8u;
        h = (h + 4u) & hmask;
    }
    for (; i + 1u < src_w; i += 2u, h = (h + 1u) & hmask) {
        dst[i]      = lut[row_even[h] & mask];
        dst[i + 1u] = lut[row_odd[h] & mask];
    }
    if (i < src_w) dst[i] = lut[row_even[h] & mask];
    return dst;
}

static void render_bitmap(const v9990_t *v, unsigned line,
                          uint16_t *out, unsigned dst_w, unsigned src_w) {
    const unsigned bpp = v9990_bpp(v);
    const unsigned iw = v9990_image_width(v);
    const unsigned ih_mask = (V9990_VRAM_SIZE * 8u / (iw * bpp)) - 1u;
    const v9990_color_mode_t cm = v9990_color_mode(v);
    const uint8_t r13 = v->regs[V9990_R_PALETTE_CTRL];
    const uint16_t *pal = v->palette_rgb565;

    unsigned scx = (v->regs[V9990_R_SCROLL_AX_HI] << 3) |
                   (v->regs[V9990_R_SCROLL_AX_LO] & 0x07u);
    unsigned y = image_y_a(v, line, ih_mask);
    const uint32_t row_bytes = iw * bpp / 8u;
    fetch_row(v, (uint32_t)y * row_bytes, row_bytes);

    const unsigned xmask = iw - 1u;
    if (src_w > 2048u) src_w = 2048u;
    const bool direct = dst_w == src_w;        /* no scaling: decode into out */
    uint16_t *px = src_px;

    switch (cm) {
    case V9990_COLOR_BP2: {
        /* leftmost pixel in the MSBs (manual 10.10) */
        const uint16_t *p = pal + ((r13 & 0x0F) << 2);
        for (unsigned i = 0, x = scx & xmask; i < src_w; i++, x = (x + 1u) & xmask) {
            unsigned off = x >> 2;
            src_px[i] = p[(ROW_BYTE(off) >> (6 - ((x & 3) << 1))) & 3];
        }
        break;
    }
    case V9990_COLOR_BP4: {
        const uint16_t *p = pal + ((r13 & 0x0C) << 2);
        for (unsigned i = 0, x = scx & xmask; i < src_w; i++, x = (x + 1u) & xmask) {
            unsigned off = x >> 1;
            src_px[i] = p[(ROW_BYTE(off) >> (4 - ((x & 1) << 2))) & 15];
        }
        break;
    }
    case V9990_COLOR_BP6:
        px = decode_bytes(direct ? out : src_px, pal, 0x3F, scx, iw, src_w);
        break;
    case V9990_COLOR_BD16:
        /* 16bpp: low byte even (bank 0), high byte odd (bank 1) */
        for (unsigned i = 0, x = scx & xmask; i < src_w; i++, x = (x + 1u) & xmask)
            src_px[i] = bd16_to_rgb565((uint16_t)(row_even[x] | (row_odd[x] << 8)));
        break;
    case V9990_COLOR_BD8:
    default:    /* TODO: YJK / YUV */
        if (!bd8_lut_ready) {
            for (unsigned c = 0; c < 256; c++) bd8_lut[c] = bd8_to_rgb565((uint8_t)c);
            bd8_lut_ready = true;
        }
        px = decode_bytes(direct ? out : src_px, bd8_lut, 0xFF, scx, iw, src_w);
        break;
    }

    if (px == out) {
        /* decoded in place */
    } else if (dst_w == src_w) {
        memcpy(out, src_px, src_w * sizeof(uint16_t));
    } else if (dst_w == 2u * src_w) {
        for (unsigned i = 0; i < src_w; i++) out[2u * i] = out[2u * i + 1u] = src_px[i];
    } else {
        scale_copy(src_px, src_w, out, dst_w);
    }
}

/* ------------------------------------------------------------------ */

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
        render_pattern_mode(v, mode == V9990_MODE_P2, line, src_w);
        scale_copy(line_rgb, src_w, out + left, dst_w);
    } else {
        render_bitmap(v, line, out + left, dst_w, src_w);
    }
    fill(out + left + dst_w, out_width - left - dst_w, backdrop);
}
