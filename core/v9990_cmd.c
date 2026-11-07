/*
 * Command engine (blitter).
 *
 * Implemented: STOP, LMMC, LMMV, LMMM, CMMC, CMMM, LINE, PSET (bitmap
 * coordinates).
 * P1/P2 use the coordinate systems openMSX implements (not documented).
 * TODO: LMCM, CMMK, BMXL, BMLX, BMLL, SRCH, POINT, ADVN, realistic
 *       execution timing.
 *
 * Commands run incrementally via v9990_cmd_step() so the bus loop on core1
 * stays responsive; there is no attempt at cycle-accurate speed yet.
 *
 * Manual 11.2: FC, BC and WM map onto VRAM bit positions - bits 0-7 to the
 * VRAM0 byte (even CPU address in bitmap modes), bits 8-15 to VRAM1 (odd);
 * with 2/4bpp the dot's bits within that byte are used. NX = 0 means 2048,
 * NY = 0 means 4096. Character commands (CMMC/CMMM) take one bit per dot,
 * MSB first, as a continuous stream across rows: 1 = FC, 0 = BC.
 */
#include "v9990.h"
#include "v9990_vram.h"

V9990_HOT static uint16_t reg16(const v9990_t *v, unsigned lo) {
    return (uint16_t)(v->regs[lo] | (v->regs[lo + 1] << 8));
}

/* Bitwise logical operation. LOP bit L<s><d> gives the result for a source
 * bit s and destination bit d, e.g. 0x0C = IMP (copy source). */
V9990_HOT static inline unsigned logop(uint8_t lop, unsigned s, unsigned d) {
    unsigned r = 0;
    if (lop & 0x8) r |=  s &  d;
    if (lop & 0x4) r |=  s & ~d;
    if (lop & 0x2) r |= ~s &  d;
    if (lop & 0x1) r |= ~s & ~d;
    return r;
}

/* Pattern modes use their own coordinate systems (openMSX, "TODO figure
 * out exactly how the coordinate system maps to vram in P1"): 4 bits per
 * dot, left dot in the high nibble;
 *   P1: 256-dot lines of 128 bytes, linear; X bit 9 selects VRAM1 (layer B)
 *   P2: 512-dot lines of 256 bytes, through the P2 address mapping
 * Bitmap modes: the image width and depth of R#6, bitmap mapping. */
V9990_HOT static unsigned cmd_mode(const v9990_t *v) {
    return V9990_DSPM(v->regs[V9990_R_SCREEN_MODE0]);    /* 0 P1, 1 P2, else bitmap */
}

V9990_HOT static unsigned cmd_bpp(const v9990_t *v) {
    return cmd_mode(v) <= 1 ? 4u : v9990_bpp(v);
}

V9990_HOT static unsigned cmd_width(const v9990_t *v) {
    switch (cmd_mode(v)) {
    case 0:  return 256;
    case 1:  return 512;
    default: return v9990_image_width(v);
    }
}

/* The VRAM byte holding dot (x, y) (not 16bpp): its address, the dot's
 * bit position in it and whether it is in VRAM1 (selects WM / FC bytes). */
V9990_HOT static uint8_t *dot_byte(const v9990_t *v, unsigned x, unsigned y,
                                   unsigned *shift, bool *vram1) {
    uint32_t phys;
    switch (cmd_mode(v)) {
    case 0:
        phys = ((((x >> 1) & 127u) + y * 128u) & 0x3FFFFu) | ((x & 0x200u) << 9);
        *shift = (x & 1u) ? 0u : 4u;
        break;
    case 1:
        phys = v9990_vram_transform_p2((((x >> 1) & 255u) + y * 256u) & V9990_VRAM_MASK);
        *shift = (x & 1u) ? 0u : 4u;
        break;
    default: {
        const unsigned bpp = v9990_bpp(v), iw = v9990_image_width(v);
        uint32_t bit = ((uint32_t)y * iw + (x & (iw - 1u))) * bpp;
        uint32_t addr = (bit >> 3) & V9990_VRAM_MASK;
        *shift = 8u - bpp - (bit & 7u);
        phys = v9990_vram_transform_bx(addr);
        break;
    }
    }
    *vram1 = (phys & 0x40000u) != 0;
    return v9990_vram_ptr(v, phys);
}

/* 16bpp bitmap: the dot's two bytes (low byte in VRAM0). */
V9990_HOT static uint32_t dot16_addr(const v9990_t *v, unsigned x, unsigned y) {
    const unsigned iw = v9990_image_width(v);
    return ((((uint32_t)y * iw + (x & (iw - 1u))) * 16u) >> 3) & V9990_VRAM_MASK;
}

V9990_HOT static void write_pixel(v9990_t *v, unsigned x, unsigned y, unsigned color) {
    const unsigned bpp = cmd_bpp(v);
    const uint8_t lop = v->regs[V9990_R_CMD_LOP];

    if ((lop & V9990_LOP_TP) && color == 0) return;

    if (bpp == 16) {
        uint32_t addr = dot16_addr(v, x, y);
        for (unsigned i = 0; i < 2; i++) {
            uint8_t wm = v->regs[V9990_R_CMD_WM0 + i];
            uint8_t d = v9990_vram_read_bx(v, addr + i);
            uint8_t s = (uint8_t)(color >> (8 * i));
            uint8_t r = (uint8_t)logop(lop, s, d);
            v9990_vram_write_bx(v, addr + i, (uint8_t)((d & ~wm) | (r & wm)));
        }
        return;
    }

    unsigned shift; bool hi;
    uint8_t *p = dot_byte(v, x, y, &shift, &hi);
    uint8_t wm = v->regs[V9990_R_CMD_WM0 + (hi ? 1 : 0)];   /* VRAM0 / VRAM1 */
    uint8_t mask = (uint8_t)((((1u << bpp) - 1u) << shift) & wm);
    uint8_t d = *p;
    uint8_t r = (uint8_t)(logop(lop, color << shift, d));
    *p = (uint8_t)((d & ~mask) | (r & mask));
}

/* Colour of dot (x, y) taken from a 16-bit FC/BC style register pair. */
V9990_HOT static unsigned color_at(const v9990_t *v, unsigned x, unsigned y, uint16_t reg) {
    const unsigned bpp = cmd_bpp(v);
    if (bpp == 16) return reg;
    unsigned shift; bool hi;
    (void)dot_byte(v, x, y, &shift, &hi);
    uint8_t byte = (uint8_t)(hi ? (reg >> 8) : reg);
    return (byte >> shift) & ((1u << bpp) - 1u);
}

V9990_HOT static unsigned read_pixel(const v9990_t *v, unsigned x, unsigned y) {
    const unsigned bpp = cmd_bpp(v);
    if (bpp == 16) {
        uint32_t addr = dot16_addr(v, x, y);
        return v9990_vram_read_bx(v, addr) | (v9990_vram_read_bx(v, addr + 1) << 8);
    }
    unsigned shift; bool hi;
    const uint8_t *p = dot_byte(v, x, y, &shift, &hi);
    return (*p >> shift) & ((1u << bpp) - 1u);
}

V9990_HOT static void finish(v9990_t *v) {
    v->cmd.op = 0;
    V9990_CLR_BITS(v->status, V9990_ST_CE | V9990_ST_TR);
    V9990_SET_BITS(v->int_flags, V9990_INT_CE);
}

/* Advance the destination cursor of a rectangle command.
 * Returns false when the rectangle is complete. */
V9990_HOT static bool advance_rect(v9990_t *v) {
    const uint8_t arg = v->regs[V9990_R_CMD_ARG];
    const int dx = (arg & V9990_ARG_DIX) ? -1 : 1;
    const int dy = (arg & V9990_ARG_DIY) ? -1 : 1;
    v->cmd.x = (uint16_t)(v->cmd.x + dx);
    v->cmd.src_x = (uint16_t)(v->cmd.src_x + dx);
    if (--v->cmd.remaining_x == 0) {
        if (--v->cmd.remaining_y == 0) return false;
        v->cmd.remaining_x = v->cmd.nx;
        v->cmd.x = v->cmd.dx;
        v->cmd.src_x = v->cmd.sx;
        v->cmd.y = (uint16_t)(v->cmd.y + dy);
        v->cmd.src_y = (uint16_t)(v->cmd.src_y + dy);
    }
    return true;
}

/* Write dot (x, y) with the colour from FC (bit = 1) or BC (bit = 0). */
V9990_HOT static void write_fc_bc(v9990_t *v, unsigned x, unsigned y, bool fc) {
    uint16_t reg = reg16(v, fc ? V9990_R_CMD_FC0 : V9990_R_CMD_BC0);
    write_pixel(v, x, y, color_at(v, x, y, reg));
}

/* One character bit (CMMC/CMMM); returns false when the rectangle is done. */
V9990_HOT static bool character_bit(v9990_t *v, bool bit) {
    write_fc_bc(v, v->cmd.x, v->cmd.y, bit);
    if (!advance_rect(v)) {
        finish(v);
        return false;
    }
    return true;
}

V9990_HOT void v9990_cmd_start(v9990_t *v) {
    v9990_cmd_state_t *c = &v->cmd;
    c->op = v->regs[V9990_R_CMD_OP] >> 4;
    c->sx = reg16(v, V9990_R_CMD_SX0) & 0x07FF;
    c->sy = reg16(v, V9990_R_CMD_SY0) & 0x0FFF;
    c->dx = reg16(v, V9990_R_CMD_DX0) & 0x07FF;
    c->dy = reg16(v, V9990_R_CMD_DY0) & 0x0FFF;
    c->nx = reg16(v, V9990_R_CMD_NX0) & 0x07FF;
    c->ny = reg16(v, V9990_R_CMD_NY0) & 0x0FFF;
    if (c->nx == 0) c->nx = 2048;
    if (c->ny == 0) c->ny = 4096;
    c->x = c->dx;
    c->y = c->dy;
    c->src_x = c->sx;
    c->src_y = c->sy;
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
    case V9990_CMD_LMMM:
        break;
    case V9990_CMD_CMMC:
        V9990_SET_BITS(v->status, V9990_ST_TR);
        break;
    case V9990_CMD_CMMM:
        /* SA: linear byte address in R#32, R#34, R#35 */
        c->src_addr = (v->regs[V9990_R_CMD_SX0] | (v->regs[V9990_R_CMD_SY0] << 8) |
                       ((v->regs[V9990_R_CMD_SY1] & 0x07u) << 16)) & V9990_VRAM_MASK;
        c->bits_left = 0;
        break;
    case V9990_CMD_LINE:
        /* As openMSX: NX dots along the major axis (ARG MAJ: 0 = X), NY the
         * minor length; raw 12-bit counts, the end is drawn too. State:
         * x = X position, y = DY, src_x = error term, src_y = dots drawn. */
        c->nx = reg16(v, V9990_R_CMD_NX0) & 0x0FFF;
        c->ny = reg16(v, V9990_R_CMD_NY0) & 0x0FFF;
        c->src_x = (uint16_t)((uint16_t)(c->nx - 1u) / 2u);
        c->src_y = 0;
        break;
    case V9990_CMD_PSET:
        write_fc_bc(v, c->dx, c->dy, true);
        finish(v);
        break;
    default:
        /* TODO: unimplemented command, end it immediately */
        finish(v);
        break;
    }
}

V9990_HOT bool v9990_cmd_step(v9990_t *v, unsigned budget) {
    if (v->cmd.op == V9990_CMD_LMMM) {
        while (budget--) {
            /* write_pixel skips colour 0 when TP is set */
            write_pixel(v, v->cmd.x, v->cmd.y, read_pixel(v, v->cmd.src_x, v->cmd.src_y));
            if (!advance_rect(v)) {
                finish(v);
                return false;
            }
        }
    }
    if (v->cmd.op == V9990_CMD_LMMV) {
        while (budget--) {
            write_fc_bc(v, v->cmd.x, v->cmd.y, true);
            if (!advance_rect(v)) {
                finish(v);
                return false;
            }
        }
    }
    if (v->cmd.op == V9990_CMD_LINE) {
        v9990_cmd_state_t *c = &v->cmd;
        const uint8_t arg = v->regs[V9990_R_CMD_ARG];
        const uint16_t tx = (arg & V9990_ARG_DIX) ? 0xFFFFu : 1u;
        const uint16_t ty = (arg & V9990_ARG_DIY) ? 0xFFFFu : 1u;
        const bool y_major = (arg & V9990_ARG_MAJ) != 0;
        const unsigned width = cmd_width(v);
        while (budget--) {
            write_fc_bc(v, c->x, c->y, true);
            if (y_major) c->y = (uint16_t)(c->y + ty);
            else         c->x = (uint16_t)(c->x + tx);
            if (c->src_x < c->ny) {
                c->src_x = (uint16_t)(c->src_x + c->nx);
                if (y_major) c->x = (uint16_t)(c->x + tx);
                else         c->y = (uint16_t)(c->y + ty);
            }
            c->src_x = (uint16_t)(c->src_x - c->ny);
            /* ends after NX + 1 dots, or when X leaves the image */
            if (c->src_y++ == c->nx || (c->x & width)) {
                /* DY keeps the last position (openMSX) */
                v->regs[V9990_R_CMD_DY0] = (uint8_t)c->y;
                v->regs[V9990_R_CMD_DY1] = (uint8_t)((c->y >> 8) & 0x0F);
                finish(v);
                return false;
            }
        }
    }
    if (v->cmd.op == V9990_CMD_CMMM) {
        while (budget--) {
            if (!v->cmd.bits_left) {
                v->cmd.bits = v9990_vram_read_bx(v, v->cmd.src_addr);
                v->cmd.src_addr = (v->cmd.src_addr + 1) & V9990_VRAM_MASK;
                v->cmd.bits_left = 8;
            }
            v->cmd.bits_left--;
            bool bit = (v->cmd.bits & 0x80) != 0;
            v->cmd.bits = (uint8_t)(v->cmd.bits << 1);
            if (!character_bit(v, bit)) return false;
        }
    }
    return v->cmd.op != 0;
}

V9990_HOT void v9990_cmd_write_data(v9990_t *v, uint8_t data) {
    if (v->cmd.op == V9990_CMD_CMMC) {
        for (unsigned i = 0; i < 8; i++, data = (uint8_t)(data << 1))
            if (!character_bit(v, (data & 0x80) != 0)) return;
        return;
    }
    if (v->cmd.op != V9990_CMD_LMMC) return;
    unsigned bpp = cmd_bpp(v);

    if (bpp == 16) {
        /* low byte first (manual 11.4) */
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

    /* 8/bpp dots per byte, leftmost dot in the MSBs (manual 11.4) */
    for (unsigned shift = 8; shift >= bpp; shift -= bpp) {
        write_pixel(v, v->cmd.x, v->cmd.y,
                    (data >> (shift - bpp)) & ((1u << bpp) - 1u));
        if (!advance_rect(v)) {
            finish(v);
            return;
        }
    }
}

V9990_HOT uint8_t v9990_cmd_read_data(v9990_t *v) {
    /* TODO: LMCM / POINT */
    return v->cmd_read_latch;
}
