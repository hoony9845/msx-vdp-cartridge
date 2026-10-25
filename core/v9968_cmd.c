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
 */
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
} ctx_t;

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
static uint8_t lop(unsigned op, uint8_t src, uint8_t dst) {
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

V9968_HOT unsigned v9968_cmd_step(v9968_t *v, unsigned budget) {
    if (v->cmd.state == ST_IDLE) return 0;
    ctx_t x;
    make_ctx(&x, v);
    while (v->cmd.state != ST_IDLE && x.cost < budget) {
        if (!step_one(&x)) break;
    }
    return x.cost;
}

/* geo3d's issuer: R#36-R#46 (LINE) or R#32-R#50 (LRMM) whenever CE = 0. */
V9968_HOT unsigned v9968_service(v9968_t *v, unsigned budget) {
    geo3d_t *g = &v->geo;
    if (g->running && v->cmd.state == ST_IDLE) {
        uint8_t b[GEO3D_CMD_MAX];
        unsigned n = geo3d_next_command(g, b);
        if (n == 0) g->running = false;
        for (unsigned k = 0; k < n; k++) v9968_cmd_write_reg(v, geo3d_command_reg(n, k), b[k]);
    }
    return v9968_cmd_step(v, budget);
}
