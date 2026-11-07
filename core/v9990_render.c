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
static uint8_t line_info[512];    /* sprite priority: 0 none, 1 front layer, 2 sprite */
static uint16_t line_rgb[1024];   /* source-resolution RGB565 */

V9990_HOT static inline uint16_t bd8_to_rgb565(uint8_t c) {
    /* GGGRRRBB */
    uint16_t g = (c >> 5) & 7, r = (c >> 2) & 7, b = c & 3;
    uint16_t r5 = (uint16_t)((r << 2) | (r >> 1));
    uint16_t g6 = (uint16_t)((g << 3) | g);
    uint16_t b5 = (uint16_t)((b << 3) | (b << 1) | (b >> 1));
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

V9990_HOT static inline uint16_t bd16_to_rgb565(uint16_t c) {
    /* YS(15) G(14-10) R(9-5) B(4-0) */
    uint16_t g = (c >> 10) & 0x1F, r = (c >> 5) & 0x1F, b = c & 0x1F;
    return (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
}

V9990_HOT uint16_t v9990_backdrop_rgb565(const v9990_t *v) {
    return v->palette_rgb565[v->regs[V9990_R_BACKDROP] & 0x3F];
}

V9990_HOT static void fill(uint16_t *out, unsigned n, uint16_t c) {
    while (n--) *out++ = c;
}

/* Pick an integer upscale if possible, otherwise squeeze to fit. */
V9990_HOT static unsigned scaled_width(unsigned src_w, unsigned out_w) {
    if (src_w > out_w) return out_w;
    return src_w * (out_w / src_w);
}

V9990_HOT static void scale_copy(const uint16_t *src, unsigned src_w, uint16_t *dst, unsigned dst_w) {
    uint32_t step = (uint32_t)(((uint64_t)src_w << 16) / dst_w);
    uint32_t pos = 0;
    for (unsigned i = 0; i < dst_w; i++, pos += step)
        dst[i] = src[pos >> 16];
}

/* ------------------------------------------------------------------ */
/* Vertical scroll                                                     */
/* ------------------------------------------------------------------ */

/* Roll mask from R#18 bits 7-6 (R512/R256); 0 = roll at the image height. */
V9990_HOT static unsigned roll_mask(const v9990_t *v, unsigned image_mask) {
    switch (v->regs[V9990_R_SCROLL_AY_HI] >> 6) {
    case 0:  return image_mask;
    case 2:  return 0x1FF;
    default: return 0x0FF;     /* 1 = 256 lines; 3 undocumented */
    }
}

/* Image Y of layer A for a display line. */
V9990_HOT static unsigned image_y_a(const v9990_t *v, unsigned line, unsigned image_mask) {
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

/* VRAM fetch of `len` physical bytes (the platform may stream them). */
V9990_HOT static void fetch_phys(const v9990_t *v, uint32_t phys, uint8_t *dst, uint32_t len) {
    if (v->vram_fetch) v->vram_fetch(v, phys, dst, len);
    else               memcpy(dst, v9990_vram_ptr(v, phys), len);
}

/* Name table rows of the two layers. A row is fetched again only for
 * another tile row, table or frame: a name written in the middle of a
 * frame can show up to 7 lines later than on the real chip. */
static uint8_t name_cache[2][256] __attribute__((aligned(4)));
static uint32_t name_tag[2][2] = { { ~0u, ~0u }, { ~0u, ~0u } };    /* {address, frame} */
static const uint8_t *name_row;
static const uint8_t *spat;           /* sprite attribute table (in VRAM0) */

V9990_HOT static void select_name_row(const v9990_t *v, const pattern_layer_t *l, unsigned img_y) {
    uint32_t addr = l->name_base + (img_y >> 3) * l->names_per_row * 2u;
    unsigned slot = (l->name_base == V9990_P1_PNT_B) ? 1u : 0u;
    if (name_tag[slot][0] != addr || name_tag[slot][1] != v->frame) {
        fetch_phys(v, addr, name_cache[slot], l->names_per_row * 2u);
        name_tag[slot][0] = addr;
        name_tag[slot][1] = v->frame;
    }
    name_row = name_cache[slot];
}

/* 4 bytes (8 dots, left dot in the MSBs) of the pattern row under image
 * dot x. The layer's name row must be in name_row. */
V9990_HOT static inline void pattern_row(const v9990_t *v, const pattern_layer_t *l, bool p2,
                                         unsigned x, unsigned img_y, uint8_t b[4]) {
    const unsigned pats_per_row = l->pgt_pitch * 2 / 8;
    unsigned n = (x >> 3) * 2u;
    unsigned pat = (name_row[n] | (name_row[n + 1] << 8)) & 0x1FFF;
    uint32_t a = l->pgt_base + ((pat / pats_per_row) * 8 + (img_y & 7)) * l->pgt_pitch
               + (pat % pats_per_row) * 4;
    if (p2) {               /* even bytes in bank 0, odd bytes in bank 1 */
        uint32_t e = (a & 0x7FFFCu) >> 1;
        b[0] = v->vram[e];  b[2] = v->vram[e + 1];
        b[1] = v->vram1[e]; b[3] = v->vram1[e + 1];
    } else {
        const uint8_t *q = v9990_vram_ptr(v, a & V9990_VRAM_MASK);
        b[0] = q[0]; b[1] = q[1]; b[2] = q[2]; b[3] = q[3];
    }
}

/* Draw dots [start, end) of a pattern layer into col (RGB565), image dot
 * img_x at col[0]. tab_e / tab_o: colour of each 4-bit value for even /
 * odd pattern bytes. Back layer: every dot, value 0 = tab[0] (backdrop).
 * Front layer: value 0 is transparent; opaque dots set info[] to 1 if
 * info is given (for the sprite priorities). */
V9990_HOT static void draw_layer(const v9990_t *v, const pattern_layer_t *l, bool p2,
                                 unsigned img_x, unsigned img_y, uint16_t *col,
                                 unsigned start, unsigned end, bool front, uint8_t *info,
                                 const uint16_t *tab_e, const uint16_t *tab_o) {
    select_name_row(v, l, img_y);
    unsigned x = (img_x + start) & l->x_mask;
    for (unsigned i = start; i < end; ) {
        uint8_t b[4];
        pattern_row(v, l, p2, x, img_y, b);
        unsigned d = x & 7u, n = 8u - d;
        if (n > end - i) n = end - i;
        if (n == 8) {                           /* whole tile: unrolled */
            uint16_t *c = col + i;
            if (!front) {
                c[0] = tab_e[b[0] >> 4]; c[1] = tab_e[b[0] & 15];
                c[2] = tab_o[b[1] >> 4]; c[3] = tab_o[b[1] & 15];
                c[4] = tab_e[b[2] >> 4]; c[5] = tab_e[b[2] & 15];
                c[6] = tab_o[b[3] >> 4]; c[7] = tab_o[b[3] & 15];
            } else if (b[0] | b[1] | b[2] | b[3]) {
                for (unsigned k = 0; k < 4; k++) {
                    const uint16_t *t = (k & 1) ? tab_o : tab_e;
                    unsigned hi = b[k] >> 4, lo = b[k] & 15u;
                    if (hi) { c[2 * k] = t[hi];     if (info) info[i + 2 * k] = 1; }
                    if (lo) { c[2 * k + 1] = t[lo]; if (info) info[i + 2 * k + 1] = 1; }
                }
            }
            i += 8;
            x = (x + 8u) & l->x_mask;
            continue;
        }
        if (front && (b[0] | b[1] | b[2] | b[3]) == 0) {   /* fully transparent */
            i += n;
            x = (x + n) & l->x_mask;
            continue;
        }
        for (unsigned j = 0; j < n; j++, d++, i++) {
            unsigned k = d >> 1;
            unsigned c = (d & 1) ? (b[k] & 0x0Fu) : (b[k] >> 4);
            if (front) {
                if (!c) continue;
                if (info) info[i] = 1;
            }
            col[i] = ((k & 1) ? tab_o : tab_e)[c];
        }
        x = (x + n) & l->x_mask;
    }
}

/* Sprites of the pattern modes over col. info: 0 = nothing in front,
 * 1 = front layer, 2 = sprite (a sprite also hides later ones where it is
 * itself behind the front layer). The attribute table (0x3FE00 P1,
 * 0x7BE00 P2: both at physical 0x3FE00) is fetched once per line. */
static uint8_t sprite_list[16];

V9990_HOT static unsigned scan_sprites(const v9990_t *v, unsigned line) {
    if (v->regs[V9990_R_CONTROL] & V9990_R8_SPD) return 0;
    spat = v9990_vram_ptr(v, 0x3FE00u);        /* 500 bytes, all in VRAM0 */
    /* Up to 16 sprites per line; hidden (PR0) ones still count. */
    unsigned count = 0, limit = 16;
    for (unsigned s = 0; s < 125 && count < limit; s++) {
        const uint8_t *a = spat + 4 * s;
        uint8_t row = (uint8_t)(line - (uint8_t)(a[0] + 1));
        if (row >= 16) continue;
        if (a[3] & V9990_SPR_PR0)
            limit--;
        else
            sprite_list[count++] = (uint8_t)s;
    }
    return count;
}

V9990_HOT static void draw_sprites(const v9990_t *v, bool p2, unsigned line, unsigned count,
                                   uint16_t *col, uint8_t *info, unsigned width) {
    const uint8_t r25 = v->regs[V9990_R_SPRITE_PGT];
    const uint32_t sgt = p2 ? ((uint32_t)(r25 & 0x0F) << 15) : ((uint32_t)(r25 & 0x0E) << 14);
    /* Lower sprite numbers win: draw in order and never overwrite a sprite. */
    for (unsigned k = 0; k < count; k++) {
        const uint8_t *a = spat + 4 * sprite_list[k];
        uint8_t attr = a[3];
        uint8_t pat  = a[1];
        uint8_t row  = (uint8_t)(line - (uint8_t)(a[0] + 1));
        int x = a[2] | ((attr & 0x03) << 8);
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
                if (info[px] < level) col[px] = v->palette_rgb565[(pal + c) & 0x3F];
                info[px] = 2;   /* also when hidden behind the front layer */
            }
        }
    }
}

/* One line of P1 / P2 into col (RGB565, `width` source dots). */
V9990_HOT static void render_pattern_mode(const v9990_t *v, bool p2, unsigned line,
                                          uint16_t *col, unsigned width) {
    const uint8_t r13 = v->regs[V9990_R_PALETTE_CTRL];
    const uint8_t pal_a = (uint8_t)((r13 & 0x03) << 4);
    const uint8_t pal_b = (uint8_t)((r13 & 0x0C) << 2);
    const unsigned scax = (v->regs[V9990_R_SCROLL_AX_HI] << 3) | (v->regs[V9990_R_SCROLL_AX_LO] & 7);
    const uint16_t backdrop = v9990_backdrop_rgb565(v);

    /* colour tables, entry 0 = backdrop (only used by a back layer) */
    uint16_t tab_a[16], tab_b[16];
    tab_a[0] = tab_b[0] = backdrop;
    for (unsigned c = 1; c < 16; c++) {
        tab_a[c] = v->palette_rgb565[(pal_a + c) & 0x3F];
        tab_b[c] = v->palette_rgb565[(pal_b + c) & 0x3F];
    }

    unsigned sprites = scan_sprites(v, line);
    uint8_t *info = sprites ? line_info : NULL;
    if (info) for (unsigned i = 0; i < width; i++) info[i] = 0;

    if (p2) {
        /* single 1024x512 image plane; even/odd PGT bytes use two palettes */
        pattern_layer_t l = { V9990_P2_PNT, V9990_P2_PGT, 128, 256, 1023, pal_a, pal_b };
        fill(col, width, backdrop);
        draw_layer(v, &l, true, scax, image_y_a(v, line, 0x1FF), col, 0, width, true, info,
                   tab_a, tab_b);
    } else {
        pattern_layer_t la = { V9990_P1_PNT_A, V9990_P1_PGT_A, 64, 128, 511, pal_a, pal_a };
        pattern_layer_t lb = { V9990_P1_PNT_B, V9990_P1_PGT_B, 64, 128, 511, pal_b, pal_b };
        unsigned scbx = ((v->regs[V9990_R_SCROLL_BX_HI] & 0x3F) << 3) |
                        (v->regs[V9990_R_SCROLL_BX_LO] & 7);
        unsigned scby = v->regs[V9990_R_SCROLL_BY_LO] | ((v->scroll_by_high & 1u) << 8);
        unsigned yb = ((line - v->line_offset_b) + scby) & 0x1FF;   /* B always rolls at 512 */
        unsigned ya = image_y_a(v, line, 0x1FF);

        /* Layer B is in front right of PRX and from PRY down (R#27). */
        uint8_t r27 = v->regs[V9990_R_PRIORITY_CTRL];
        unsigned prx = (r27 & 0x03) ? (r27 & 0x03) * 64u : 256u;
        unsigned pry = (r27 & 0x0C) ? ((r27 >> 2) & 3) * 64u : 256u;
        if (line >= pry) prx = 0;
        if (prx > width) prx = width;
        /* back layers first: B left of PRX, A right of it; then the fronts */
        if (prx > 0)     draw_layer(v, &lb, false, scbx, yb, col, 0, prx, false, NULL, tab_b, tab_b);
        if (prx < width) draw_layer(v, &la, false, scax, ya, col, prx, width, false, NULL, tab_a, tab_a);
        if (prx > 0)     draw_layer(v, &la, false, scax, ya, col, 0, prx, true, info, tab_a, tab_a);
        if (prx < width) draw_layer(v, &lb, false, scbx, yb, col, prx, width, true, info, tab_b, tab_b);
    }

    if (sprites) draw_sprites(v, p2, line, sprites, col, info, width);
}

/* ------------------------------------------------------------------ */
/* Bitmap modes                                                        */
/* ------------------------------------------------------------------ */

/* One bitmap row, fetched in physical order: the even bytes (bank 0)
 * then the odd bytes (bank 1). Largest row: 4096 bytes (2048 x 16bpp). */
static uint8_t row_even[2048] __attribute__((aligned(4)));
static uint8_t row_odd[2048] __attribute__((aligned(4)));

V9990_HOT static void fetch_row(const v9990_t *v, uint32_t row_start, uint32_t row_bytes) {
    /* row_start is a multiple of row_bytes (>= 64), so both halves are
     * contiguous and word aligned in physical VRAM */
    uint32_t half = row_bytes / 2u, phys = (row_start & V9990_VRAM_MASK) >> 1;
    if (v->vram_fetch) {
        v->vram_fetch(v, phys, row_even, half);
        v->vram_fetch(v, phys | 0x40000u, row_odd, half);
    } else {
        memcpy(row_even, v->vram + phys, half);
        memcpy(row_odd, v->vram1 + phys, half);
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
V9990_HOT static uint16_t *decode_bytes(uint16_t *dst, const uint16_t *lut, uint32_t mask,
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

/* ------------------------------------------------------------------ */
/* Bitmap cursors (two 32x32 1bpp, after openMSX's tests on a GFX9000)   */
/* ------------------------------------------------------------------ */

/* Attributes at 0x7FE00 / 0x7FE08 (Y lo, Y hi bit 0, X lo, attr: X hi in
 * bits 1-0, disable bit 4, EOR bit 5, CC1-CC0 bits 7-6), patterns at
 * 0x7FF00 / 0x7FF80 (4 bytes per line, MSB = left). Cursor 0 is in front.
 * CC = 0 takes the bitmap colour, otherwise palette R#28 * 4 + CC; EOR
 * complements the colour. The cursor shows 1 line (2 when interlaced)
 * below its Y. Coordinates are display dots of the current mode. */
V9990_HOT static void draw_cursors(const v9990_t *v, unsigned line, uint16_t *px, unsigned w) {
    if (v->regs[V9990_R_CONTROL] & V9990_R8_SPD) return;
    const unsigned delay = (v->regs[V9990_R_SCREEN_MODE1] & V9990_R7_IL) ? 2u : 1u;
    unsigned x0 = 0;
    uint32_t pattern0 = 0;                      /* dots taken by cursor 0 */
    for (unsigned n = 0; n < 2; n++) {
        uint32_t attr_addr = 0x7FE00u + 8u * n;
        uint8_t attr = v9990_vram_read_bx(v, attr_addr + 6);
        if ((attr & 0x10) || (attr & 0xE0) == 0) continue;     /* off, or invisible */
        unsigned y = v9990_vram_read_bx(v, attr_addr) +
                     (v9990_vram_read_bx(v, attr_addr + 2) & 1u) * 256u + delay;
        unsigned row = (line - y) & 511u;
        if (row >= 32) continue;
        uint32_t pat_addr = 0x7FF00u + 0x80u * n + 4u * row;
        uint32_t pattern = ((uint32_t)v9990_vram_read_bx(v, pat_addr) << 24) |
                           ((uint32_t)v9990_vram_read_bx(v, pat_addr + 1) << 16) |
                           ((uint32_t)v9990_vram_read_bx(v, pat_addr + 2) << 8) |
                           v9990_vram_read_bx(v, pat_addr + 3);
        if (!pattern) continue;
        unsigned x = v9990_vram_read_bx(v, attr_addr + 4) + (attr & 3u) * 256u;
        bool eor_only = (attr & 0xE0) == 0x20;  /* CC = 0: complement the bitmap */
        uint16_t color = v->palette_rgb565[((v->regs[V9990_R_SPRITE_PAL_CTRL] << 2) +
                                            (attr >> 6)) & 0x3F];
        if (attr & 0x20) color ^= 0xFFFF;
        for (unsigned i = 0; i < 32 && x + i < w; i++) {
            if (!(pattern & (0x80000000u >> i))) continue;
            unsigned d = x + i - x0;            /* cursor 0 is in front */
            if (n == 1 && x + i >= x0 && d < 32 && (pattern0 & (0x80000000u >> d))) continue;
            px[x + i] = eor_only ? (uint16_t)(px[x + i] ^ 0xFFFF) : color;
        }
        if (n == 0) { x0 = x; pattern0 = pattern; }
    }
}

V9990_HOT static void render_bitmap(const v9990_t *v, unsigned line,
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

    draw_cursors(v, line, px, src_w);

    if (px == out) {
        /* decoded in place */
    } else if (dst_w == src_w) {
        for (unsigned i = 0; i < src_w; i++) out[i] = src_px[i];
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
        render_pattern_mode(v, mode == V9990_MODE_P2, line, line_rgb, src_w);
        if (dst_w == 2u * src_w && !(((uintptr_t)(out + left)) & 3u)) {
            uint32_t *o = (uint32_t *)(out + left);
            for (unsigned i = 0; i < src_w; i++) o[i] = line_rgb[i] * 0x10001u;
        } else {
            scale_copy(line_rgb, src_w, out + left, dst_w);
        }
    } else {
        render_bitmap(v, line, out + left, dst_w, src_w);
    }
    fill(out + left + dst_w, out_width - left - dst_w, backdrop);
}
