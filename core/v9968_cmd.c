/*
 * V9968 command engine: the V9938 commands plus LRMM (rotated copy), LFMC and
 * LFMM (font drawing), as implemented by vdp_command.v.
 *
 * The RTL is followed closely, including its quirks, because the cartridge
 * is the reference: one command step reads/writes one pixel at the current
 * counters, decides from those counters whether the command ends, and then
 * advances every counter by the rules of vdp_command.v (all from the old
 * values, like its non-blocking updates). The only intended difference is
 * LMCM, which reads (SX, SY) as on the V9938 (the RTL reads DX and never
 * advances DY, so multi-line LMCM could not work there).
 *
 * Speed: every VRAM access costs clocks of the 85.9MHz engine clock; the
 * caller hands in a budget in the same unit. The compatible-speed waits
 * ({40, 2'b11} clocks per access) and the high-speed figures measured on
 * the RTL (LINE about 7, LRMM about 12 clocks per pixel) are approximated.
 *
 * Follows HRA!'s V9968 RTL (Copyright (C) 2025 Takayuki Hara) and is
 * distributed under its conditions, including the non-commercial clause;
 * see THIRD_PARTY_NOTICES.md.
 */
#include <stddef.h>
#include <string.h>
#include "v9968.h"

enum {
    ST_IDLE = 0,
    ST_POINT, ST_PSET, ST_SRCH, ST_LINE, ST_LMMV, ST_LMMM, ST_LMCM,
    ST_LMCM_WAIT, ST_LMMC, ST_HMMV, ST_HMMM, ST_YMMM, ST_HMMC, ST_LRMM,
    ST_LFMC, ST_LFMC_BIT, ST_LFMM, ST_LFMM_BIT,
};

enum {
    OP_STOP = 0x0, OP_LFMM = 0x1, OP_LFMC = 0x2, OP_LRMM = 0x3,
    OP_POINT = 0x4, OP_PSET = 0x5, OP_SRCH = 0x6, OP_LINE = 0x7,
    OP_LMMV = 0x8, OP_LMMM = 0x9, OP_LMCM = 0xA, OP_LMMC = 0xB,
    OP_HMMV = 0xC, OP_HMMM = 0xD, OP_YMMM = 0xE, OP_HMMC = 0xF,
};

#define COST_READ_HS     4
#define COST_WRITE_HS    3
#define COST_ACCESS_SLOW 167      /* {40, 2'b11} wait + the access itself */

/* ------------------------------------------------------------------ */
/* Per-step context                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    v9968_t *v;
    v9968_cmd_state_t *c;
    v9968_mode_t mode;
    bool g4, g5, g6;            /* address layout (g4 includes FG4) */
    unsigned bpp;               /* 2, 4 or 8 */
    bool interleave;
    bool v256;                  /* V9968 mode: 256KB, 11-bit Y */
    bool hs;
    unsigned cost;
    const volatile uint32_t *watch;     /* v9968_cmd_watch and its value at the start */
    uint32_t watch_val;
} ctx_t;

const volatile uint32_t *v9968_cmd_watch;
bool v9968_cmd_reference;

/* Something else needs core1 (see v9968_cmd_watch), after a minimum run. */
#define YIELD_MIN 32u
static inline bool yield_now(const ctx_t *x, unsigned cost) {
    return x->watch && cost >= YIELD_MIN && *x->watch != x->watch_val;
}

static inline bool arg_maj(const v9968_cmd_state_t *c) { return c->arg & 0x01; }
static inline bool arg_eq(const v9968_cmd_state_t *c)  { return c->arg & 0x02; }
static inline bool arg_dix(const v9968_cmd_state_t *c) { return c->arg & 0x04; }
static inline bool arg_diy(const v9968_cmd_state_t *c) { return c->arg & 0x08; }
static inline bool arg_xhr(const ctx_t *x) { return !v9968_v58(x->v) && (x->c->arg & 0x40); }
static inline bool arg_fg4(const v9968_t *v, const v9968_cmd_state_t *c) {
    return !v9968_v58(v) && (c->arg & 0x80);
}

static void make_ctx(ctx_t *x, v9968_t *v) {
    x->v = v;
    x->c = &v->cmd;
    x->mode = v9968_mode(v);
    bool fg4 = arg_fg4(v, &v->cmd);
    x->g4 = fg4 || x->mode == V9968_MODE_G4;
    x->g5 = !x->g4 && x->mode == V9968_MODE_G5;
    x->g6 = !x->g4 && x->mode == V9968_MODE_G6;
    /* vdp_command.v takes the pixel size from the screen mode alone */
    x->bpp = (x->mode == V9968_MODE_G4 || x->mode == V9968_MODE_G6) ? 4 :
             (x->mode == V9968_MODE_G5) ? 2 : 8;
    x->interleave = v9968_interleave(v);
    x->v256 = !v9968_v58(v);
    x->hs = (v->regs[20] & 0x01) != 0;
    x->cost = 0;
    x->watch = v9968_cmd_watch;
    x->watch_val = x->watch ? *x->watch : 0;
}

/* ------------------------------------------------------------------ */
/* VRAM access                                                         */
/* ------------------------------------------------------------------ */

static inline uint32_t sx_int(const v9968_cmd_state_t *c) { return (c->sx >> 8) & 0xFFFu; }
static inline uint32_t sy_int(const v9968_cmd_state_t *c) { return (c->sy >> 8) & 0x1FFFu; }

/* (SX, SY): 12.8 / 13.8 fixed point, as w_address_s */
static uint32_t addr_s(const ctx_t *x, uint32_t sx, uint32_t sy) {
    uint32_t a;
    if (x->g4)      a = ((sy >> 8) & 0x7FFu) << 7 | ((sx >> 9) & 0x7Fu);
    else if (x->g5) a = ((sy >> 8) & 0x7FFu) << 7 | ((sx >> 10) & 0x7Fu);
    else if (x->g6) a = ((sy >> 8) & 0x3FFu) << 8 | ((sx >> 9) & 0xFFu);
    else            a = ((sy >> 8) & 0x3FFu) << 8 | ((sx >> 8) & 0xFFu);
    return v9968_phys(x->interleave, a);
}

static uint32_t addr_d(const ctx_t *x, uint32_t dx, uint32_t dy) {
    uint32_t a;
    if (x->g4)      a = (dy & 0x7FFu) << 7 | ((dx >> 1) & 0x7Fu);
    else if (x->g5) a = (dy & 0x7FFu) << 7 | ((dx >> 2) & 0x7Fu);
    else if (x->g6) a = (dy & 0x3FFu) << 8 | ((dx >> 1) & 0xFFu);
    else            a = (dy & 0x3FFu) << 8 | (dx & 0xFFu);
    return v9968_phys(x->interleave, a);
}

static inline uint8_t vram_read(ctx_t *x, uint32_t a) {
    x->cost += x->hs ? COST_READ_HS : COST_ACCESS_SLOW;
    return x->v->vram[a];
}

static inline void vram_write(ctx_t *x, uint32_t a, uint8_t d) {
    x->cost += x->hs ? COST_WRITE_HS : COST_ACCESS_SLOW;
    x->v->vram[a] = d;
}

static uint8_t pixel_of(const ctx_t *x, uint8_t byte, unsigned xsel) {
    switch (x->bpp) {
    case 4:  return (xsel & 1) ? (byte & 0x0F) : (byte >> 4);
    case 2:  return (byte >> (6 - 2 * (xsel & 3))) & 3;
    default: return byte;
    }
}

static uint8_t merge(const ctx_t *x, unsigned dx, uint8_t pix, uint8_t base) {
    switch (x->bpp) {
    case 4:
        return (dx & 1) ? (uint8_t)((base & 0xF0) | (pix & 0x0F))
                        : (uint8_t)((base & 0x0F) | (pix << 4));
    case 2: {
        unsigned sh = 6 - 2 * (dx & 3);
        return (uint8_t)((base & ~(3u << sh)) | ((pix & 3u) << sh));
    }
    default:
        return pix;
    }
}

/* func_lop: on full bytes; TIMP and friends test all 8 source bits */
static inline __attribute__((always_inline)) uint8_t lop(unsigned op, uint8_t src, uint8_t dst) {
    if ((op & 8) && src == 0) return dst;
    switch (op & 7) {
    case 1:  return src & dst;
    case 2:  return src | dst;
    case 3:  return src ^ dst;
    case 4:  return (uint8_t)~src;
    default: return src;
    }
}

/* Read-modify-write of the destination pixel with the logical operation. */
static void put_pixel(ctx_t *x, uint8_t src) {
    v9968_cmd_state_t *c = x->c;
    uint32_t a = addr_d(x, c->dx, c->dy);
    uint8_t base = vram_read(x, a);
    uint8_t dst = pixel_of(x, base, c->dx & 3);
    vram_write(x, a, merge(x, c->dx & 3, lop(c->lop, src, dst), base));
}

static uint8_t get_source_pixel(ctx_t *x) {
    v9968_cmd_state_t *c = x->c;
    return pixel_of(x, vram_read(x, addr_s(x, c->sx, c->sy)), (c->sx >> 8) & 3);
}

/* ------------------------------------------------------------------ */
/* Counters                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t next_sx, next_sy, next_dx, next_dy;
    bool sx_ovf, dx_ovf, sy_ovf, dy_ovf;
    bool nx_end, ny_end, line_end;
} cond_t;

static bool sx_active(unsigned op) {
    return op == OP_LMMM || op == OP_HMMM || op == OP_YMMM || op == OP_LMCM || op == OP_SRCH;
}
static bool sy_active(unsigned op) {
    return op == OP_LMMM || op == OP_HMMM || op == OP_YMMM || op == OP_LMCM || op == OP_LRMM;
}
static bool dxy_active(unsigned op) {
    return op != OP_LMCM && op != OP_SRCH && op != OP_PSET && op != OP_POINT && op != OP_STOP;
}

static void conditions(const ctx_t *x, cond_t *k) {
    const v9968_cmd_state_t *c = x->c;
    unsigned step = c->pixel_step;
    bool dix = arg_dix(c), diy = arg_diy(c);

    k->next_sx = (dix ? sx_int(c) - step : sx_int(c) + step) & 0xFFFu;
    k->next_sy = (diy ? sy_int(c) - 1u : sy_int(c) + 1u) & 0x1FFFu;
    k->next_dx = (dix ? c->dx - step : c->dx + step) & 0x3FFu;
    k->next_dy = (diy ? c->dy - 1u : c->dy + 1u) & 0x1FFFu;

    k->sx_ovf = sx_active(c->op) &&
                ((k->next_sx & 0x200) || (!c->wide && (k->next_sx & 0x100)));
    k->dx_ovf = dxy_active(c->op) &&
                ((k->next_dx & 0x200) || (!c->wide && (k->next_dx & 0x100)));
    k->sy_ovf = sy_active(c->op) && diy && sy_int(c) == 0;
    k->dy_ovf = (k->next_dy & 0x1000) != 0;

    unsigned nx_max = (step == 4) ? (c->reg_nx & ~3u) :
                      (step == 2) ? (c->reg_nx & ~1u) : c->reg_nx;
    k->nx_end = c->nx == nx_max && c->op != OP_YMMM;
    unsigned w_ny = (c->ny + 1u) & 0xFFFu;
    k->ny_end = c->ny == c->reg_ny || (w_ny & 0x800) || ((w_ny & 0x400) && !x->v256);
    k->line_end = k->nx_end || k->sx_ovf || k->dx_ovf;
}

static inline uint32_t sext16(uint16_t v, unsigned shift) {
    return (uint32_t)((int32_t)(int16_t)v * (int32_t)(1u << shift));
}

/* All the "if (ff_count_valid)" register updates of vdp_command.v. */
static void advance(ctx_t *x, const cond_t *k) {
    v9968_cmd_state_t *c = x->c;
    unsigned op = c->op;
    unsigned step = c->pixel_step;
    bool xhr = arg_xhr(x);
    bool maj = arg_maj(c);
    bool lfmm = op == OP_LFMM;
    bool bit0 = c->bit_count == 0;

    /* LINE's error term */
    uint32_t next_nyb = ((uint32_t)c->nyb - c->reg_ny) & 0x1FFFu;
    bool shift = (next_nyb & 0x800) != 0;

    uint32_t sx = c->sx, sy = c->sy, sx2 = c->sx2, sy2 = c->sy2;
    uint16_t dx = c->dx, dy = c->dy, nx = c->nx, ny = c->ny;
    uint16_t reg_dx = c->reg_dx, reg_nx = c->reg_nx;
    uint8_t bit = c->bit_count;
    uint16_t dy_mask = x->v256 ? 0x7FF : 0x3FF;

    /* SX */
    if (op == OP_SRCH)
        sx = k->next_sx << 8;
    else if (op == OP_LRMM)
        sx = k->nx_end ? c->sx2 - sext16(c->vy, xhr ? 1 : 0) : c->sx + sext16(c->vx, 0);
    else if (k->line_end)
        sx = (op == OP_YMMM) ? (uint32_t)c->reg_dx << 8 : (uint32_t)c->reg_sx << 8;
    else
        sx = k->next_sx << 8;
    sx &= 0xFFFFFu;

    /* SY */
    if (sy_active(op)) {
        if (op == OP_LRMM)
            sy = k->nx_end ? c->sy2 + sext16(c->vx, xhr ? 1 : 0) : c->sy + sext16(c->vy, 0);
        else if (k->line_end)
            sy = (k->next_sy & dy_mask) << 8;
        sy &= 0x1FFFFFu;
    }

    /* LRMM's row start */
    if (!lfmm && k->nx_end) {
        sx2 = (c->sx2 - sext16(c->vy, xhr ? 1 : 0)) & 0xFFFFFu;
        sy2 = (c->sy2 + sext16(c->vx, xhr ? 1 : 0)) & 0x1FFFFFu;
    }

    /* DX, bit counter */
    if (op == OP_LINE) {
        if (!maj || shift) dx = (uint16_t)(k->next_dx & 0x1FF);
    } else if (lfmm) {
        if (bit0 && k->ny_end) reg_dx = (uint16_t)(k->next_dx & 0x1FF);
        if ((bit0 && !k->ny_end) || k->dx_ovf) {
            dx = c->reg_dx;
            bit = 7;
        } else {
            dx = (uint16_t)(k->next_dx & 0x1FF);
            bit = (uint8_t)((bit - 1) & 7);
        }
    } else if (k->line_end) {
        dx = c->reg_dx;
        bit = 7;
    } else {
        dx = (uint16_t)(k->next_dx & 0x1FF);
        bit = (uint8_t)((bit - 1) & 7);
    }

    /* DY */
    if (dxy_active(op)) {
        if (op == OP_LINE) {
            if (maj || shift) dy = (uint16_t)(k->next_dy & dy_mask);
        } else if (lfmm) {
            if (bit0 || k->dx_ovf)
                dy = k->ny_end ? (uint16_t)((c->sy2 >> 8) & 0x7FF) : (uint16_t)(k->next_dy & 0x7FF);
        } else if (k->line_end) {
            dy = (uint16_t)(k->next_dy & dy_mask);
        }
    }

    /* NX */
    if (op == OP_LINE) {
        if (!k->nx_end) nx = (uint16_t)((nx + step) & 0x7FF);
    } else if (lfmm) {
        if (bit0 && k->ny_end) reg_nx = c->nx;
        if (k->ny_end) nx = (uint16_t)((nx + 1) & 0x7FF);
    } else if (k->line_end) {
        nx = ((op & 0xC) == 0xC) ? step : 1;
    } else {
        nx = (uint16_t)((nx + step) & 0x7FF);
    }

    /* NY */
    if ((op & 0xC) != 0x4) {
        unsigned w_ny = (c->ny + 1u) & 0x7FF;
        if (lfmm) {
            if (bit0) ny = k->ny_end ? 1 : (uint16_t)w_ny;
        } else if (k->line_end && !k->ny_end) {
            ny = (uint16_t)w_ny;
        }
    }

    /* LFMM source (any command steps it, like the RTL) */
    if (bit0) c->font_addr = (c->font_addr + 1) & 0x3FFFFu;

    /* (the RTL steps it in every command; only LINE reads it and every
     * command start sets it, so the others leave it alone) */
    if (op == OP_LINE)
        c->nyb = (uint16_t)(shift ? ((next_nyb + c->reg_nx) & 0xFFF) : (next_nyb & 0xFFF));
    c->sx = sx; c->sy = sy; c->sx2 = sx2; c->sy2 = sy2;
    c->dx = dx; c->dy = dy; c->nx = nx; c->ny = ny;
    c->reg_dx = reg_dx; c->reg_nx = reg_nx;
    c->bit_count = bit;
}

static void finish(ctx_t *x, bool border) {
    v9968_cmd_state_t *c = x->c;
    c->state = ST_IDLE;
    c->reg_ny = c->ny;
    if (border) c->border_detect = true;
    V9968_SET_BITS(x->v->int_flags, V9968_INT_CMD_END);
}

/* ------------------------------------------------------------------ */
/* Register writes                                                     */
/* ------------------------------------------------------------------ */

static void start(v9968_t *v, uint8_t data) {
    v9968_cmd_state_t *c = &v->cmd;
    unsigned op = data >> 4;
    v9968_mode_t mode = v9968_mode(v);
    bool v256 = !v9968_v58(v);
    bool fg4 = arg_fg4(v, c);
    bool xhr = v256 && (c->arg & 0x40);

    c->op = (uint8_t)op;
    c->lop = data & 0x0F;
    if ((op & 0xC) != 0xC ||
        !(mode == V9968_MODE_G4 || mode == V9968_MODE_G5 || mode == V9968_MODE_G6 || fg4))
        c->pixel_step = 1;
    else
        c->pixel_step = (mode == V9968_MODE_G5) ? 4 : 2;
    c->wide = mode == V9968_MODE_G5 || mode == V9968_MODE_G6;

    uint16_t reg_sx = c->reg_sx;
    uint32_t sy = c->sy;
    uint16_t reg_nx = c->reg_nx, reg_ny = c->reg_ny;

    if (op == OP_YMMM)       c->sx = (uint32_t)c->reg_dx << 8;
    else if (op == OP_LRMM)  c->sx = (uint32_t)reg_sx << 8;
    else                     c->sx = (uint32_t)(reg_sx & 0x1FF) << 8;
    if (op != OP_LRMM) c->reg_sx &= 0x1FF;

    if (op == OP_LRMM)       c->sy = xhr ? (sy << 1) & 0x1FFFFFu : sy;
    else if (v256)           c->sy = sy & 0x7FFFFu;
    else                     c->sy = sy & 0x3FFFFu;

    c->sx2 = (uint32_t)reg_sx << 8;
    if (op == OP_LFMM)                 c->sy2 = (uint32_t)c->dy << 8;
    else if (op == OP_LRMM && xhr)     c->sy2 = (sy << 1) & 0x1FFFFFu;
    else                               c->sy2 = sy;

    c->dx = c->reg_dx;
    c->bit_count = 7;
    if (op != OP_LRMM) c->reg_nx &= 0x1FF;
    c->nx = (op == OP_LINE) ? 0 : c->pixel_step;
    if (op != OP_LRMM && !v256) c->reg_ny &= 0x3FF;
    c->ny = ((op & 0xC) == 0x4) ? reg_ny : 1;
    c->nyb = (uint16_t)((((unsigned)reg_nx - 1u) & 0x7FF) >> 1);
    c->source = c->color;
    c->read_color = false;

    if (op == OP_LMCM) { c->transfer_ready = false; c->lmcm_lag = false; }
    if (op == OP_LFMC) { c->transfer_ready = true; c->fore_color = c->color; }
    if (op == OP_LFMM) c->fore_color = c->color;

    static const uint8_t first_state[16] = {
        [OP_STOP] = ST_IDLE, [OP_LFMM] = ST_LFMM, [OP_LFMC] = ST_LFMC, [OP_LRMM] = ST_LRMM,
        [OP_POINT] = ST_POINT, [OP_PSET] = ST_PSET, [OP_SRCH] = ST_SRCH, [OP_LINE] = ST_LINE,
        [OP_LMMV] = ST_LMMV, [OP_LMMM] = ST_LMMM, [OP_LMCM] = ST_LMCM, [OP_LMMC] = ST_LMMC,
        [OP_HMMV] = ST_HMMV, [OP_HMMM] = ST_HMMM, [OP_YMMM] = ST_YMMM, [OP_HMMC] = ST_HMMC,
    };
    uint8_t st = first_state[op];
    if (op <= OP_LRMM && !v256) st = ST_IDLE;     /* extended commands need ECOM */
    c->state = st;
    if (st == ST_IDLE) {                           /* STOP: ends at once */
        c->reg_ny = c->ny;
        V9968_SET_BITS(v->int_flags, V9968_INT_CMD_END);
    }
}

void v9968_cmd_write_reg(v9968_t *v, unsigned reg, uint8_t data) {
    v9968_cmd_state_t *c = &v->cmd;
    bool v256 = !v9968_v58(v);
    switch (reg) {
    case 32:
        c->reg_sx = (uint16_t)((c->reg_sx & 0xF00) | data);
        c->font_addr = (c->font_addr & 0x3FF00u) | data;
        break;
    case 33:
        c->reg_sx = (uint16_t)((c->reg_sx & 0x0FF) | ((data & 0x0F) << 8));
        c->font_addr = (c->font_addr & 0x300FFu) | ((uint32_t)data << 8);
        break;
    case 34:
        c->sy = (c->sy & 0x1F0000u) | ((uint32_t)data << 8);
        c->font_addr = (c->font_addr & 0x0FFFFu) | ((uint32_t)(data & 3) << 16);
        break;
    case 35: c->sy = (c->sy & 0x00FFFFu) | ((uint32_t)(data & 0x1F) << 16); break;
    case 36: c->reg_dx = (uint16_t)((c->reg_dx & 0x100) | data); break;
    case 37: c->reg_dx = (uint16_t)((c->reg_dx & 0x0FF) | ((data & 1) << 8)); break;
    case 38: c->dy = (uint16_t)((c->dy & 0x700) | data); break;
    case 39: c->dy = (uint16_t)((c->dy & 0x0FF) | ((data & (v256 ? 7 : 3)) << 8)); break;
    case 40: c->reg_nx = (uint16_t)((c->reg_nx & 0x700) | data); break;
    case 41: c->reg_nx = (uint16_t)((c->reg_nx & 0x0FF) | ((data & 7) << 8)); break;
    case 42: c->reg_ny = (uint16_t)((c->reg_ny & 0x700) | data); break;
    case 43: c->reg_ny = (uint16_t)((c->reg_ny & 0x0FF) | ((data & (v256 ? 7 : 3)) << 8)); break;
    case 44:
        c->color = data;
        c->transfer_ready = false;
        break;
    case 45: c->arg = data; break;
    case 46: start(v, data); break;
    case 47: c->vx = (uint16_t)((c->vx & 0xFF00) | data); break;
    case 48: c->vx = (uint16_t)((c->vx & 0x00FF) | (data << 8)); break;
    case 49: c->vy = (uint16_t)((c->vy & 0xFF00) | data); break;
    case 50: c->vy = (uint16_t)((c->vy & 0x00FF) | (data << 8)); break;
    case 51: c->wsx = (uint16_t)((c->wsx & 0x100) | data); break;
    case 52: c->wsx = (uint16_t)((c->wsx & 0x0FF) | ((data & 1) << 8)); break;
    case 53: c->wsy = (uint16_t)((c->wsy & 0x700) | data); break;
    case 54: c->wsy = (uint16_t)((c->wsy & 0x0FF) | ((data & 7) << 8)); break;
    case 55: c->wex = (uint16_t)((c->wex & 0x100) | data); break;
    case 56: c->wex = (uint16_t)((c->wex & 0x0FF) | ((data & 1) << 8)); break;
    case 57: c->wey = (uint16_t)((c->wey & 0x700) | data); break;
    case 58: c->wey = (uint16_t)((c->wey & 0x0FF) | ((data & 7) << 8)); break;
    default: break;
    }
}

/* R#36-R#46 in one go (geo3d's LINE): what v9968_cmd_write_reg does for
 * each of them in turn; every pair writes both bytes of its register. */
static void write_line_regs(v9968_t *v, const uint8_t b[11]) {
    v9968_cmd_state_t *c = &v->cmd;
    bool v256 = !v9968_v58(v);
    c->reg_dx = (uint16_t)(b[0] | ((b[1] & 1) << 8));
    c->dy = (uint16_t)(b[2] | ((b[3] & (v256 ? 7 : 3)) << 8));
    c->reg_nx = (uint16_t)(b[4] | ((b[5] & 7) << 8));
    c->reg_ny = (uint16_t)(b[6] | ((b[7] & (v256 ? 7 : 3)) << 8));
    c->color = b[8];
    c->transfer_ready = false;
    c->arg = b[9];
    start(v, b[10]);
}

void v9968_cmd_read_color(v9968_t *v) {
    v->cmd.read_color = true;
    v->cmd.transfer_ready = false;
}

void v9968_cmd_clear_bd(v9968_t *v) {
    v->cmd.border_detect = false;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static inline int32_t sext(uint32_t v, unsigned bits) {
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

/* One pixel (or one wait check). Returns false when nothing can be done
 * now (waiting for the CPU), true otherwise. */
static bool step_one(ctx_t *x) {
    v9968_cmd_state_t *c = x->c;
    v9968_t *v = x->v;
    cond_t k;

    switch (c->state) {
    case ST_POINT:
        if ((c->sx & 0x10000) && !c->wide) { finish(x, false); break; }
        c->status_color = get_source_pixel(x);
        finish(x, false);
        break;

    case ST_PSET:
        if ((c->dx & 0x100) && !c->wide) { finish(x, false); break; }
        put_pixel(x, c->source);
        finish(x, false);
        break;

    case ST_SRCH:
        conditions(x, &k);
        if ((c->sx & 0x20000) || ((c->sx & 0x10000) && !c->wide)) {
            bool at_start = c->sx == (uint32_t)c->reg_sx << 8;
            bool eq = arg_eq(c);
            if (at_start && !eq) advance(x, &k);
            finish(x, at_start && eq);
            break;
        }
        if ((get_source_pixel(x) == c->color) == arg_eq(c)) {
            advance(x, &k);
        } else {
            finish(x, true);
        }
        break;

    case ST_LINE: {
        if ((c->dx & 0x100) && !c->wide) { finish(x, false); break; }
        conditions(x, &k);
        put_pixel(x, c->source);
        bool end = k.nx_end || k.dx_ovf || k.dy_ovf;
        advance(x, &k);
        if (end) finish(x, false);
        break;
    }

    case ST_LMMV:
    case ST_LMMM:
    case ST_LRMM: {
        conditions(x, &k);
        uint8_t src = c->color;
        bool end;
        if (c->state == ST_LMMM) {
            src = get_source_pixel(x);
            end = k.line_end && (k.ny_end || k.sy_ovf || k.dy_ovf);
        } else if (c->state == ST_LRMM) {
            uint32_t a = addr_s(x, c->sx, arg_xhr(x) ? (uint32_t)(sext(c->sy, 21) >> 1) & 0x1FFFFFu : c->sy);
            uint8_t pix = pixel_of(x, vram_read(x, a), (c->sx >> 8) & 3);
            int32_t ix = sext(sx_int(c), 12);
            uint32_t syw = arg_xhr(x) ? (uint32_t)(sext(c->sy, 21) >> 1) : c->sy;
            int32_t iy = sext((syw >> 8) & 0x1FFF, 13);
            bool outside = ix < (int32_t)c->wsx || iy < (int32_t)c->wsy ||
                           ix > (int32_t)c->wex || iy > (int32_t)c->wey;
            src = outside ? c->color : pix;
            end = (k.nx_end || k.dx_ovf) && (k.ny_end || k.dy_ovf);
        } else {
            end = k.line_end && (k.ny_end || k.dy_ovf);
        }
        put_pixel(x, src);
        advance(x, &k);
        if (end) finish(x, false);
        break;
    }

    case ST_LMCM:
        /* checked before the first read: a start already past the edge of
         * the last line reads nothing (later the RTL repeats the check on
         * the counters LMCM_WAIT already checked) */
        if (!c->lmcm_lag) {
            conditions(x, &k);
            if (k.line_end && k.ny_end) { finish(x, false); break; }
        }
        if (c->rtl_lmcm) {
            uint16_t rx = c->lmcm_lag ? c->lmcm_dx : c->dx;
            uint16_t ry = c->lmcm_lag ? c->lmcm_dy : c->dy;
            c->status_color = pixel_of(x, vram_read(x, addr_d(x, rx, ry)), rx & 3);
        }
        else
            c->status_color = get_source_pixel(x);
        c->color = c->status_color;
        c->transfer_ready = true;
        c->state = ST_LMCM_WAIT;
        break;

    case ST_LMCM_WAIT:
        if (!c->read_color) return false;
        c->read_color = false;
        conditions(x, &k);
        if (k.line_end && (k.ny_end || k.dy_ovf)) {
            finish(x, false);
        } else {
            c->lmcm_lag = true;
            c->lmcm_dx = c->dx;
            c->lmcm_dy = c->dy;
            advance(x, &k);
            c->state = ST_LMCM;
        }
        break;

    case ST_LMMC:
    case ST_HMMC: {
        if (c->transfer_ready) return false;
        conditions(x, &k);
        if (c->state == ST_LMMC) put_pixel(x, c->color);
        else                     vram_write(x, addr_d(x, c->dx, c->dy), c->color);
        bool end = (k.nx_end || k.dx_ovf) && (k.ny_end || k.dy_ovf);
        advance(x, &k);
        c->transfer_ready = true;
        if (end) finish(x, false);
        break;
    }

    case ST_HMMV:
    case ST_HMMM:
    case ST_YMMM: {
        conditions(x, &k);
        uint8_t d = c->color;
        bool end;
        if (c->state == ST_HMMV) {
            end = k.line_end && (k.ny_end || k.dy_ovf);
        } else {
            d = vram_read(x, addr_s(x, c->sx, c->sy));
            end = k.line_end && (k.ny_end || k.sy_ovf || k.dy_ovf);
        }
        vram_write(x, addr_d(x, c->dx, c->dy), d);
        advance(x, &k);
        if (end) finish(x, false);
        break;
    }

    case ST_LFMC:
        if (c->transfer_ready) return false;
        c->state = ST_LFMC_BIT;
        /* fall through */
    case ST_LFMC_BIT: {
        conditions(x, &k);
        uint8_t src = ((c->color >> c->bit_count) & 1) ? c->fore_color : v->regs[12];
        put_pixel(x, src);
        if (c->bit_count == 0) c->transfer_ready = true;
        bool line_end = k.nx_end || k.sx_ovf || k.dx_ovf;
        bool end = line_end && (k.ny_end || k.dy_ovf);
        advance(x, &k);
        if (end)           finish(x, false);
        else if (line_end) c->state = ST_LFMC;
        break;
    }

    case ST_LFMM:
        c->font_byte = vram_read(x, c->font_addr & V9968_VRAM_MASK);
        c->state = ST_LFMM_BIT;
        break;

    case ST_LFMM_BIT: {
        conditions(x, &k);
        uint8_t src = ((c->font_byte >> c->bit_count) & 1) ? c->fore_color : v->regs[12];
        put_pixel(x, src);
        bool end = (k.nx_end || k.dx_ovf) && (k.ny_end || k.dy_ovf);
        bool next_byte = c->bit_count == 0;
        advance(x, &k);
        if (end)            finish(x, false);
        else if (next_byte) c->state = ST_LFMM;
        break;
    }

    default:
        c->state = ST_IDLE;
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* LRMM fast path                                                      */
/* ------------------------------------------------------------------ */

/* The LRMM pixels inside a row (not its last pixel, not at the edge of the
 * screen): only SX/SY/DX/NX and the bit counter move, so this does what
 * step_one would, without the general conditions()/advance(). It stops
 * before the row's last pixel, which step_one does (it starts the next row
 * or ends the command), where the budget runs out, or when the bus needs
 * core1 (yield_now). The template parameters are constants in each copy
 * below; `imp` is LOP = IMP, where the destination is not read (and two
 * pixels of a byte are written at once: G4 nibbles, or FG4 in an 8-bit mode,
 * where both write the whole byte). The window must not be empty
 * (WEX >= WSX, WEY >= WSY), so that it can be tested with two unsigned
 * compares. */
static inline __attribute__((always_inline))
uint8_t lrmm_src(const uint8_t *vram, uint32_t sx, uint32_t sy, unsigned layout, unsigned bpp,
                 bool il, bool xhr, int32_t wsx, uint32_t wxr, int32_t wsy, uint32_t wyr,
                 uint8_t color) {
    uint32_t syw = xhr ? ((uint32_t)(sext(sy, 21) >> 1) & 0x1FFFFFu) : sy;
    int32_t ix = sext((sx >> 8) & 0xFFFu, 12);
    int32_t iy = sext((syw >> 8) & 0x1FFFu, 13);
    if ((uint32_t)(ix - wsx) > wxr || (uint32_t)(iy - wsy) > wyr) return color;
    uint32_t a;
    if (layout == 0)      a = ((syw >> 8) & 0x7FFu) << 7 | ((sx >> 9) & 0x7Fu);
    else if (layout == 1) a = ((syw >> 8) & 0x7FFu) << 7 | ((sx >> 10) & 0x7Fu);
    else if (layout == 2) a = ((syw >> 8) & 0x3FFu) << 8 | ((sx >> 9) & 0xFFu);
    else                  a = ((syw >> 8) & 0x3FFu) << 8 | ((sx >> 8) & 0xFFu);
    uint8_t b = vram[v9968_phys(il, a)];
    unsigned xs = (sx >> 8) & 3u;
    if (bpp == 4)      return (xs & 1) ? (b & 0x0F) : (b >> 4);
    else if (bpp == 2) return (b >> (6 - 2 * xs)) & 3;
    else               return b;
}

static inline __attribute__((always_inline))
void lrmm_row(ctx_t *x, unsigned budget, unsigned layout, unsigned bpp, bool il, bool xhr,
              bool imp) {
    v9968_cmd_state_t *c = x->c;
    const unsigned px_cost = x->hs ? COST_READ_HS * 2 + COST_WRITE_HS : COST_ACCESS_SLOW * 3;
    if (x->cost >= budget) return;

    /* pixels before the row's last one, or before DX leaves the screen */
    uint32_t dx = c->dx, nx = c->nx;
    bool dix = arg_dix(c);
    uint32_t n = (c->reg_nx - nx) & 0x7FFu, room;
    if (dix) room = (!c->wide && dx > 256) ? 0 : dx;
    else {
        uint32_t last = c->wide ? 511 : 255;
        room = dx < last ? last - dx : 0;
    }
    if (room < n) n = room;
    uint32_t nb = (budget - x->cost + px_cost - 1) / px_cost;      /* as many as step_one runs */
    if (nb < n) n = nb;
    if (n == 0) return;

    /* pixels before a bus event may stop it, and what to watch */
    const volatile uint32_t *watch = x->watch;
    const uint32_t watch_val = x->watch_val;
    uint32_t ymin = x->cost >= YIELD_MIN ? 0 : (YIELD_MIN - x->cost + px_cost - 1) / px_cost;
    uint32_t ynext = ymin > 4 ? ymin : 4;

    uint8_t *vram = x->v->vram;
    uint32_t sx = c->sx, sy = c->sy;
    const uint32_t vx = (uint32_t)(int32_t)(int16_t)c->vx;
    const uint32_t vy = (uint32_t)(int32_t)(int16_t)c->vy;
    const int32_t wsx = c->wsx, wsy = c->wsy;
    const uint32_t wxr = (uint32_t)(c->wex - c->wsx), wyr = (uint32_t)(c->wey - c->wsy);
    const unsigned lopc = c->lop;
    const uint8_t color = c->color;
    const int32_t dstep = dix ? -1 : 1;
#define SRC() lrmm_src(vram, sx, sy, layout, bpp, il, xhr, wsx, wxr, wsy, wyr, color)
#define STEP() do { sx = (sx + vx) & 0xFFFFFu; sy = (sy + vy) & 0x1FFFFFu; } while (0)

    /* destination row (DY does not change inside a row) */
    uint32_t dy = c->dy, drow;
    if (layout <= 1) drow = (dy & 0x7FFu) << 7;
    else             drow = (dy & 0x3FFu) << 8;

    uint32_t d = dx, i = 0;
    while (i < n) {
        uint32_t da;
        if (layout == 0)      da = drow | ((d >> 1) & 0x7Fu);
        else if (layout == 1) da = drow | ((d >> 2) & 0x7Fu);
        else if (layout == 2) da = drow | ((d >> 1) & 0xFFu);
        else                  da = drow | (d & 0xFFu);
        da = v9968_phys(il, da);

        if (imp && bpp == 4 && !dix && !(d & 1) && i + 1 < n) {
            /* both pixels of the byte */
            uint8_t p0 = SRC();
            STEP();
            uint8_t p1 = SRC();
            STEP();
            vram[da] = (uint8_t)((p0 << 4) | (p1 & 0x0F));
            d += 2;
            i += 2;
        } else if (imp && bpp == 8 && layout == 0 && !dix && !(d & 1) && i + 1 < n) {
            /* FG4: both pixels write the whole byte, the second one stays */
            STEP();
            vram[da] = SRC();
            STEP();
            d += 2;
            i += 2;
        } else {
            uint8_t src = SRC();
            uint8_t out;
            if (imp && bpp == 8) {
                out = src;
            } else {
                uint8_t base = vram[da];
                unsigned op = imp ? 0 : lopc;
                if (bpp == 4) {
                    uint8_t dp = (d & 1) ? (base & 0x0F) : (base >> 4);
                    uint8_t p = lop(op, src, dp);
                    out = (d & 1) ? (uint8_t)((base & 0xF0) | (p & 0x0F))
                                  : (uint8_t)((base & 0x0F) | (p << 4));
                } else if (bpp == 2) {
                    unsigned sh = 6 - 2 * (d & 3);
                    uint8_t p = lop(op, src, (base >> sh) & 3);
                    out = (uint8_t)((base & ~(3u << sh)) | ((p & 3u) << sh));
                } else {
                    out = lop(op, src, base);
                }
            }
            vram[da] = out;
            STEP();
            d = (uint32_t)((int32_t)d + dstep);
            i++;
        }
        if (i >= ynext) {
            ynext = i + 4;
            if (watch && *watch != watch_val) break;
        }
    }
#undef SRC
#undef STEP
    n = i;

    /* the rest of advance() for n pixels: the bit counter steps the LFMM
     * source address each time it passes 0 */
    uint32_t bit = c->bit_count;
    c->font_addr = (c->font_addr + (n + 7u - bit) / 8u) & 0x3FFFFu;
    c->bit_count = (uint8_t)((bit - n) & 7u);
    c->sx = sx; c->sy = sy;
    c->dx = (uint16_t)(d & 0x1FFu);
    c->nx = (uint16_t)((nx + n) & 0x7FFu);
    x->cost += n * px_cost;
}

/* The copies for the screen modes and LOP the demos use; the rest run the
 * general one. */
V9968_HOT static void lrmm_g4(ctx_t *x, unsigned b)     { lrmm_row(x, b, 0, 4, false, false, true); }
V9968_HOT static void lrmm_fg4(ctx_t *x, unsigned b)    { lrmm_row(x, b, 0, 8, false, false, true); }
V9968_HOT static void lrmm_g7(ctx_t *x, unsigned b)     { lrmm_row(x, b, 3, 8, false, false, true); }
V9968_HOT static void lrmm_g7_il(ctx_t *x, unsigned b)  { lrmm_row(x, b, 3, 8, true, false, true); }
V9968_HOT static void __attribute__((noinline))
lrmm_any(ctx_t *x, unsigned b, unsigned layout, bool il, bool xhr) {
    lrmm_row(x, b, layout, x->bpp, il, xhr, false);
}

V9968_HOT static void lrmm_run(ctx_t *x, unsigned budget) {
    v9968_cmd_state_t *c = x->c;
    /* an empty window: every pixel is outside, step_one does them */
    if (c->wex < c->wsx || c->wey < c->wsy) return;
    unsigned layout = x->g4 ? 0 : x->g5 ? 1 : x->g6 ? 2 : 3;
    bool il = x->interleave, xhr = arg_xhr(x);
    void (*fast)(ctx_t *, unsigned) = NULL;
    if (c->lop == 0 && !xhr) {
        if (!il && layout == 0) fast = x->bpp == 4 ? lrmm_g4 : x->bpp == 8 ? lrmm_fg4 : NULL;
        else if (layout == 3 && x->bpp == 8) fast = il ? lrmm_g7_il : lrmm_g7;
    }

    while (c->state == ST_LRMM && x->cost < budget) {
        if (fast) fast(x, budget);
        else      lrmm_any(x, budget, layout, il, xhr);
        if (x->cost >= budget || yield_now(x, x->cost)) break;
        step_one(x);                    /* the row's last pixel, or at the edge */
    }
}

/* ------------------------------------------------------------------ */
/* HMMM / LMMM fast path                                               */
/* ------------------------------------------------------------------ */

/* Pixels (k = 0, 1, ...) after which the next counter value v -/+ (k + 1) *
 * step stays on the screen (no bit 9; no bit 8 unless `wide`), as the
 * sx_ovf / dx_ovf tests of conditions(). */
static inline uint32_t room_on_screen(uint32_t v, unsigned step, bool down, bool wide) {
    uint32_t lim = wide ? 512u : 256u;
    if (down) return (v < step || v - step >= lim) ? 0 : v / step;
    return v + step < lim ? (lim - 1u - v) / step : 0;
}

/* The HMMM / LMMM pixels inside a row, as lrmm_row: only SX/DX/NX and the
 * bit counter move (SX and DX by the pixel step), and the row's last pixel
 * or the edge of the screen is left to step_one. */
static inline __attribute__((always_inline))
void mm_row(ctx_t *x, unsigned budget, unsigned layout, unsigned bpp, bool il, bool lmmm, bool imp) {
    v9968_cmd_state_t *c = x->c;
    unsigned px_cost = lmmm ? (x->hs ? COST_READ_HS * 2 + COST_WRITE_HS : COST_ACCESS_SLOW * 3)
                            : (x->hs ? COST_READ_HS + COST_WRITE_HS : COST_ACCESS_SLOW * 2);
    if (x->cost >= budget) return;

    unsigned step = c->pixel_step;
    bool dix = arg_dix(c);
    uint32_t sxi = sx_int(c), dx = c->dx, nx = c->nx;
    uint32_t nx_max = (step == 4) ? (c->reg_nx & ~3u) : (step == 2) ? (c->reg_nx & ~1u) : c->reg_nx;
    uint32_t n = ((nx_max - nx) & 0x7FFu) / step;
    uint32_t r = room_on_screen(sxi, step, dix, c->wide);
    if (r < n) n = r;
    r = room_on_screen(dx, step, dix, c->wide);
    if (r < n) n = r;
    uint32_t nb = (budget - x->cost + px_cost - 1) / px_cost;
    if (nb < n) n = nb;
    if (n == 0) return;

    const volatile uint32_t *watch = x->watch;
    const uint32_t watch_val = x->watch_val;
    uint32_t ymin = x->cost >= YIELD_MIN ? 0 : (YIELD_MIN - x->cost + px_cost - 1) / px_cost;
    uint32_t ynext = ymin > 8 ? ymin : 8;

    uint8_t *vram = x->v->vram;
    uint32_t sy = (c->sy >> 8), dy = c->dy;
    uint32_t srow, drow;
    if (layout <= 1) { srow = (sy & 0x7FFu) << 7; drow = (dy & 0x7FFu) << 7; }
    else             { srow = (sy & 0x3FFu) << 8; drow = (dy & 0x3FFu) << 8; }
    const int32_t st = dix ? -(int32_t)step : (int32_t)step;
    const unsigned lopc = c->lop;

    uint32_t s = sxi, d = dx, i = 0;
    while (i < n) {
        uint32_t sa, da;
        if (layout == 0)      { sa = srow | ((s >> 1) & 0x7Fu); da = drow | ((d >> 1) & 0x7Fu); }
        else if (layout == 1) { sa = srow | ((s >> 2) & 0x7Fu); da = drow | ((d >> 2) & 0x7Fu); }
        else if (layout == 2) { sa = srow | ((s >> 1) & 0xFFu); da = drow | ((d >> 1) & 0xFFu); }
        else                  { sa = srow | (s & 0xFFu);        da = drow | (d & 0xFFu); }
        sa = v9968_phys(il, sa);
        da = v9968_phys(il, da);
        uint8_t b = vram[sa];
        if (!lmmm) {
            vram[da] = b;
        } else {
            uint8_t src;
            if (bpp == 4)      src = (s & 1) ? (b & 0x0F) : (b >> 4);
            else if (bpp == 2) src = (b >> (6 - 2 * (s & 3))) & 3;
            else               src = b;
            if (imp && bpp == 8) {
                vram[da] = src;
            } else {
                uint8_t base = vram[da];
                unsigned op = imp ? 0 : lopc;
                uint8_t out;
                if (bpp == 4) {
                    uint8_t dp = (d & 1) ? (base & 0x0F) : (base >> 4);
                    uint8_t p = lop(op, src, dp);
                    out = (d & 1) ? (uint8_t)((base & 0xF0) | (p & 0x0F))
                                  : (uint8_t)((base & 0x0F) | (p << 4));
                } else if (bpp == 2) {
                    unsigned sh = 6 - 2 * (d & 3);
                    uint8_t p = lop(op, src, (base >> sh) & 3);
                    out = (uint8_t)((base & ~(3u << sh)) | ((p & 3u) << sh));
                } else {
                    out = lop(op, src, base);
                }
                vram[da] = out;
            }
        }
        s = (uint32_t)((int32_t)s + st);
        d = (uint32_t)((int32_t)d + st);
        i++;
        if (i >= ynext) {
            ynext = i + 8;
            if (watch && *watch != watch_val) break;
        }
    }
    n = i;

    uint32_t bit = c->bit_count;
    c->font_addr = (c->font_addr + (n + 7u - bit) / 8u) & 0x3FFFFu;
    c->bit_count = (uint8_t)((bit - n) & 7u);
    c->sx = (s & 0xFFFu) << 8;
    c->dx = (uint16_t)(d & 0x1FFu);
    c->nx = (uint16_t)((nx + n * step) & 0x7FFu);
    x->cost += n * px_cost;
}

V9968_HOT static void hmmm_g4(ctx_t *x, unsigned b)    { mm_row(x, b, 0, 4, false, false, false); }
V9968_HOT static void hmmm_g7(ctx_t *x, unsigned b)    { mm_row(x, b, 3, 8, false, false, false); }
V9968_HOT static void lmmm_g4(ctx_t *x, unsigned b)    { mm_row(x, b, 0, 4, false, true, true); }
V9968_HOT static void lmmm_g7(ctx_t *x, unsigned b)    { mm_row(x, b, 3, 8, false, true, true); }
V9968_HOT static void __attribute__((noinline))
mm_any(ctx_t *x, unsigned b, unsigned layout, bool il, bool lmmm) {
    mm_row(x, b, layout, x->bpp, il, lmmm, false);
}

V9968_HOT static void mm_run(ctx_t *x, unsigned budget) {
    v9968_cmd_state_t *c = x->c;
    unsigned layout = x->g4 ? 0 : x->g5 ? 1 : x->g6 ? 2 : 3;
    bool il = x->interleave, lmmm = c->state == ST_LMMM;
    uint8_t state = c->state;
    void (*fast)(ctx_t *, unsigned) = NULL;
    if (!il && x->bpp == (layout == 0 ? 4u : 8u) && (layout == 0 || layout == 3) &&
        (!lmmm || c->lop == 0))
        fast = layout == 0 ? (lmmm ? lmmm_g4 : hmmm_g4) : (lmmm ? lmmm_g7 : hmmm_g7);

    while (c->state == state && x->cost < budget) {
        if (fast) fast(x, budget);
        else      mm_any(x, budget, layout, il, lmmm);
        if (x->cost >= budget || yield_now(x, x->cost)) break;
        step_one(x);                    /* the row's last pixel, or at the edge */
    }
}

/* ------------------------------------------------------------------ */
/* LINE fast path                                                      */
/* ------------------------------------------------------------------ */

/* LINE's last pixel (NX = R#40), as step_one: put_pixel, the LINE parts of
 * advance() at the end of the line (SX back to R#32, LRMM's row start
 * stepped, DX/DY/NYB as for any pixel; NX stays) and finish(). */
static inline __attribute__((always_inline))
void line_last(ctx_t *x, unsigned budget, unsigned layout, unsigned bpp, bool il) {
    v9968_cmd_state_t *c = x->c;
    if (x->cost >= budget) return;
    uint32_t d = c->dx, y = c->dy;
    if ((d & 0x100) && !c->wide) { finish(x, false); return; }
    const bool dix = arg_dix(c), diy = arg_diy(c), maj = arg_maj(c);
    const unsigned lopc = c->lop;
    const uint8_t src = c->source;
    uint32_t a;
    if (layout == 0)      a = (y & 0x7FFu) << 7 | ((d >> 1) & 0x7Fu);
    else if (layout == 1) a = (y & 0x7FFu) << 7 | ((d >> 2) & 0x7Fu);
    else if (layout == 2) a = (y & 0x3FFu) << 8 | ((d >> 1) & 0xFFu);
    else                  a = (y & 0x3FFu) << 8 | (d & 0xFFu);
    a = v9968_phys(il, a);
    uint8_t *vram = x->v->vram;
    uint8_t base = vram[a];
    if (bpp == 4) {
        uint8_t p = lop(lopc, src, (d & 1) ? (base & 0x0F) : (base >> 4));
        vram[a] = (d & 1) ? (uint8_t)((base & 0xF0) | (p & 0x0F)) : (uint8_t)((base & 0x0F) | (p << 4));
    } else if (bpp == 2) {
        unsigned sh = 6 - 2 * (d & 3);
        uint8_t p = lop(lopc, src, (base >> sh) & 3);
        vram[a] = (uint8_t)((base & ~(3u << sh)) | ((p & 3u) << sh));
    } else {
        vram[a] = lop(lopc, src, base);
    }
    x->cost += x->hs ? COST_READ_HS + COST_WRITE_HS : COST_ACCESS_SLOW * 2;

    uint32_t next_nyb = ((uint32_t)c->nyb - c->reg_ny) & 0x1FFFu;
    bool shift = (next_nyb & 0x800) != 0;
    bool xhr = arg_xhr(x);
    c->sx = ((uint32_t)c->reg_sx << 8) & 0xFFFFFu;
    c->sx2 = (c->sx2 - sext16(c->vy, xhr ? 1 : 0)) & 0xFFFFFu;
    c->sy2 = (c->sy2 + sext16(c->vx, xhr ? 1 : 0)) & 0x1FFFFFu;
    if (!maj || shift) c->dx = (uint16_t)(((dix ? d - 1u : d + 1u) & 0x3FFu) & 0x1FF);
    if (maj || shift)  c->dy = (uint16_t)(((diy ? y - 1u : y + 1u) & 0x1FFFu) & (x->v256 ? 0x7FFu : 0x3FFu));
    if (c->bit_count == 0) c->font_addr = (c->font_addr + 1) & 0x3FFFFu;
    c->nyb = (uint16_t)(shift ? ((next_nyb + c->reg_nx) & 0xFFF) : (next_nyb & 0xFFF));
    finish(x, false);
}

/* The LINE pixels before its last one (NX = R#40) and away from the edges
 * (DX or DY about to leave the screen, or DX past 255 in a 256-pixel mode):
 * step_one does those and ends the command. In between, a pixel is
 * conditions() without any end, put_pixel(), and the LINE parts of
 * advance(): the error term NYB, DX on the major axis or a shift, DY on the
 * minor one or a shift, NX, and SX stepping by one (from the integer part,
 * as next_sx). */
static inline __attribute__((always_inline))
void line_pixels(ctx_t *x, unsigned budget, unsigned layout, unsigned bpp, bool il) {
    v9968_cmd_state_t *c = x->c;
    const unsigned px_cost = x->hs ? COST_READ_HS + COST_WRITE_HS : COST_ACCESS_SLOW * 2;
    if (x->cost >= budget) return;
    uint32_t nx = c->nx, reg_nx = c->reg_nx;
    if (nx > reg_nx) return;
    if (nx == reg_nx) { line_last(x, budget, layout, bpp, il); return; }
    uint32_t n = reg_nx - nx;
    uint32_t nb = (budget - x->cost + px_cost - 1) / px_cost;
    if (nb < n) n = nb;

    const volatile uint32_t *watch = x->watch;
    const uint32_t watch_val = x->watch_val;
    uint32_t ymin = x->cost >= YIELD_MIN ? 0 : (YIELD_MIN - x->cost + px_cost - 1) / px_cost;
    uint32_t ynext = ymin > 8 ? ymin : 8;

    uint8_t *vram = x->v->vram;
    const bool dix = arg_dix(c), diy = arg_diy(c), maj = arg_maj(c), wide = c->wide;
    const uint32_t dlast = wide ? 511u : 255u;          /* DX + 1 overflows past it */
    const uint32_t dy_mask = x->v256 ? 0x7FFu : 0x3FFu;
    const uint32_t reg_ny = c->reg_ny;
    const unsigned lopc = c->lop;
    const uint8_t src = c->source;
    const bool skip = (lopc & 8) && src == 0;           /* T-ops with colour 0 keep VRAM */
    uint32_t d = c->dx, y = c->dy, nyb = c->nyb, i = 0;

    /* A horizontal line (NY = 0 on the X axis, as geo3d fills faces): NYB
     * never reaches bit 11, so only DX moves; with IMP (or a T-op on a
     * colour other than 0) the run is a fill (256-pixel modes: FG4 in G6
     * wraps its 512 pixels in 128-byte rows). */
    if (!maj && reg_ny == 0 && (lopc & 7) == 0 && !skip && !il && !wide &&
        ((layout == 0 && bpp == 4) || (layout == 3 && bpp == 8))) {
        uint32_t room;
        if (diy && y == 0) room = 0;                    /* dy_ovf on every pixel */
        else if (dix) room = (!wide && d >= 256) ? 0 : d;
        else room = d < dlast ? dlast - d : 0;
        if (room < n) n = room;
        if (n == 0) return;
        uint32_t lo = dix ? d - (n - 1) : d, hi = lo + n - 1;
        if (layout == 3) {
            memset(vram + ((y & 0x3FFu) << 8 | lo), src, n);
        } else {
            uint8_t *row = vram + ((y & 0x7FFu) << 7);
            uint8_t p = src & 0x0F;
            if (lo & 1) { row[lo >> 1] = (uint8_t)((row[lo >> 1] & 0xF0) | p); lo++; }
            if (lo <= hi && !(hi & 1)) { row[hi >> 1] = (uint8_t)((row[hi >> 1] & 0x0F) | (p << 4)); if (hi == 0) goto done; hi--; }
            if (lo <= hi) memset(row + (lo >> 1), p * 0x11, (hi - lo + 1) >> 1);
        }
    done:
        i = n;
        d = dix ? d - n : d + n;
        goto out;
    }

    while (i < n) {
        /* dx_ovf, and step_one's DX >= 256 test in a 256-pixel mode */
        if (dix ? (d == 0 || (!wide && d >= 256)) : d >= dlast) break;
        if (diy && y == 0) break;                       /* dy_ovf */
        if (!skip) {
            uint32_t a;
            if (layout == 0)      a = (y & 0x7FFu) << 7 | ((d >> 1) & 0x7Fu);
            else if (layout == 1) a = (y & 0x7FFu) << 7 | ((d >> 2) & 0x7Fu);
            else if (layout == 2) a = (y & 0x3FFu) << 8 | ((d >> 1) & 0xFFu);
            else                  a = (y & 0x3FFu) << 8 | (d & 0xFFu);
            a = v9968_phys(il, a);
            uint8_t base = vram[a];
            if (bpp == 4) {
                uint8_t p = lop(lopc, src, (d & 1) ? (base & 0x0F) : (base >> 4));
                vram[a] = (d & 1) ? (uint8_t)((base & 0xF0) | (p & 0x0F))
                                  : (uint8_t)((base & 0x0F) | (p << 4));
            } else if (bpp == 2) {
                unsigned sh = 6 - 2 * (d & 3);
                uint8_t p = lop(lopc, src, (base >> sh) & 3);
                vram[a] = (uint8_t)((base & ~(3u << sh)) | ((p & 3u) << sh));
            } else {
                vram[a] = lop(lopc, src, base);
            }
        }
        uint32_t next_nyb = (nyb - reg_ny) & 0x1FFFu;
        bool shift = (next_nyb & 0x800) != 0;
        nyb = shift ? ((next_nyb + reg_nx) & 0xFFFu) : (next_nyb & 0xFFFu);
        if (!maj || shift) d = dix ? d - 1 : d + 1;
        if (maj || shift)  y = (diy ? y - 1 : y + 1) & dy_mask;
        i++;
        if (i >= ynext) {
            ynext = i + 8;
            if (watch && *watch != watch_val) break;
        }
    }
out:
    if (i == 0) return;
    if (c->bit_count == 0) c->font_addr = (c->font_addr + i) & 0x3FFFFu;
    uint32_t sxi = sx_int(c);
    sxi = (dix ? sxi - i : sxi + i) & 0xFFFu;
    c->sx = (sxi << 8) & 0xFFFFFu;
    c->dx = (uint16_t)d;
    c->dy = (uint16_t)y;
    c->nyb = (uint16_t)nyb;
    c->nx = (uint16_t)(nx + i);
    x->cost += i * px_cost;
    if (c->nx == reg_nx && !yield_now(x, x->cost)) line_last(x, budget, layout, bpp, il);
}

V9968_HOT static void line_g4(ctx_t *x, unsigned b) { line_pixels(x, b, 0, 4, false); }
V9968_HOT static void line_g7(ctx_t *x, unsigned b) { line_pixels(x, b, 3, 8, false); }
/* the other modes, and G7 interleaved (without sprite mode 3): the demos
 * draw lines in G4 and in G7 with SP3 */
V9968_HOT static void __attribute__((noinline)) line_any(ctx_t *x, unsigned b) {
    unsigned layout = x->g4 ? 0 : x->g5 ? 1 : x->g6 ? 2 : 3;
    line_pixels(x, b, layout, x->bpp, x->interleave);
}

V9968_HOT static void line_run(ctx_t *x, unsigned budget) {
    if (x->g4 && x->bpp == 4 && !x->interleave) line_g4(x, budget);
    else if (!x->g4 && !x->g5 && !x->g6 && x->bpp == 8 && !x->interleave) line_g7(x, budget);
    else line_any(x, budget);
}

V9968_HOT unsigned v9968_cmd_step(v9968_t *v, unsigned budget) {
    if (v->cmd.state == ST_IDLE) return 0;
    ctx_t x;
    make_ctx(&x, v);
    if (!v9968_cmd_reference) {
        if (v->cmd.state == ST_LRMM) lrmm_run(&x, budget);
        else if (v->cmd.state == ST_LMMM || (v->cmd.state == ST_HMMM && v->cmd.op == OP_HMMM))
            mm_run(&x, budget);
        else if (v->cmd.state == ST_LINE) line_run(&x, budget);
    }
    while (v->cmd.state != ST_IDLE && x.cost < budget && !yield_now(&x, x.cost)) {
        if (!step_one(&x)) break;
    }
    return x.cost;
}

/* geo3d's issuer: R#36-R#46 (LINE) or R#32-R#50 (LRMM) whenever CE = 0. */
/* geo3d's short commands follow one another within the budget, until a
 * bus event waits (v9968_cmd_watch), rather than one per call. */
V9968_HOT unsigned v9968_service(v9968_t *v, unsigned budget) {
    geo3d_t *g = v->geo;
    const volatile uint32_t *watch = v9968_cmd_watch;
    const uint32_t watch_val = watch ? *watch : 0;
    unsigned used = 0;
    for (;;) {
        if (g->running && v->cmd.state == ST_IDLE) {
            uint8_t b[GEO3D_CMD_MAX];
            unsigned n = geo3d_next_command(g, b);
            if (n == 0) g->running = false;
            if (n == 11 && geo3d_command_reg(n, 0) == 36) write_line_regs(v, b);
            else for (unsigned k = 0; k < n; k++) v9968_cmd_write_reg(v, geo3d_command_reg(n, k), b[k]);
        }
        used += v9968_cmd_step(v, budget - used);
        if (used >= budget || !g->running || v->cmd.state != ST_IDLE) return used;
        if (watch && *watch != watch_val) return used;
    }
}
