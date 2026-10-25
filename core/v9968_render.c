/*
 * Scanline renderer: one V9968 display line -> RGB565 line buffer.
 *
 * Implemented:
 *   - TEXT1/2 (with blink), MULTICOLOR, GRAPHIC1-7, YJK and YJK+palette,
 *     the 256-colour extended palette (EPAL)
 *   - V9958 horizontal scroll (R#26/R#27, two pages with SP2, MSK), R#23,
 *     display adjust (vertical, as the RTL), even/odd pages and the flat
 *     interlace (both fields shown as one progressive picture)
 *   - sprite mode 1/2 (CC/IC, EC, 16 per line with S16, priority shuffle)
 *     and the V9968 sprite mode 3 (64 planes, 16 per line, scaled from a
 *     GRAPHIC4 page, reversal, palette sets, 25/50/75% transparency)
 *   - S#0 (5S, C, the overflowing plane) and the collision coordinates
 *
 * The address formulas and the pixel rules follow
 * vdp_timing_control_screen_mode.v, vdp_sprite_*.v and vdp_color_palette.v.
 */
#include <string.h>
#include "v9968.h"

#define LINE_MAX 512

static uint16_t line_c15[LINE_MAX];       /* composed line, 0RRRRRGGGGGBBBBB */
static uint8_t  bg_idx[LINE_MAX];         /* palette modes: palette index */
static uint8_t  bg_zero[LINE_MAX];        /* pixel value was 0 (transparent) */

/* sprites, 256-dot resolution */
static uint8_t  spr_col[256];             /* {palette set, colour} */
static uint8_t  spr_on[256];
static uint8_t  spr_fix[256];
static uint8_t  spr_tr[256];              /* mode 3 transparency 0..3 */

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

static const uint8_t gamma5[32] = {           /* gamma_table >> 3 */
    0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 23, 24, 25, 26, 27, 28, 29, 30, 31, 31,
};
static const uint8_t gamma6[32] = {           /* gamma_table >> 2 */
    0, 3, 6, 9, 11, 13, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34,
    36, 38, 40, 42, 44, 46, 47, 49, 51, 53, 55, 56, 58, 60, 62, 63,
};

static inline uint16_t c15_to_565(bool gamma, uint16_t c) {
    unsigned r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    if (gamma) return (uint16_t)((gamma5[r] << 11) | (gamma6[g] << 5) | gamma5[b]);
    return (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
}

/* GRAPHIC7 fixed colours: GGGRRRBB */
static inline uint16_t g7_c15(uint8_t c) {
    unsigned g = c >> 5, r = (c >> 2) & 7, b = c & 3;
    unsigned r5 = (r << 2) | (r >> 1), g5 = (g << 2) | (g >> 1);
    unsigned b5 = (b << 3) | ((b >> 1) & b & 1) << 2 | b;
    return (uint16_t)((r5 << 10) | (g5 << 5) | b5);
}

/* GRAPHIC7 sprite colours (GGGRRRBB) */
static const uint8_t g7_sprite[16] = {
    0x00, 0x01, 0x0C, 0x0D, 0x60, 0x61, 0x6C, 0x6D,
    0x9D, 0x03, 0x1C, 0x1F, 0xE0, 0xE3, 0xFC, 0xFF,
};

static inline uint16_t mix5(unsigned bg, unsigned sp, unsigned t) {
    switch (t) {
    case 1:  return (uint16_t)((3 * sp + bg) >> 2);
    case 2:  return (uint16_t)((2 * sp + 2 * bg) >> 2);
    case 3:  return (uint16_t)((sp + 3 * bg) >> 2);
    default: return (uint16_t)sp;
    }
}

static uint16_t mix_c15(uint16_t bg, uint16_t sp, unsigned t) {
    if (t == 0) return sp;
    return (uint16_t)((mix5((bg >> 10) & 31, (sp >> 10) & 31, t) << 10) |
                      (mix5((bg >> 5) & 31, (sp >> 5) & 31, t) << 5) |
                       mix5(bg & 31, sp & 31, t));
}

static bool g7_fixed(const v9968_t *v, v9968_mode_t m) {
    return m == V9968_MODE_G7 && !v9968_epal(v) && !(v->regs[25] & 0x08);
}

/* Border colour for the left (half = 0) and right half of a dot. */
static uint16_t border_c15(const v9968_t *v, v9968_mode_t m, int half) {
    uint8_t bd = v->regs[7];
    if (m == V9968_MODE_G7) {
        if (v9968_epal(v))          return v->palette[bd];
        if (!(v->regs[25] & 0x08))  return g7_c15(bd);
        return v->palette[bd & 15];
    }
    if (m == V9968_MODE_G5) return v->palette[half ? (bd & 3) : ((bd >> 2) & 3)];
    return v->palette[bd & 15];
}

uint16_t v9968_backdrop_rgb565(const v9968_t *v) {
    return c15_to_565(v->gamma, border_c15(v, v9968_mode(v), 0));
}

/* ------------------------------------------------------------------ */
/* Line geometry                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    v9968_t *v;
    v9968_mode_t mode;
    bool il;              /* G6/G7 interleaved VRAM */
    unsigned y;           /* field line (screen_pos_y) */
    unsigned py;          /* pixel_pos_y: y + R#23, 8 bits */
    unsigned field;       /* interlace field / even-odd page */
    bool hires;           /* 512 pixels */
    unsigned ho_l;        /* R#27: dots of border at the left */
    unsigned x0;          /* first source dot (R#26 * 8) */
} line_t;

static inline uint8_t rd(const line_t *l, uint32_t a) {
    return l->v->vram[v9968_phys(l->il, a)];
}

static inline uint32_t name_base(const v9968_t *v)  { return (uint32_t)v->regs[2] << 10; }
static inline uint32_t color_base(const v9968_t *v) {
    return ((uint32_t)(v->regs[10] & 0x0F) << 14) | ((uint32_t)v->regs[3] << 6);
}
static inline uint32_t pgen_base(const v9968_t *v)  { return (uint32_t)(v->regs[4] & 0x7F) << 11; }
static inline uint32_t sat_base(const v9968_t *v) {
    return ((uint32_t)(v->regs[11] & 7) << 15) | ((uint32_t)v->regs[5] << 7);
}
static inline uint32_t spg_base(const v9968_t *v)   { return (uint32_t)(v->regs[6] & 0x7F) << 11; }

/* ------------------------------------------------------------------ */
/* Text modes                                                          */
/* ------------------------------------------------------------------ */

static void render_text1(const line_t *l, uint16_t *c) {
    const v9968_t *v = l->v;
    uint32_t nb = name_base(v), pg = pgen_base(v);
    unsigned row = l->py >> 3, sub = l->py & 7;
    uint16_t fg = v->palette[v->regs[7] >> 4], bg = v->palette[v->regs[7] & 15];
    for (unsigned col = 0; col < 40; col++) {
        uint8_t ch = rd(l, nb | ((row * 40 + col) & 0x3FF));
        uint8_t p = rd(l, pg | ((uint32_t)ch << 3) | sub);
        uint16_t *o = c + 9 + col * 6;            /* 9 dots of border, as the V9938 */
        for (unsigned i = 0; i < 6; i++) o[i] = (p & (0x80 >> i)) ? fg : bg;
    }
}

static void render_text2(const line_t *l, uint16_t *c) {
    const v9968_t *v = l->v;
    uint32_t nb = name_base(v), pg = pgen_base(v), cb = color_base(v);
    unsigned row = l->py >> 3, sub = l->py & 7;
    bool blink = !v->blink_page;
    uint16_t fg = v->palette[v->regs[7] >> 4], bg = v->palette[v->regs[7] & 15];
    uint16_t bfg = v->palette[v->regs[12] >> 4], bbg = v->palette[v->regs[12] & 15];
    uint32_t nmask = ((uint32_t)(v->regs[2] & 3) << 10) | 0x3FF;
    uint32_t cmask = ((uint32_t)(v->regs[3] & 7) << 6) | 0x3F;
    for (unsigned col = 0; col < 80; col++) {
        uint32_t pos = row * 80 + col;
        uint8_t ch = rd(l, (nb & ~0xFFFu) | (pos & nmask));
        uint8_t p = rd(l, pg | ((uint32_t)ch << 3) | sub);
        bool on = false;
        if (blink) {
            uint8_t attr = rd(l, (cb & ~0x1FFu) | ((pos >> 3) & cmask));
            on = (attr >> (7 - (pos & 7))) & 1;
        }
        uint16_t f = on ? bfg : fg, b = on ? bbg : bg;
        uint16_t *o = c + 18 + col * 6;
        for (unsigned i = 0; i < 6; i++) o[i] = (p & (0x80 >> i)) ? f : b;
    }
}

/* ------------------------------------------------------------------ */
/* Pattern modes (GRAPHIC1-3, MULTICOLOR)                              */
/* ------------------------------------------------------------------ */

static void render_pattern(const line_t *l, unsigned n) {
    const v9968_t *v = l->v;
    uint32_t nb = name_base(v), pg = pgen_base(v), cb = color_base(v);
    unsigned py = l->py;
    unsigned x = l->x0;
    for (unsigned i = 0; i < n; ) {
        unsigned col = (x >> 3) & 31;
        uint8_t ch = rd(l, nb | ((py >> 3) << 5) | col);
        uint8_t pat, color;
        if (l->mode == V9968_MODE_MC) {
            color = rd(l, pg | ((uint32_t)ch << 3) | ((py >> 2) & 7));
            pat = 0xF0;
        } else if (l->mode == V9968_MODE_G1) {
            pat = rd(l, pg | ((uint32_t)ch << 3) | (py & 7));
            color = rd(l, cb | (ch >> 3));
        } else {
            uint32_t third = (py >> 6) & 3;
            pat = rd(l, (pg & ~0x1FFFu) | ((third & (pg >> 11)) << 11) |
                        ((uint32_t)ch << 3) | (py & 7));
            uint32_t ca = (cb & ~0x1FFFu) | ((third & (cb >> 11)) << 11) |
                          ((uint32_t)(ch >> 3) & (cb >> 6) & 31) << 6 |
                          (uint32_t)(ch & 7) << 3 | (py & 7);
            color = rd(l, ca);
        }
        for (unsigned b = x & 7; b < 8 && i < n; b++, i++, x++) {
            uint8_t c = (pat & (0x80 >> b)) ? (color >> 4) : (color & 15);
            bg_idx[i] = c;
            bg_zero[i] = c == 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bitmap modes (GRAPHIC4-7)                                           */
/* ------------------------------------------------------------------ */

/* Line address of a bitmap row (vdp_timing_control_screen_mode.v). */
static uint32_t bitmap_row(const line_t *l, unsigned page_x8) {
    const v9968_t *v = l->v;
    uint32_t base = name_base(v);
    bool fil = v->regs[20] & 0x20;
    bool eo = v->regs[9] & 0x04;
    unsigned page = (unsigned)(base >> 15) & 1;
    if ((v->regs[25] & 0x01) && page) page = page_x8;              /* SP2 */
    if (eo) page &= (unsigned)(v->blink_page && l->field);
    unsigned py = l->py;
    bool g67 = l->mode == V9968_MODE_G6 || l->mode == V9968_MODE_G7;
    if (g67) {
        if (fil)
            return ((base >> 16) & 1) << 17 | ((((base >> 10) & 0x3F) & (py >> 2)) << 11) |
                   (py & 3) << 9 | (l->field & 1) << 8;
        return ((base >> 16) & 1) << 17 | page << 16 |
               ((((base >> 10) & 0x1F) & (py >> 3)) << 11) | (py & 7) << 8;
    }
    if (fil)
        return ((base >> 16) & 3) << 16 | ((((base >> 10) & 0x3F) & (py >> 2)) << 10) |
               (py & 3) << 8 | (l->field & 1) << 7;
    return ((base >> 16) & 3) << 16 | page << 15 |
           ((((base >> 10) & 0x1F) & (py >> 3)) << 10) | (py & 7) << 7;
}

static void render_bitmap(const line_t *l, unsigned n) {
    unsigned x = l->x0;
    for (unsigned i = 0; i < n; ) {
        unsigned page_x8 = (x >> 8) & 1;
        uint32_t row = bitmap_row(l, page_x8);
        unsigned xx = x & 0xFF;
        unsigned run = 256 - xx;
        if (run > n - i) run = n - i;
        switch (l->mode) {
        case V9968_MODE_G4:
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t b = rd(l, row | (xx >> 1));
                uint8_t c = (xx & 1) ? (b & 15) : (b >> 4);
                bg_idx[i + k] = c;
                bg_zero[i + k] = c == 0;
            }
            break;
        case V9968_MODE_G5:
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t b = rd(l, row | (xx >> 1));
                uint8_t c0 = (xx & 1) ? ((b >> 2) & 3) : (b >> 6);
                uint8_t c1 = (xx & 1) ? (b & 3) : ((b >> 4) & 3);
                bg_idx[2 * (i + k)] = c0;     bg_zero[2 * (i + k)] = c0 == 0;
                bg_idx[2 * (i + k) + 1] = c1; bg_zero[2 * (i + k) + 1] = c1 == 0;
            }
            break;
        case V9968_MODE_G6:
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t b = rd(l, row | xx);
                bg_idx[2 * (i + k)] = b >> 4;     bg_zero[2 * (i + k)] = (b >> 4) == 0;
                bg_idx[2 * (i + k) + 1] = b & 15; bg_zero[2 * (i + k) + 1] = (b & 15) == 0;
            }
            break;
        default:                                    /* G7: raw bytes */
            for (unsigned k = 0; k < run; k++, xx++)
                bg_idx[i + k] = rd(l, row | xx);
            break;
        }
        i += run;
        x += run;
    }
}

/* YJK / YJK + palette on raw GRAPHIC7 bytes, 4-dot groups. */
static void yjk_to_c15(const v9968_t *v, unsigned start, unsigned n, uint16_t *c) {
    bool yae = v->regs[25] & 0x10;
    for (unsigned g = 0; g < n; g += 4) {
        const uint8_t *p = &bg_idx[g];
        int k = (int)((p[1] & 7) << 3 | (p[0] & 7));
        int j = (int)((p[3] & 7) << 3 | (p[2] & 7));
        if (k & 0x20) k -= 64;
        if (j & 0x20) j -= 64;
        for (unsigned i = 0; i < 4 && g + i < n; i++) {
            uint8_t b = p[i];
            if (yae && (b & 0x08)) {
                c[start + g + i] = v->palette[b >> 4];
                continue;
            }
            int y = yae ? (b >> 4) << 1 : b >> 3;
            int r = y + j, gg = y + k;
            int bl = (5 * y - (2 * j + k) + 2) >> 2;
            r = r < 0 ? 0 : r > 31 ? 31 : r;
            gg = gg < 0 ? 0 : gg > 31 ? 31 : gg;
            bl = bl < 0 ? 0 : bl > 31 ? 31 : bl;
            c[start + g + i] = (uint16_t)((r << 10) | (gg << 5) | bl);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sprites                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t plane;
    uint8_t row;              /* mode 1/2: pattern row; mode 3: offset y */
    uint8_t attr[8];
} sel_t;

/* vdp_sprite_divide_table.v: dot of a scaled sprite (16 << bs dots, shown
 * `mg` dots wide or high; mg = 0 means 256). */
static unsigned divide(unsigned x, unsigned mg, unsigned bs) {
    unsigned mul, exp;
    if (mg == 0) {
        mul = x << 3;
        exp = 7;
    } else {
        exp = 7;
        while (!(mg & (1u << exp))) exp--;
        unsigned sel = (mg << (7 - exp)) & 0x7F;
        unsigned coeff = 65536u / (sel + 128u);
        mul = ((coeff * x) >> 5) & 0x1FFF;
    }
    uint32_t sh;
    switch (bs) {
    case 0:  sh = mul; break;
    case 1:  sh = (mul & 0xFFF) << 1; break;
    case 2:  sh = mul << 2; break;
    default: sh = mul << 3; break;
    }
    uint32_t s = (sh & 0xFFFF) >> exp;
    bool over = (s >> 7) != 0 || (bs == 0 && (s & 0x70)) || (bs == 1 && (s & 0x60)) ||
                (bs == 2 && (s & 0x40));
    return over ? (16u << bs) - 1u : (s & 0x7F);
}

static void sprite_overflow(v9968_t *v, unsigned plane) {
    if (v->int_flags & V9968_INT_FRAME) return;         /* sprite_overmap_enable */
    if (v->sprite_status & V9968_S0_5S) return;
    uint8_t id = (plane & 0x20) ? 0x1F : (plane & 0x1F);
    uint8_t s = v->sprite_status;
    __atomic_store_n(&v->sprite_status, (uint8_t)((s & V9968_S0_C) | V9968_S0_5S | id),
                     __ATOMIC_RELAXED);
}

static void sprite_collision(v9968_t *v, unsigned x, unsigned py) {
    if (v->sprite_status & V9968_S0_C) return;
    V9968_SET_BITS(v->sprite_status, V9968_S0_C);
    v->col_x = (uint16_t)((x + 12) & 0x1FF);
    v->col_y = (uint16_t)((py + 8) & 0x3FF);
}

static void render_sprites(const line_t *l, bool *any) {
    v9968_t *v = l->v;
    bool sp3 = v9968_sp3(v);
    bool mode2 = l->mode == V9968_MODE_G3 || l->mode == V9968_MODE_G4 ||
                 l->mode == V9968_MODE_G5 || l->mode == V9968_MODE_G6 ||
                 l->mode == V9968_MODE_G7;
    bool shuffle = v->regs[25] & 0x80;
    bool s16 = v->regs[20] & 0x80;
    bool mag = v->regs[1] & 0x01, si = v->regs[1] & 0x02;
    bool tp = v->regs[8] & 0x20;
    uint32_t sat = sat_base(v), spg = spg_base(v);

    /* selection happens one line ahead, on the previous line's position */
    unsigned prev = (v->regs[20] & 0x02) ? ((l->y - 1) & 0x1FF)
                                         : ((l->y - 1 + v->regs[23]) & 0xFF);
    int32_t py9 = (prev & 0x100) ? (int32_t)prev - 0x200 : (int32_t)prev;

    unsigned limit = (sp3 || s16) ? 16 : mode2 ? 8 : 4;
    unsigned planes = sp3 ? 64 : 32;
    unsigned finish_y = (sp3 || mode2) ? 216 : 208;
    sel_t sel[16];
    unsigned nsel = 0;

    for (unsigned k = 0; k < planes; k++) {
        unsigned plane;
        if (!shuffle)         plane = k;
        else if (l->il)       plane = (v->shuffle_start + 38 * (k >> 1) + (k & 1)) & 63;
        else                  plane = (v->shuffle_start + 19 * k) & 63;
        if (!sp3) plane &= 31;
        uint8_t a[8];
        uint32_t aa;
        if (sp3)
            aa = (sat & ~0x1FFu) | (((sat >> 7) & 3 & (plane >> 4)) << 7) | ((plane & 15) << 3);
        else if (mode2)
            aa = (sat & ~0x1FFu) | (plane << 2);
        else
            aa = (sat & ~0x7Fu) | (plane << 2);
        unsigned na = sp3 ? 8 : 4;
        for (unsigned i = 0; i < na; i++) a[i] = rd(l, aa + i);

        bool visible;
        unsigned row;
        unsigned y;
        if (sp3) {
            y = (a[0] | (a[1] << 8)) & 0x3FF;
            if (y == finish_y && !shuffle) break;
            uint32_t off = (uint32_t)(py9 - (int32_t)y) & 0x3FF;
            unsigned mgy = a[2];
            visible = (mgy == 0) ? (off >> 8) == 0 : off < mgy;
            row = off & 0xFF;
        } else {
            y = a[0];
            if (y == finish_y && !shuffle) break;
            uint32_t off = (uint32_t)(py9 - (int32_t)y) & 0x3FF;
            unsigned size_shift = (si ? 4 : 3) + (mag ? 1 : 0);
            visible = ((off & 0xFF) >> size_shift) == 0;
            row = mag ? (off >> 1) & 15 : off & 15;
        }
        if (!visible) continue;
        if (nsel >= limit) {
            sprite_overflow(v, plane);
            break;
        }
        sel[nsel].plane = (uint8_t)plane;
        sel[nsel].row = (uint8_t)row;
        memcpy(sel[nsel].attr, a, 8);
        nsel++;
    }
    if (nsel == 0) return;

    memset(spr_on, 0, sizeof(spr_on));
    memset(spr_fix, 0, sizeof(spr_fix));
    bool cc0_found = false;
    unsigned ho = l->ho_l;

    for (unsigned s = 0; s < nsel; s++) {
        const sel_t *p = &sel[s];
        const uint8_t *a = p->attr;
        if (sp3) {
            unsigned bs = a[1] >> 6, mgy = a[2], ctl = a[3];
            unsigned x0 = (a[4] | (a[5] << 8)) & 0x3FF, page = (a[5] >> 4) & 7;
            unsigned mgx = a[6], pat = a[7];
            unsigned sy = divide(p->row, mgy, bs);
            if (ctl & 0x20) sy = ~sy & 0x7F;
            sy &= (16u << bs) - 1u;
            uint32_t pa = ((((pat >> 4) << 8) | (pat & 15)) + ((page << 12) | (sy << 4))) & 0x7FFF;
            uint32_t addr = (spg + (pa << 3)) & V9968_VRAM_MASK;
            uint8_t pb[8];
            for (unsigned i = 0; i < 8; i++) pb[i] = v->vram[(addr + i) & V9968_VRAM_MASK];
            for (unsigned i = 0; i < mgx; i++) {
                unsigned sx = (x0 + i) & 0x3FF;
                if (sx >= 256) continue;
                unsigned d = divide(i, mgx, 0);
                if (ctl & 0x10) d = ~d & 15;
                uint8_t nib = (d & 1) ? (pb[d >> 1] & 15) : (pb[d >> 1] >> 4);
                if (!nib) continue;
                if (spr_on[sx]) {
                    if (spr_col[sx] & 15) sprite_collision(v, sx + ho, l->py);
                    continue;
                }
                spr_on[sx] = 1;
                spr_col[sx] = (uint8_t)(((ctl & 15) << 4) | nib);
                spr_tr[sx] = (uint8_t)(ctl >> 6);
            }
            continue;
        }

        /* mode 1/2 */
        uint8_t color;
        if (mode2) {
            uint32_t ca = (sat & ~0x3FFu) | (((sat >> 7) & 3 & (p->plane >> 3)) << 7) |
                          ((p->plane & 7) << 4) | p->row;
            color = rd(l, ca);
        } else {
            color = a[3];
        }
        unsigned set = (mode2 && v9968_epal(v)) ? (a[3] & 15) : 0;
        bool cc = color & 0x40, ic = color & 0x20;
        uint8_t c = color & 15;
        unsigned pat = a[2];
        uint16_t bits;
        if (si) {
            uint32_t pa = spg | ((uint32_t)(pat & 0xFC) << 3) | (p->row & 15);
            bits = (uint16_t)(rd(l, pa) << 8 | rd(l, pa + 16));
        } else {
            bits = (uint16_t)(rd(l, spg | ((uint32_t)pat << 3) | (p->row & 7)) << 8);
        }
        int x0 = (int)a[1] - ((color & 0x80) ? 32 : 0);
        unsigned w = (si ? 16u : 8u) << (mag ? 1 : 0);
        bool first = (s == 0);
        bool cc0_before = cc0_found;

        for (unsigned px = 0; px < 256; px++) {
            int off = (int)px - x0;
            bool dot = off >= 0 && (unsigned)off < w &&
                       (bits & (0x8000 >> (mag ? off >> 1 : off)));
            bool opaque = dot && (c != 0 || tp);
            if (dot && !first && spr_on[px] && !ic && !cc &&
                ((spr_col[px] & 15) != 0 || tp))
                sprite_collision(v, px + ho, l->py);
            if (first) {
                if (!cc && opaque) { spr_on[px] = 1; spr_col[px] = (uint8_t)(set << 4 | c); }
                continue;
            }
            if (spr_fix[px]) continue;
            if (!cc0_before) {
                if (!cc && opaque) { spr_on[px] = 1; spr_col[px] = (uint8_t)(set << 4 | c); }
            } else if (opaque) {
                if (spr_on[px]) {
                    if (cc) spr_col[px] |= c;
                    else    spr_fix[px] = 1;
                } else {
                    spr_on[px] = 1;
                    spr_col[px] = (uint8_t)(set << 4 | c);
                }
            } else if (spr_on[px] && !cc) {
                spr_fix[px] = 1;
            }
        }
        if (!cc) cc0_found = true;
    }
    *any = true;
}

/* ------------------------------------------------------------------ */
/* Composition                                                         */
/* ------------------------------------------------------------------ */

static void scale_out(const v9968_t *v, const uint16_t *src, unsigned n,
                      uint16_t bl, uint16_t br, uint16_t *out, unsigned out_w) {
    /* 256 dots -> 2 pixels each, 512 -> 1 pixel each, centred */
    unsigned pic = (n == 256) ? 512 : n;
    if (pic > out_w) pic = out_w;
    unsigned left = (out_w - pic) / 2;
    for (unsigned i = 0; i < left; i++) out[i] = (i & 1) ? br : bl;
    if (n == 256) {
        for (unsigned i = 0; i < pic / 2; i++) {
            uint16_t c = c15_to_565(v->gamma, src[i]);
            out[left + 2 * i] = c;
            out[left + 2 * i + 1] = c;
        }
    } else {
        for (unsigned i = 0; i < pic; i++) out[left + i] = c15_to_565(v->gamma, src[i]);
    }
    for (unsigned i = left + pic; i < out_w; i++) out[i] = (i & 1) ? br : bl;
}

V9968_HOT void v9968_render_line(v9968_t *v, unsigned line, uint16_t *out, unsigned out_w) {
    line_t l;
    l.v = v;
    l.mode = v9968_mode(v);
    l.il = v9968_interleave(v);
    unsigned h = v9968_lines_per_field(v);
    bool progressive_il = v9968_display_height(v) > h;
    if (progressive_il) { l.y = line >> 1; l.field = line & 1; }
    else                { l.y = line;      l.field = v->field; }

    /* display adjust (vertical): positive values move the picture up */
    int adj = (v->regs[18] >> 4) & 15;
    if (adj & 8) adj -= 16;
    int yy = (int)l.y + adj;

    uint16_t bl = border_c15(v, l.mode, 0), br = border_c15(v, l.mode, 1);
    bool hires = l.mode == V9968_MODE_G5 || l.mode == V9968_MODE_G6 || l.mode == V9968_MODE_T2;
    unsigned n = hires ? 512 : 256;
    uint16_t *c = line_c15;
    unsigned bl565 = c15_to_565(v->gamma, bl), br565 = c15_to_565(v->gamma, br);

    if (!(v->regs[1] & 0x40) || yy < 0 || yy >= (int)h || l.mode == V9968_MODE_NONE) {
        for (unsigned i = 0; i < out_w; i++) out[i] = (uint16_t)((i & 1) ? br565 : bl565);
        return;
    }
    l.y = (unsigned)yy;
    l.py = (l.y + v->regs[23]) & 0xFF;
    l.hires = hires;
    bool text = l.mode == V9968_MODE_T1 || l.mode == V9968_MODE_T2;
    l.ho_l = text ? 0 : (v->regs[27] & 7);
    l.x0 = text ? 0 : ((unsigned)(v->regs[26] & 0x3F) << 3);

    for (unsigned i = 0; i < n; i++) c[i] = hires ? ((i & 1) ? br : bl) : bl;

    if (text) {
        if (l.mode == V9968_MODE_T1) render_text1(&l, c);
        else                         render_text2(&l, c);
        scale_out(v, c, n, (uint16_t)bl565, (uint16_t)br565, out, out_w);
        return;
    }

    /* background, in source dots from ho_l on */
    unsigned dots = 256 - l.ho_l;
    bool tp = v->regs[8] & 0x20;
    bool fixed = g7_fixed(v, l.mode);
    if (l.mode <= V9968_MODE_G3 || l.mode == V9968_MODE_MC) render_pattern(&l, dots);
    else                                                   render_bitmap(&l, dots);

    unsigned sub = hires ? 2 : 1;
    unsigned first = l.ho_l * sub, count = dots * sub;
    if (l.mode == V9968_MODE_G7) {
        if (v->regs[25] & 0x08)      yjk_to_c15(v, first, count, c);
        else if (v9968_epal(v))      for (unsigned i = 0; i < count; i++) c[first + i] = v->palette[bg_idx[i]];
        else                         for (unsigned i = 0; i < count; i++) c[first + i] = g7_c15(bg_idx[i]);
    } else {
        for (unsigned i = 0; i < count; i++) {
            if (bg_zero[i] && !tp)
                c[first + i] = (hires && ((first + i) & 1)) ? br : bl;
            else
                c[first + i] = v->palette[bg_idx[i]];
        }
    }

    /* sprites */
    bool any = false;
    if (!(v->regs[8] & 0x02)) render_sprites(&l, &any);
    if (any) {
        bool sp3 = v9968_sp3(v);
        for (unsigned sx = 0; sx + l.ho_l < 256; sx++) {
            if (!spr_on[sx]) continue;
            unsigned x = sx + l.ho_l;
            uint8_t col = spr_col[sx];
            if (fixed) {
                c[x] = g7_c15(g7_sprite[col & 15]);
            } else if (l.mode == V9968_MODE_G5) {
                uint16_t c0 = v->palette[(col & 0xF0) | ((col >> 2) & 3)];
                uint16_t c1 = v->palette[(col & 0xF0) | (col & 3)];
                c[2 * x]     = sp3 ? mix_c15(c[2 * x], c0, spr_tr[sx]) : c0;
                c[2 * x + 1] = sp3 ? mix_c15(c[2 * x + 1], c1, spr_tr[sx]) : c1;
            } else if (hires) {
                uint16_t sc = v->palette[col];
                c[2 * x]     = sp3 ? mix_c15(c[2 * x], sc, spr_tr[sx]) : sc;
                c[2 * x + 1] = sp3 ? mix_c15(c[2 * x + 1], sc, spr_tr[sx]) : sc;
            } else {
                uint16_t sc = v->palette[col];
                c[x] = sp3 ? mix_c15(c[x], sc, spr_tr[sx]) : sc;
            }
        }
    }

    /* left mask (R#25 MSK): the first 8 dots show the border */
    if (v->regs[25] & 0x02)
        for (unsigned i = 0; i < 8 * sub; i++) c[i] = (hires && (i & 1)) ? br : bl;

    scale_out(v, c, n, (uint16_t)bl565, (uint16_t)br565, out, out_w);
}
