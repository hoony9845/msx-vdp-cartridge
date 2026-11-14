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
 *
 * Follows HRA!'s V9968 RTL (Copyright (C) 2025 Takayuki Hara) and is
 * distributed under its conditions, including the non-commercial clause;
 * see THIRD_PARTY_NOTICES.md.
 */
#include <string.h>
#include "v9968.h"

#define LINE_MAX 512

/* Optional stage timing (the firmware defines V9968_PROF and the clock):
 * 0 background, 1 colours, 2 sprites, 3 output; inside 2: 4 sprite
 * selection, 5 sprite drawing (the rest is composition). */
#ifdef V9968_PROF
uint32_t v9968_prof_now(void);
volatile uint32_t v9968_prof_us[6];
#define PROF_MARK(i) do { uint32_t t_ = v9968_prof_now(); v9968_prof_us[i] += t_ - prof_t; prof_t = t_; } while (0)
#define PROF_START() uint32_t prof_t = v9968_prof_now()
#define PROF_SUB(i, t0) do { uint32_t t_ = v9968_prof_now(); v9968_prof_us[i] += t_ - (t0); (t0) = t_; } while (0)
#else
#define PROF_MARK(i) do { } while (0)
#define PROF_START() do { } while (0)
#define PROF_SUB(i, t0) do { (void)(t0); } while (0)
#endif

static uint16_t line_c15[LINE_MAX];       /* composed line, 0RRRRRGGGGGBBBBB */
static uint8_t  bg_idx[LINE_MAX];         /* palette modes: palette index */

/* sprites, 256-dot resolution: one word per dot, 0 where no sprite */
#define SPR_ON 0x8000u                    /* | mode 3 transparency << 8 | {palette set, colour} */
static uint16_t spr_px[256];
static uint8_t  spr_fix[256];
static unsigned spr_lo, spr_hi;           /* dots with a sprite lie in [lo, hi] */

#ifdef V9968_PROF
/* sprites drawn, sprite dots, dots composed, mixes, sprites selected */
volatile uint32_t v9968_prof_cnt[5];
/* CPU cycles (bench): clearing + start of the draw, mode 3 dot runs, composing */
volatile uint32_t v9968_prof_cyc[3];
uint32_t v9968_prof_cycles(void);
#define PROF_CNT(i, n) (v9968_prof_cnt[i] += (n))
#define PROF_CYC_START() uint32_t cyc_t = v9968_prof_cycles()
#define PROF_CYC(i) do { uint32_t c_ = v9968_prof_cycles(); v9968_prof_cyc[i] += c_ - cyc_t; cyc_t = c_; } while (0)
#define PROF_CYC_SKIP() (cyc_t = v9968_prof_cycles())
#else
#define PROF_CNT(i, n) do { } while (0)
#define PROF_CYC_START() do { } while (0)
#define PROF_CYC(i) do { } while (0)
#define PROF_CYC_SKIP() do { } while (0)
#endif

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

/* mix_c15 with the three channels side by side in one word, 10 or 11 bits
 * apart: each sum stays below 128, so one multiply-add and one shift work
 * for all three (same floor per channel as mix5). */
static inline uint16_t mix_c15_fast(uint16_t bg, uint16_t sp, unsigned t) {
    uint32_t b = (bg & 0x7C1Fu) | ((uint32_t)(bg & 0x03E0u) << 16);
    uint32_t s = (sp & 0x7C1Fu) | ((uint32_t)(sp & 0x03E0u) << 16);
    uint32_t m = ((s * (4u - t) + b * t) >> 2) & 0x03E07C1Fu;
    return (uint16_t)((m & 0x7C1Fu) | (m >> 16));
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
        case V9968_MODE_G4: {
            /* the row's 128 bytes are contiguous (G4 is never interleaved) */
            const uint8_t *src = l->v->vram + v9968_phys(false, row);
            uint8_t *d = bg_idx + i;
            unsigned k = 0;
            if (xx & 1) { d[k++] = src[xx >> 1] & 15; }
            for (; k + 1 < run; k += 2) {
                uint8_t b = src[(xx + k) >> 1];
                d[k] = b >> 4;
                d[k + 1] = b & 15;
            }
            if (k < run) d[k] = src[(xx + k) >> 1] >> 4;
            break;
        }
        case V9968_MODE_G5:
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t b = rd(l, row | (xx >> 1));
                uint8_t c0 = (xx & 1) ? ((b >> 2) & 3) : (b >> 6);
                uint8_t c1 = (xx & 1) ? (b & 3) : ((b >> 4) & 3);
                bg_idx[2 * (i + k)] = c0;
                bg_idx[2 * (i + k) + 1] = c1;
            }
            break;
        case V9968_MODE_G6:
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t b = rd(l, row | xx);
                bg_idx[2 * (i + k)] = b >> 4;
                bg_idx[2 * (i + k) + 1] = b & 15;
            }
            break;
        default: {                                  /* G7: raw bytes */
            /* consecutive dots of one row: one bank for even, one for odd
             * addresses when interleaved, else straight */
            const uint8_t *vr = l->v->vram;
            if (!l->il) {
                const uint8_t *src = vr + v9968_phys(false, row | xx);
                for (unsigned k = 0; k < run; k++) bg_idx[i + k] = src[k];
            } else {
                /* row has its low 8 bits clear: dot xx is at base + xx / 2,
                 * in the odd half (+0x10000) for odd xx */
                const uint8_t *e = vr + ((row & 0x20000u) | ((row >> 1) & 0xFFFFu));
                const uint8_t *o = e + 0x10000u;
                for (unsigned k = 0; k < run; k++, xx++)
                    bg_idx[i + k] = ((xx & 1u) ? o : e)[xx >> 1];
            }
            break;
        }
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
                /* palette dot; colour 0 is transparent like in SCREEN 5 */
                unsigned idx = b >> 4;
                if (idx == 0 && !(v->regs[8] & 0x20)) idx = v->regs[7] & 15;
                c[start + g + i] = v->palette[idx];
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
static uint16_t div_coeff[256];               /* per mg: 65536 / (128 + mantissa) */
static uint8_t  div_exp[256];                 /* per mg: top bit */

/* sprite mode 3 X: the first i (< mg) showing source dot d, d = 0..15, and
 * mg at d = 16 (i.e. divide(i, mg, 0) == d for div_start[d] <= i <
 * div_start[d + 1]; empty when equal) */
static uint8_t div_start[256][17];

static void divide_init(void) {
    if (div_coeff[1]) return;
    for (unsigned mg = 1; mg < 256; mg++) {
        unsigned exp = 7;
        while (!(mg & (1u << exp))) exp--;
        div_exp[mg] = (uint8_t)exp;
        unsigned coeff = 65536u / (((mg << (7 - exp)) & 0x7F) + 128u);
        div_coeff[mg] = (uint16_t)coeff;
        /* d(i) = min(15, coeff * i >> (5 + exp)) never decreases in i
         * (coeff * i < 2^17: the RTL's 13-bit mask never cuts) */
        unsigned d = 0;
        div_start[mg][0] = 0;
        for (unsigned i = 0; i < mg; i++) {
            unsigned di = (coeff * i) >> (5 + exp);
            if (di > 15) di = 15;
            while (d < di) div_start[mg][++d] = (uint8_t)i;
        }
        while (d < 16) div_start[mg][++d] = (uint8_t)mg;
    }
}

static unsigned divide(unsigned x, unsigned mg, unsigned bs) {
    unsigned mul, exp;
    if (mg == 0) {
        mul = x << 3;
        exp = 7;
    } else {
        divide_init();
        exp = div_exp[mg];
        mul = ((div_coeff[mg] * x) >> 5) & 0x1FFF;
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

/* The fast path's output for mode 3 sprites: each dot is composed into `o`
 * (one word per dot, RGB565 twice) as soon as a sprite takes it, which is
 * final since the first sprite to take a dot keeps it. */
typedef struct {
    uint32_t *o;
    const uint16_t *pal, *tbl;      /* palette / G7 colours, RGB565 */
    bool fixed, g7, epal, tp;
    uint16_t bl;                    /* border, 15 bits */
    unsigned ho_l;
} spr_out_t;

/* Mode 3 translucency over the background dot b (as the compose loop of
 * v9968_render_line): s_part is the sprite colour in mix_c15_fast's packed
 * form times 4 - t. */
static inline uint16_t sp3_mix(const v9968_t *v, const spr_out_t *so, unsigned b,
                               uint32_t s_part, unsigned t) {
    uint16_t bgc = so->g7 ? (so->epal ? v->palette[b] : g7_c15((uint8_t)b))
                          : ((b == 0 && !so->tp) ? so->bl : v->palette[b]);
    uint32_t bw = (bgc & 0x7C1Fu) | ((uint32_t)(bgc & 0x03E0u) << 16);
    uint32_t m = ((s_part + bw * t) >> 2) & 0x03E07C1Fu;
    return c15_to_565(v->gamma, (uint16_t)((m & 0x7C1Fu) | (m >> 16)));
}

/* Translucent results by (background index, sprite colour, transparency):
 * entry = valid | tag << 16 | RGB565, index = b | (col & 15) << 8 (the
 * colours of one sprite never share an entry), tag = col >> 4 | t << 4.
 * Emptied when anything sp3_mix reads changes. */
#define MIX_VALID 0x01000000u
static uint32_t mix_cache[4096];
static uint32_t mix_cache_gen = ~0u, mix_cache_ctx = ~0u;

static void mix_cache_check(const v9968_t *v, const spr_out_t *so) {
    uint32_t gen = v->pal_gen;
    uint32_t ctx = (uint32_t)v->gamma | (uint32_t)so->g7 << 1 | (uint32_t)so->epal << 2 |
                   (uint32_t)so->tp << 3 | (uint32_t)so->bl << 4;
    if (gen != mix_cache_gen || ctx != mix_cache_ctx) {
        memset(mix_cache, 0, sizeof(mix_cache));
        mix_cache_gen = gen;
        mix_cache_ctx = ctx;
    }
}

/* Returns true when it composed everything into so->o (mode 3 with `so`). */
static bool render_sprites(const line_t *l, bool *any, const spr_out_t *so) {
    v9968_t *v = l->v;
#ifdef V9968_PROF
    uint32_t sub_t = v9968_prof_now();
#else
    uint32_t sub_t = 0;
#endif
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
        const uint8_t *asrc = v->vram + (aa & V9968_VRAM_MASK);
        if (sp3) {
            /* 8-byte aligned, not interleaved with mode 3: read in place,
             * Y and MGY first, the rest only for a selected sprite */
            a[0] = asrc[0]; a[1] = asrc[1]; a[2] = asrc[2];
        } else {
            for (unsigned i = 0; i < 4; i++) a[i] = rd(l, aa + i);
        }

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
        if (sp3) for (unsigned i = 3; i < 8; i++) a[i] = asrc[i];
        if (nsel >= limit) {
            sprite_overflow(v, plane);
            break;
        }
        sel[nsel].plane = (uint8_t)plane;
        sel[nsel].row = (uint8_t)row;
        memcpy(sel[nsel].attr, a, 8);
        nsel++;
    }
    PROF_SUB(4, sub_t);
    PROF_CNT(4, nsel);
    if (nsel == 0) return false;
    if (!sp3) so = NULL;
    if (so) mix_cache_check(v, so);
    PROF_CYC_START();

    memset(spr_px, 0, sizeof(spr_px));
    if (!sp3) memset(spr_fix, 0, sizeof(spr_fix));
    spr_lo = sp3 ? 255u : 0u;
    spr_hi = sp3 ? 0u : 255u;
    bool cc0_found = false;
    PROF_CYC(0);

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
            if (!mgx) continue;                      /* mgx = 0 draws nothing */
            /* the part on screen: X wraps at 1024, dots 256.. are hidden */
            unsigned ia, ib;
            if (x0 < 256)               { ia = 0;          ib = 256 - x0 < mgx ? 256 - x0 : mgx; }
            else if (x0 + mgx > 1024)   { ia = 1024 - x0;  ib = mgx; }
            else continue;
            /* the row's 16 dots, dot 0 in the top nibble (mirrored if asked) */
            uint32_t r[2] = { 0, 0 };
            for (unsigned i = 0; i < 8; i++)
                r[i >> 2] = r[i >> 2] << 8 | v->vram[(addr + i) & V9968_VRAM_MASK];
            if (!(r[0] | r[1])) continue;
            bool mirror = ctl & 0x10;
            divide_init();
            const uint8_t *bound = div_start[mgx];
            unsigned lo = spr_lo, hi = spr_hi;
            unsigned tr = ctl >> 6;
            uint16_t vbase = (uint16_t)(SPR_ON | tr << 8 | (ctl & 15) << 4);
            /* collisions: only until the first one sets C */
            bool detect = !(v->sprite_status & V9968_S0_C);
            PROF_CNT(0, 1);
            PROF_CYC_SKIP();
            /* Non-transparent source dots d in increasing order (bit 28 - 4k
             * of word k / 8 marks nibble k; mirrored, d = 15 - k), one run of
             * i each (div_start); i runs over on-screen dots only, x0 + i
             * does not wrap inside it. */
            if (so) {
                /* Compose into the output as the dots are taken: per
                 * non-transparent source dot d (in increasing order: bit
                 * 28 - 4k of mask word k / 8 marks nibble k; mirrored, d =
                 * 15 - k) its run of i (div_start), on-screen part only. */
                uint32_t z[2];
                for (unsigned k = 0; k < 2; k++) {
                    uint32_t w = r[k] | r[k] >> 1;
                    z[k] = (w | w >> 2) & 0x11111111u;
                }
                unsigned ho_l = so->ho_l, ndots = 0;
                bool mix = !so->fixed && tr;
                uint32_t *out = so->o;
                uint8_t cb = (uint8_t)((ctl & 15) << 4);
                for (unsigned half = 0; half < 2; half++) {
                    unsigned k = mirror ? 1 - half : half;
                    uint32_t m = z[k];
                    while (m) {
                        unsigned pos = mirror ? (unsigned)__builtin_ctz(m) : 31u - (unsigned)__builtin_clz(m);
                        m &= ~(1u << pos);
                        unsigned e4 = k * 8 + (28 - pos) / 4;
                        unsigned d = mirror ? 15 - e4 : e4;
                        unsigned i = bound[d], e = bound[d + 1];
                        if (i < ia) i = ia;
                        if (e > ib) e = ib;
                        if (i >= e) continue;
                        unsigned nib = (r[k] >> pos) & 15;
                        uint8_t col = (uint8_t)(cb | nib);
                        uint16_t val = (uint16_t)(vbase | nib);
                        uint32_t c32 = so->fixed ? so->tbl[g7_sprite[nib]] * 0x10001u   /* G7 colours */
                                                 : so->pal[col] * 0x10001u;
                        uint32_t tag = MIX_VALID | (uint32_t)((col >> 4) | tr << 4) << 16;
                        unsigned x = (x0 + i) & 0x3FF, xe = x + (e - i);   /* no wrap inside */
                        ndots += e - i;
                        if (mix) PROF_CNT(2, e - i);
                        for (; x < xe; x++) {
                            if (spr_px[x]) {
                                if (detect) {
                                    sprite_collision(v, x, l->py);   /* spr_col & 15 != 0 in mode 3 */
                                    detect = false;
                                }
                                continue;
                            }
                            spr_px[x] = val;
                            if (x < ho_l) continue;                  /* in R#27's border */
                            if (!mix) { out[x] = c32; continue; }
                            unsigned b = bg_idx[x - ho_l];
                            unsigned idx = b | (col & 15u) << 8;
                            uint32_t en = mix_cache[idx];
                            if ((en & 0xFFFF0000u) != tag) {
                                uint16_t sp = v->palette[col];
                                uint32_t s_part = ((sp & 0x7C1Fu) | ((uint32_t)(sp & 0x03E0u) << 16)) * (4u - tr);
                                en = tag | sp3_mix(v, so, b, s_part, tr);
                                mix_cache[idx] = en;
                                PROF_CNT(3, 1);
                            }
                            out[x] = (en & 0xFFFFu) * 0x10001u;
                        }
                    }
                }
                PROF_CNT(1, ndots);
                PROF_CYC(1);
                continue;
            }
            uint32_t z[2];
            for (unsigned k = 0; k < 2; k++) {
                uint32_t w = r[k] | r[k] >> 1;
                z[k] = (w | w >> 2) & 0x11111111u;
            }
            for (unsigned half = 0; half < 2; half++) {
                unsigned k = mirror ? 1 - half : half;
                uint32_t m = z[k];
                while (m) {
                    unsigned pos = mirror ? (unsigned)__builtin_ctz(m) : 31u - (unsigned)__builtin_clz(m);
                    m &= ~(1u << pos);
                    unsigned e4 = k * 8 + (28 - pos) / 4;          /* nibble */
                    unsigned d = mirror ? 15 - e4 : e4;            /* source dot */
                    unsigned nib = (r[k] >> pos) & 15;
                    unsigned i = bound[d], e = bound[d + 1];
                    if (i < ia) i = ia;
                    if (e > ib) e = ib;
                    if (i >= e) continue;
                    uint16_t val = (uint16_t)(vbase | nib);
                    unsigned a0 = (x0 + i) & 0x3FF, a1 = (x0 + e - 1) & 0x3FF;
                    uint16_t *px = spr_px + a0;
                    unsigned cnt = e - i;
                    PROF_CNT(1, cnt);
                    for (unsigned j = 0; j < cnt; j++) {
                        if (px[j]) {
                            if (detect) {
                                sprite_collision(v, a0 + j, l->py);   /* spr_col & 15 != 0 in mode 3 */
                                detect = false;
                            }
                            continue;
                        }
                        px[j] = val;
                    }
                    if (a0 < lo) lo = a0;
                    if (a1 > hi) hi = a1;
                }
            }
            spr_lo = lo;
            spr_hi = hi;
            PROF_CYC(1);
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
            if (dot && !first && spr_px[px] && !ic && !cc &&
                ((spr_px[px] & 15) != 0 || tp))
                sprite_collision(v, px, l->py);
            if (first) {
                if (!cc && opaque) spr_px[px] = (uint16_t)(SPR_ON | set << 4 | c);
                continue;
            }
            if (spr_fix[px]) continue;
            if (!cc0_before) {
                if (!cc && opaque) spr_px[px] = (uint16_t)(SPR_ON | set << 4 | c);
            } else if (opaque) {
                if (spr_px[px]) {
                    if (cc) spr_px[px] |= c;
                    else    spr_fix[px] = 1;
                } else {
                    spr_px[px] = (uint16_t)(SPR_ON | set << 4 | c);
                }
            } else if (spr_px[px] && !cc) {
                spr_fix[px] = 1;
            }
        }
        if (!cc) cc0_found = true;
    }
    PROF_SUB(5, sub_t);
    *any = true;
    return so != NULL;
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

/* ---- 256-dot fast path: straight to RGB565 ----
 * Palette and GRAPHIC7 colours converted once (gamma included), then one
 * lookup per dot; only dots under a translucent sprite go through the
 * 15-bit mix. Same output as the general path (c15 -> c15_to_565). */
static uint16_t pal565[256], g7_565[256];
static uint32_t out32[256];          /* when `out` is not 4-byte aligned */

/* GRAPHIC7 straight to output words (render_bitmap's G7 case fused with
 * the colour lookup); bg_idx is still filled for translucent sprites. */
static void render_g7_fast(const line_t *l, unsigned n, uint32_t *o, const uint16_t *tbl) {
    unsigned x = l->x0;
    const uint8_t *vr = l->v->vram;
    for (unsigned i = 0; i < n; ) {
        uint32_t row = bitmap_row(l, (x >> 8) & 1);
        unsigned xx = x & 0xFF;
        unsigned run = 256 - xx;
        if (run > n - i) run = n - i;
        uint8_t *d = bg_idx + i;
        uint32_t *q = o + i;
        if (!l->il) {
            const uint8_t *src = vr + v9968_phys(false, row | xx);
            for (unsigned k = 0; k < run; k++) { uint8_t c = src[k]; d[k] = c; q[k] = tbl[c] * 0x10001u; }
        } else {
            const uint8_t *e = vr + ((row & 0x20000u) | ((row >> 1) & 0xFFFFu));
            const uint8_t *od = e + 0x10000u;
            for (unsigned k = 0; k < run; k++, xx++) {
                uint8_t c = ((xx & 1u) ? od : e)[xx >> 1];
                d[k] = c;
                q[k] = tbl[c] * 0x10001u;
            }
        }
        i += run;
        x += run;
    }
}

/* GRAPHIC4 straight to output words (render_bitmap's G4 case fused with the
 * colour lookup); bg_idx is still filled for translucent sprites. */
static void render_g4_fast(const line_t *l, unsigned n, uint32_t *o, const uint32_t *t32) {
    unsigned x = l->x0;
    for (unsigned i = 0; i < n; ) {
        uint32_t row = bitmap_row(l, (x >> 8) & 1);
        unsigned xx = x & 0xFF;
        unsigned run = 256 - xx;
        if (run > n - i) run = n - i;
        const uint8_t *src = l->v->vram + v9968_phys(false, row);
        uint8_t *d = bg_idx + i;
        uint32_t *q = o + i;
        unsigned k = 0;
        if (xx & 1) { uint8_t c = src[xx >> 1] & 15; d[0] = c; q[0] = t32[c]; k = 1; }
        const uint8_t *sp = src + ((xx + k) >> 1);
        for (; k + 1 < run; k += 2) {
            uint8_t b = *sp++;
            uint8_t hi = b >> 4, lo = b & 15;
            d[k] = hi; d[k + 1] = lo;
            q[k] = t32[hi]; q[k + 1] = t32[lo];
        }
        if (k < run) { uint8_t c = *sp >> 4; d[k] = c; q[k] = t32[c]; }
        i += run;
        x += run;
    }
}
static uint32_t pal565_gen = ~0u;
static int pal565_gamma = -1, g7_565_gamma = -1;
static const uint16_t *pal565_src;

static const uint16_t *palette_565(const v9968_t *v) {
    if (pal565_src != v->palette || pal565_gen != v->pal_gen || pal565_gamma != (int)v->gamma) {
        uint32_t gen;
        do {                /* a palette write in between: convert again */
            gen = v->pal_gen;
            for (unsigned i = 0; i < 256; i++) pal565[i] = c15_to_565(v->gamma, v->palette[i]);
        } while (gen != v->pal_gen);
        pal565_gen = gen;
        pal565_gamma = v->gamma;
        pal565_src = v->palette;
    }
    return pal565;
}

static const uint16_t *g7_colors_565(const v9968_t *v) {
    if (g7_565_gamma != (int)v->gamma) {
        for (unsigned i = 0; i < 256; i++) g7_565[i] = c15_to_565(v->gamma, g7_c15((uint8_t)i));
        g7_565_gamma = v->gamma;
    }
    return g7_565;
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

    bool fast = !text && !hires && !(l.mode == V9968_MODE_G7 && (v->regs[25] & 0x08));
    if (!fast)
        for (unsigned i = 0; i < n; i++) c[i] = hires ? ((i & 1) ? br : bl) : bl;

    if (text) {
        if (l.mode == V9968_MODE_T1) render_text1(&l, c);
        else                         render_text2(&l, c);
        scale_out(v, c, n, (uint16_t)bl565, (uint16_t)br565, out, out_w);
        return;
    }

    /* background, in source dots from ho_l on */
    PROF_START();
    unsigned dots = 256 - l.ho_l;
    bool tp = v->regs[8] & 0x20;
    bool fixed = g7_fixed(v, l.mode);       /* over YJK sprites use the palette */
    if (l.mode <= V9968_MODE_G3 || l.mode == V9968_MODE_MC) render_pattern(&l, dots);
    else if (!(fast && (l.mode == V9968_MODE_G4 || l.mode == V9968_MODE_G7)))
        render_bitmap(&l, dots);

    PROF_MARK(0);
    unsigned sub = hires ? 2 : 1;
    unsigned first = l.ho_l * sub, count = dots * sub;

    if (fast) {
        /* 256 dots, no YJK: the fast path. One 32-bit word per dot (the dot
         * twice in RGB565), written straight into `out` when aligned. */
        bool g7 = l.mode == V9968_MODE_G7, epal = v9968_epal(v);
        const uint16_t *pal = palette_565(v);
        const uint16_t *tbl = (g7 && !epal) ? g7_colors_565(v) : pal;
        uint32_t bl32 = bl565 * 0x10001u;
        unsigned pic = out_w < 512 ? out_w : 512, left = (out_w - pic) / 2;
        bool direct = pic == 512 && !((uintptr_t)(out + left) & 3u);
        uint32_t *o = direct ? (uint32_t *)(void *)(out + left) : out32;
        for (unsigned i = 0; i < first; i++) o[i] = bl32;
        if (g7) {
            render_g7_fast(&l, dots, o + first, tbl);
        } else {
            /* 16 colours here (bg_idx < 16); colour 0 is the border unless TP */
            uint32_t t32[16];
            for (unsigned i = 0; i < 16; i++) t32[i] = tbl[i] * 0x10001u;
            if (!tp) t32[0] = bl32;
            if (l.mode == V9968_MODE_G4) render_g4_fast(&l, dots, o + first, t32);
            else for (unsigned i = 0; i < count; i++) o[first + i] = t32[bg_idx[i]];
        }
        PROF_MARK(1);

        bool any = false, composed = false;
        if (!(v->regs[8] & 0x02)) {
            spr_out_t so = { o, pal, tbl, fixed, g7, epal, tp, bl, l.ho_l };
            composed = render_sprites(&l, &any, &so);
        }
        if (any && !composed) {
            bool sp3 = v9968_sp3(v);
            unsigned x1 = spr_hi + 1u;
            unsigned mix_key = ~0u;
            uint16_t mix_565 = 0;
            unsigned ncomp = 0, nmix = 0;
            PROF_CYC_START();
            for (unsigned x = spr_lo > l.ho_l ? spr_lo : l.ho_l; x < x1; x++) {
                unsigned sv = spr_px[x];
                if (!sv) continue;
                ncomp++;
                uint8_t col = (uint8_t)sv;
                unsigned tr = (sv >> 8) & 3;
                uint16_t c565;
                if (fixed) {
                    c565 = tbl[g7_sprite[col & 15]];          /* fixed => G7 colours */
                } else if (sp3 && tr) {
                    /* translucent: mix in 15 bits over this dot's background
                     * (the last result is reused for a run of equal dots) */
                    unsigned b = bg_idx[x - l.ho_l];
                    unsigned key = b | (sv & 0x3FF) << 8;
                    if (key != mix_key) {
                        uint16_t bgc = g7 ? (epal ? v->palette[b] : g7_c15((uint8_t)b))
                                          : ((b == 0 && !tp) ? bl : v->palette[b]);
                        mix_565 = c15_to_565(v->gamma, mix_c15_fast(bgc, v->palette[col], tr));
                        mix_key = key;
                        nmix++;
                    }
                    c565 = mix_565;
                } else {
                    c565 = pal[col];
                }
                o[x] = c565 * 0x10001u;
            }
            PROF_CYC(2);
            PROF_CNT(2, ncomp);
            PROF_CNT(3, nmix);
        }
        PROF_MARK(2);

        /* left mask (R#25 MSK): the first 8 dots show the border */
        if (v->regs[25] & 0x02)
            for (unsigned i = 0; i < 8; i++) o[i] = bl32;

        for (unsigned i = 0; i < left; i++) out[i] = (uint16_t)bl565;
        if (!direct)
            for (unsigned i = 0; i < pic / 2; i++)
                out[left + 2 * i] = out[left + 2 * i + 1] = (uint16_t)o[i];
        for (unsigned i = left + pic; i < out_w; i++) out[i] = (uint16_t)bl565;
        PROF_MARK(3);
        return;
    }
    if (l.mode == V9968_MODE_G7) {
        if (v->regs[25] & 0x08) {
            /* J and K need a whole 4-dot group: the YJK picture comes out 4
             * dots late, the first 4 dots show the border (the RTL's delay
             * line; openMSX shows the same) */
            yjk_to_c15(v, first + 4, count - 4, c);
            for (unsigned i = 0; i < 4; i++) c[first + i] = bl;
        }
        else if (v9968_epal(v))      for (unsigned i = 0; i < count; i++) c[first + i] = v->palette[bg_idx[i]];
        else                         for (unsigned i = 0; i < count; i++) c[first + i] = g7_c15(bg_idx[i]);
    } else {
        for (unsigned i = 0; i < count; i++) {
            if (bg_idx[i] == 0 && !tp)
                c[first + i] = (hires && ((first + i) & 1)) ? br : bl;
            else
                c[first + i] = v->palette[bg_idx[i]];
        }
    }

    PROF_MARK(1);
    /* sprites */
    bool any = false;
    if (!(v->regs[8] & 0x02)) render_sprites(&l, &any, NULL);
    if (any) {
        bool sp3 = v9968_sp3(v);
        /* sprites ignore R#27 (vdp_sprite_makeup_pixel gets the unscrolled
         * position) but are hidden in the border it opens at the left; over
         * YJK they come out 4 dots late with the picture (as in openMSX) */
        unsigned delay = (l.mode == V9968_MODE_G7 && (v->regs[25] & 0x08)) ? 4 : 0;
        for (unsigned x = l.ho_l + delay; x < 256; x++) {
            unsigned sx = x - delay;
            unsigned sv = spr_px[sx];
            if (!sv) continue;
            uint8_t col = (uint8_t)sv;
            unsigned tr = (sv >> 8) & 3;
            if (fixed) {
                c[x] = g7_c15(g7_sprite[col & 15]);
            } else if (l.mode == V9968_MODE_G5) {
                uint16_t c0 = v->palette[(col & 0xF0) | ((col >> 2) & 3)];
                uint16_t c1 = v->palette[(col & 0xF0) | (col & 3)];
                c[2 * x]     = sp3 ? mix_c15(c[2 * x], c0, tr) : c0;
                c[2 * x + 1] = sp3 ? mix_c15(c[2 * x + 1], c1, tr) : c1;
            } else if (hires) {
                uint16_t sc = v->palette[col];
                c[2 * x]     = sp3 ? mix_c15(c[2 * x], sc, tr) : sc;
                c[2 * x + 1] = sp3 ? mix_c15(c[2 * x + 1], sc, tr) : sc;
            } else {
                uint16_t sc = v->palette[col];
                c[x] = sp3 ? mix_c15(c[x], sc, tr) : sc;
            }
        }
    }

    PROF_MARK(2);
    /* left mask (R#25 MSK): the first 8 dots show the border */
    if (v->regs[25] & 0x02)
        for (unsigned i = 0; i < 8 * sub; i++) c[i] = (hires && (i & 1)) ? br : bl;

    scale_out(v, c, n, (uint16_t)bl565, (uint16_t)br565, out, out_w);
    PROF_MARK(3);
}
