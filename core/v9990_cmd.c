/*
 * Command engine (blitter).
 *
 * Implemented: STOP, LMMC, LMMV, PSET.
 * TODO: LMCM, LMMM, CMMC, CMMK, CMMM, BMXL, BMLX, BMLL, LINE, SRCH, POINT,
 *       ADVN, P1/P2 coordinate systems, realistic execution timing.
 *
 * Commands run incrementally via v9990_cmd_step() so the bus loop on core1
 * stays responsive; there is no attempt at cycle-accurate speed yet.
 */
#include "v9990.h"
#include "v9990_vram.h"

static uint16_t reg16(const v9990_t *v, unsigned lo) {
    return (uint16_t)(v->regs[lo] | (v->regs[lo + 1] << 8));
}

/* Bitwise logical operation. LOP bit L<s><d> gives the result for a source
 * bit s and destination bit d, e.g. 0x0C = IMP (copy source). */
static inline unsigned logop(uint8_t lop, unsigned s, unsigned d) {
    unsigned r = 0;
    if (lop & 0x8) r |=  s &  d;
    if (lop & 0x4) r |=  s & ~d;
    if (lop & 0x2) r |= ~s &  d;
    if (lop & 0x1) r |= ~s & ~d;
    return r;
}

static void write_pixel(v9990_t *v, unsigned x, unsigned y, unsigned color) {
    const unsigned bpp = v9990_bpp(v);
    const unsigned iw = v9990_image_width(v);
    const uint8_t lop = v->regs[V9990_R_CMD_LOP];

    if ((lop & V9990_LOP_TP) && color == 0) return;

    x &= iw - 1u;
    uint32_t bit = ((uint32_t)y * iw + x) * bpp;
    uint32_t addr = (bit >> 3) & V9990_VRAM_MASK;

    if (bpp == 16) {
        for (unsigned i = 0; i < 2; i++) {
            uint8_t wm = v->regs[V9990_R_CMD_WM0 + i];
            uint8_t d = v9990_vram_read_bx(v, addr + i);
            uint8_t s = (uint8_t)(color >> (8 * i));
            uint8_t r = (uint8_t)logop(lop, s, d);
            v9990_vram_write_bx(v, addr + i, (uint8_t)((d & ~wm) | (r & wm)));
        }
        return;
    }

    /* VERIFY: which WM byte applies to even/odd addresses */
    uint8_t wm = v->regs[V9990_R_CMD_WM0 + (addr & 1)];
    unsigned shift = 8u - bpp - (bit & 7u);
    uint8_t mask = (uint8_t)((((1u << bpp) - 1u) << shift) & wm);
    uint8_t d = v9990_vram_read_bx(v, addr);
    uint8_t r = (uint8_t)(logop(lop, color << shift, d));
    v9990_vram_write_bx(v, addr, (uint8_t)((d & ~mask) | (r & mask)));
}

static void finish(v9990_t *v) {
    v->cmd.op = 0;
    V9990_CLR_BITS(v->status, V9990_ST_CE | V9990_ST_TR);
    V9990_SET_BITS(v->int_flags, V9990_INT_CE);
}

/* Advance the destination cursor of a rectangle command.
 * Returns false when the rectangle is complete. */
static bool advance_rect(v9990_t *v) {
    const uint8_t arg = v->regs[V9990_R_CMD_ARG];
    v->cmd.x = (uint16_t)(v->cmd.x + ((arg & V9990_ARG_DIX) ? -1 : 1));
    if (--v->cmd.remaining_x == 0) {
        if (--v->cmd.remaining_y == 0) return false;
        v->cmd.remaining_x = v->cmd.nx;
        v->cmd.x = v->cmd.dx;
        v->cmd.y = (uint16_t)(v->cmd.y + ((arg & V9990_ARG_DIY) ? -1 : 1));
    }
    return true;
}

static unsigned fore_color(const v9990_t *v) {
    /* For 2/4/8bpp software replicates the color across FC; the low bits
     * are sufficient here. */
    unsigned bpp = v9990_bpp(v);
    unsigned fc = reg16(v, V9990_R_CMD_FC0);
    return bpp == 16 ? fc : (fc & ((1u << bpp) - 1u));
}

void v9990_cmd_start(v9990_t *v) {
    v9990_cmd_state_t *c = &v->cmd;
    c->op = v->regs[V9990_R_CMD_OP] >> 4;
    c->sx = reg16(v, V9990_R_CMD_SX0) & 0x07FF;
    c->sy = reg16(v, V9990_R_CMD_SY0) & 0x0FFF;
    c->dx = reg16(v, V9990_R_CMD_DX0) & 0x07FF;
    c->dy = reg16(v, V9990_R_CMD_DY0) & 0x0FFF;
    c->nx = reg16(v, V9990_R_CMD_NX0) & 0x07FF;
    c->ny = reg16(v, V9990_R_CMD_NY0) & 0x0FFF;
    if (c->nx == 0) c->nx = 2048;   /* VERIFY */
    if (c->ny == 0) c->ny = 4096;   /* VERIFY */
    c->x = c->dx;
    c->y = c->dy;
    c->remaining_x = c->nx;
    c->remaining_y = c->ny;
    c->data_phase = 0;
    c->data_word = 0;

    V9990_SET_BITS(v->status, V9990_ST_CE);
    V9990_CLR_BITS(v->status, V9990_ST_TR);

    switch (c->op) {
    case V9990_CMD_STOP:
        c->op = 0;
        V9990_CLR_BITS(v->status, V9990_ST_CE | V9990_ST_TR);
        break;
    case V9990_CMD_LMMC:
        V9990_SET_BITS(v->status, V9990_ST_TR);
        break;
    case V9990_CMD_LMMV:
        break;
    case V9990_CMD_PSET:
        write_pixel(v, c->dx, c->dy, fore_color(v));
        finish(v);
        break;
    default:
        /* TODO: unimplemented command, end it immediately */
        finish(v);
        break;
    }
}

V9990_HOT bool v9990_cmd_step(v9990_t *v, unsigned budget) {
    if (v->cmd.op == V9990_CMD_LMMV) {
        unsigned color = fore_color(v);
        while (budget--) {
            write_pixel(v, v->cmd.x, v->cmd.y, color);
            if (!advance_rect(v)) {
                finish(v);
                return false;
            }
        }
    }
    return v->cmd.op != 0;
}

void v9990_cmd_write_data(v9990_t *v, uint8_t data) {
    if (v->cmd.op != V9990_CMD_LMMC) return;
    unsigned bpp = v9990_bpp(v);

    if (bpp == 16) {
        /* VERIFY: low byte first */
        if (v->cmd.data_phase == 0) {
            v->cmd.data_word = data;
            v->cmd.data_phase = 1;
            return;
        }
        v->cmd.data_phase = 0;
        write_pixel(v, v->cmd.x, v->cmd.y, v->cmd.data_word | (data << 8));
        if (!advance_rect(v)) finish(v);
        return;
    }

    /* VERIFY: pixels per byte, leftmost pixel in the MSBs */
    for (unsigned shift = 8; shift >= bpp; shift -= bpp) {
        write_pixel(v, v->cmd.x, v->cmd.y,
                    (data >> (shift - bpp)) & ((1u << bpp) - 1u));
        if (!advance_rect(v)) {
            finish(v);
            return;
        }
    }
}

uint8_t v9990_cmd_read_data(v9990_t *v) {
    /* TODO: LMCM / POINT */
    return v->cmd_read_latch;
}
