/*
 * Command engine (blitter).
 *
 * All sixteen commands: STOP, LMMC, LMMV, LMCM, LMMM, CMMC, CMMK, CMMM,
 * BMXL, BMLX, BMLL, LINE, SRCH, POINT, PSET, ADVN. CMMK draws characters
 * from a Kanji ROM, which a GFX9000 does not have: it ends at once (as in
 * openMSX). PSET and ADVN move the pointer (DX, DY) by R#52's AXE/AXM and
 * AYE/AYM (manual: "the drawing point can be shifted after executing the
 * command"; openMSX leaves it). BMLL ignores DIX/DIY, as openMSX does.
 * P1/P2 use the coordinate systems openMSX implements (not documented).
 * TODO: realistic execution timing.
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
    v->cmd.end_after_read = false;
    v->cmd_read_latch = 0xFF;
    V9990_CLR_BITS(v->status, V9990_ST_CE | V9990_ST_TR);
    V9990_SET_BITS(v->int_flags, V9990_INT_CE);
}

V9990_HOT static bool advance_rect(v9990_t *v);

/* Byte addresses of BMXL/BMLX/BMLL: SA in R#32, R#34, R#35; DA in R#36,
 * R#38, R#39; NA in R#40, R#42, R#43 (bits 7-0, 15-8, 18-16). */
V9990_HOT static uint32_t reg_addr(const v9990_t *v, unsigned lo) {
    return (v->regs[lo] | ((uint32_t)v->regs[lo + 2] << 8) |
            ((uint32_t)(v->regs[lo + 3] & 0x07u) << 16)) & V9990_VRAM_MASK;
}

/* A logical operation on a byte of 8/bpp dots; TP keeps the destination
 * dots whose source dot is 0. */
V9990_HOT static uint8_t logop_byte(uint8_t lop, uint8_t s, uint8_t d, unsigned bpp) {
    uint8_t r = (uint8_t)logop(lop, s, d);
    if (lop & V9990_LOP_TP) {
        const unsigned m = (1u << bpp) - 1u;
        for (unsigned sh = 0; sh < 8; sh += bpp)
            if (!((s >> sh) & m)) r = (uint8_t)((r & ~(m << sh)) | (d & (m << sh)));
    }
    return r;
}

/* The next byte of LMCM (the source rectangle, dots packed as LMMC takes
 * them, low byte first at 16bpp) or the second of POINT at 16bpp. */
V9990_HOT static void read_next(v9990_t *v) {
    v9990_cmd_state_t *c = &v->cmd;
    const unsigned bpp = cmd_bpp(v);
    if (bpp == 16) {
        if (c->data_phase) {                    /* the pixel's high byte */
            v->cmd_read_latch = (uint8_t)(c->data_word >> 8);
            c->data_phase = 0;
            if (c->op == V9990_CMD_POINT) c->end_after_read = true;
            return;
        }
        c->data_word = (uint16_t)read_pixel(v, c->src_x, c->src_y);
        v->cmd_read_latch = (uint8_t)c->data_word;
        c->data_phase = 1;
        if (!advance_rect(v)) c->remaining_y = 0;     /* done after the high byte */
        return;
    }
    uint8_t d = 0;
    for (unsigned shift = 8; shift >= bpp; shift -= bpp) {
        d |= (uint8_t)(read_pixel(v, c->src_x, c->src_y) << (shift - bpp));
        if (!advance_rect(v)) { c->end_after_read = true; break; }
    }
    v->cmd_read_latch = d;
}

/* After PSET / ADVN: R#52 AXE (bit 0) moves DX one dot, right or (AXM,
 * bit 1) left; AYE (bit 2) moves DY, down or (AYM, bit 3) up. */
V9990_HOT static void advance_pointer(v9990_t *v) {
    const uint8_t op = v->regs[V9990_R_CMD_OP];
    uint16_t dx = v->cmd.dx, dy = v->cmd.dy;
    if (op & 0x01) dx = (uint16_t)((dx + ((op & 0x02) ? 0x7FFu : 1u)) & 0x7FFu);
    if (op & 0x04) dy = (uint16_t)((dy + ((op & 0x08) ? 0xFFFu : 1u)) & 0xFFFu);
    v->regs[V9990_R_CMD_DX0] = (uint8_t)dx;
    v->regs[V9990_R_CMD_DX1] = (uint8_t)(dx >> 8);
    v->regs[V9990_R_CMD_DY0] = (uint8_t)dy;
    v->regs[V9990_R_CMD_DY1] = (uint8_t)(dy >> 8);
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
    c->end_after_read = false;
    v->cmd_read_latch = 0xFF;

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
        advance_pointer(v);
        finish(v);
        break;
    case V9990_CMD_ADVN:
        advance_pointer(v);
        finish(v);
        break;
    case V9990_CMD_LMCM:                        /* VRAM -> CPU through P#2 */
        read_next(v);
        V9990_SET_BITS(v->status, V9990_ST_TR);
        break;
    case V9990_CMD_POINT:                       /* the colour code of (SX, SY) */
        c->data_word = (uint16_t)read_pixel(v, c->sx, c->sy);
        v->cmd_read_latch = (uint8_t)c->data_word;
        if (cmd_bpp(v) == 16) c->data_phase = 1;    /* then the high byte */
        else                  c->end_after_read = true;
        V9990_SET_BITS(v->status, V9990_ST_TR);
        break;
    case V9990_CMD_SRCH:                        /* x: the dot being looked at */
        c->x = c->sx;
        c->y = c->sy;
        break;
    case V9990_CMD_BMXL:                        /* linear SA -> rectangle at DX, DY */
        c->src_addr = reg_addr(v, V9990_R_CMD_SX0);
        break;
    case V9990_CMD_BMLX:                        /* rectangle at SX, SY -> linear DA */
        c->dst_addr = reg_addr(v, V9990_R_CMD_DX0);
        break;
    case V9990_CMD_BMLL:                        /* linear SA -> linear DA, NA bytes */
        c->src_addr = reg_addr(v, V9990_R_CMD_SX0);
        c->dst_addr = reg_addr(v, V9990_R_CMD_DX0);
        c->count = reg_addr(v, V9990_R_CMD_NX0);
        if (c->count == 0) c->count = 0x80000;
        if (cmd_bpp(v) == 16) {                 /* words: VRAM0 low, VRAM1 high byte */
            c->src_addr >>= 1;
            c->dst_addr >>= 1;
            c->count >>= 1;
        }
        break;
    default:                                    /* CMMK: no Kanji ROM */
        finish(v);
        break;
    }
}

const volatile uint32_t *v9990_cmd_watch;
bool v9990_cmd_reference;

/* LMMV / LMMM in the bitmap modes, the per-dot code's results with its
 * per-command constants worked out once: a dot's byte (Bx layout: even
 * bytes in VRAM0, odd in VRAM1), its bits, FC and WM by bank, the logical
 * operation as masks. Returns the dots done, or less with the command
 * finished. */
#define FAST_YIELD_MIN 32u
V9990_HOT static unsigned rect_fast(v9990_t *v, unsigned budget) {
    v9990_cmd_state_t *c = &v->cmd;
    const bool copy = c->op == V9990_CMD_LMMM;
    const unsigned bpp = v9990_bpp(v), iw = v9990_image_width(v), xmask = iw - 1u;
    const uint8_t lop = v->regs[V9990_R_CMD_LOP];
    const bool tp = (lop & V9990_LOP_TP) != 0;
    /* logop as masks: r = s&d&m8 | s&~d&m4 | ~s&d&m2 | ~s&~d&m1 */
    const unsigned m8 = (lop & 8) ? 0xFFFFu : 0, m4 = (lop & 4) ? 0xFFFFu : 0;
    const unsigned m2 = (lop & 2) ? 0xFFFFu : 0, m1 = (lop & 1) ? 0xFFFFu : 0;
    const uint8_t wm[2] = { v->regs[V9990_R_CMD_WM0], v->regs[V9990_R_CMD_WM1] };
    const uint16_t fc = reg16(v, V9990_R_CMD_FC0);
    const uint8_t fcb[2] = { (uint8_t)fc, (uint8_t)(fc >> 8) };
    const uint8_t arg = v->regs[V9990_R_CMD_ARG];
    const uint16_t sdx = (arg & V9990_ARG_DIX) ? 0xFFFFu : 1u, sdy = (arg & V9990_ARG_DIY) ? 0xFFFFu : 1u;
    uint8_t *const bank[2] = { v->vram, v->vram1 };
    const volatile uint32_t *watch = v9990_cmd_watch;
    const uint32_t watch_val = watch ? *watch : 0;
    unsigned done = 0;
#define LOGOP(s, d) ((((s) & (d)) & m8) | (((s) & ~(d)) & m4) | ((~(s) & (d)) & m2) | ((~(s) & ~(d)) & m1))

    if (bpp == 16) {
        while (done < budget) {
            unsigned color;
            if (copy) {
                uint32_t sa = (((uint32_t)c->src_y * iw + (c->src_x & xmask)) * 2u) & V9990_VRAM_MASK;
                color = bank[0][sa >> 1] | (bank[1][sa >> 1] << 8);
            } else {
                color = fc;
            }
            if (!(tp && color == 0)) {
                uint32_t a = (((uint32_t)c->y * iw + (c->x & xmask)) * 2u) & V9990_VRAM_MASK;
                for (unsigned i = 0; i < 2; i++) {
                    uint8_t *p = bank[i] + (a >> 1);
                    unsigned d = *p, s = (color >> (8 * i)) & 0xFFu;
                    unsigned r = LOGOP(s, d);
                    *p = (uint8_t)((d & ~wm[i]) | (r & wm[i]));
                }
            }
            done++;
            c->x = (uint16_t)(c->x + sdx);
            c->src_x = (uint16_t)(c->src_x + sdx);
            if (--c->remaining_x == 0) {
                if (--c->remaining_y == 0) { finish(v); return done; }
                c->remaining_x = c->nx;
                c->x = c->dx;
                c->src_x = c->sx;
                c->y = (uint16_t)(c->y + sdy);
                c->src_y = (uint16_t)(c->src_y + sdy);
            }
            if (watch && done >= FAST_YIELD_MIN && (done & 15u) == 0 && *watch != watch_val) break;
        }
        return done;
    }

    const unsigned pm = (1u << bpp) - 1u;
    while (done < budget) {
        uint32_t bit = ((uint32_t)c->y * iw + (c->x & xmask)) * bpp;
        uint32_t a = (bit >> 3) & V9990_VRAM_MASK;
        unsigned shift = 8u - bpp - (bit & 7u);
        unsigned color;
        if (copy) {
            uint32_t sbit = ((uint32_t)c->src_y * iw + (c->src_x & xmask)) * bpp;
            uint32_t sa = (sbit >> 3) & V9990_VRAM_MASK;
            color = (bank[sa & 1u][sa >> 1] >> (8u - bpp - (sbit & 7u))) & pm;
        } else {
            color = (fcb[a & 1u] >> shift) & pm;
        }
        if (!(tp && color == 0)) {
            uint8_t *p = bank[a & 1u] + (a >> 1);
            unsigned d = *p, s = color << shift;
            unsigned r = LOGOP(s, d);
            unsigned mask = (pm << shift) & wm[a & 1u];
            *p = (uint8_t)((d & ~mask) | (r & mask));
        }
        done++;
        c->x = (uint16_t)(c->x + sdx);
        c->src_x = (uint16_t)(c->src_x + sdx);
        if (--c->remaining_x == 0) {
            if (--c->remaining_y == 0) { finish(v); return done; }
            c->remaining_x = c->nx;
            c->x = c->dx;
            c->src_x = c->sx;
            c->y = (uint16_t)(c->y + sdy);
            c->src_y = (uint16_t)(c->src_y + sdy);
        }
        if (watch && done >= FAST_YIELD_MIN && (done & 15u) == 0 && *watch != watch_val) break;
    }
#undef LOGOP
    return done;
}

V9990_HOT bool v9990_cmd_fast(const v9990_t *v) {
    return (v->cmd.op == V9990_CMD_LMMV || v->cmd.op == V9990_CMD_LMMM) && cmd_mode(v) >= 2 &&
           !v9990_cmd_reference;
}

V9990_HOT bool v9990_cmd_step(v9990_t *v, unsigned budget) {
    if (v9990_cmd_fast(v)) {
        rect_fast(v, budget);
        return v->cmd.op != 0;
    }
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
    if (v->cmd.op == V9990_CMD_SRCH) {
        v9990_cmd_state_t *c = &v->cmd;
        const uint16_t tx = (v->regs[V9990_R_CMD_ARG] & V9990_ARG_DIX) ? 0xFFFFu : 1u;
        const bool neq = (v->regs[V9990_R_CMD_ARG] & V9990_ARG_NEQ) != 0;
        const uint16_t fc = reg16(v, V9990_R_CMD_FC0);
        const unsigned width = cmd_width(v);
        while (budget--) {
            /* found: the border colour (NEQ: any other colour) at x */
            bool eq = read_pixel(v, c->x, c->y) == color_at(v, c->x, c->y, fc);
            bool found = eq != neq;
            if (!found) c->x = (uint16_t)(c->x + tx);
            if (found || (c->x & width)) {
                if (found) V9990_SET_BITS(v->status, V9990_ST_BD);
                else       V9990_CLR_BITS(v->status, V9990_ST_BD);
                v->regs[V9990_R_CMD_BX0] = (uint8_t)c->x;
                v->regs[V9990_R_CMD_BX1] = (uint8_t)((c->x >> 8) & 0x07);
                finish(v);
                return false;
            }
        }
    }
    if (v->cmd.op == V9990_CMD_BMXL) {
        v9990_cmd_state_t *c = &v->cmd;
        const unsigned bpp = cmd_bpp(v);
        while (budget--) {
            if (bpp == 16) {
                unsigned col = v9990_vram_read_bx(v, c->src_addr) |
                               (v9990_vram_read_bx(v, (c->src_addr + 1) & V9990_VRAM_MASK) << 8);
                c->src_addr = (c->src_addr + 2) & V9990_VRAM_MASK;
                write_pixel(v, c->x, c->y, col);
                if (!advance_rect(v)) { finish(v); return false; }
                continue;
            }
            uint8_t d = v9990_vram_read_bx(v, c->src_addr);
            c->src_addr = (c->src_addr + 1) & V9990_VRAM_MASK;
            for (unsigned shift = 8; shift >= bpp; shift -= bpp) {
                write_pixel(v, c->x, c->y, (d >> (shift - bpp)) & ((1u << bpp) - 1u));
                if (!advance_rect(v)) { finish(v); return false; }
            }
        }
    }
    if (v->cmd.op == V9990_CMD_BMLX) {
        v9990_cmd_state_t *c = &v->cmd;
        const unsigned bpp = cmd_bpp(v);
        while (budget--) {
            if (bpp == 16) {
                unsigned col = read_pixel(v, c->src_x, c->src_y);
                v9990_vram_write_bx(v, c->dst_addr, (uint8_t)col);
                v9990_vram_write_bx(v, (c->dst_addr + 1) & V9990_VRAM_MASK, (uint8_t)(col >> 8));
                c->dst_addr = (c->dst_addr + 2) & V9990_VRAM_MASK;
                if (!advance_rect(v)) { finish(v); return false; }
                continue;
            }
            uint8_t d = 0;
            bool done = false;
            for (unsigned shift = 8; shift >= bpp; shift -= bpp) {
                d |= (uint8_t)(read_pixel(v, c->src_x, c->src_y) << (shift - bpp));
                if (!advance_rect(v)) { done = true; break; }
            }
            v9990_vram_write_bx(v, c->dst_addr, d);   /* the last byte too, zero-filled */
            c->dst_addr = (c->dst_addr + 1) & V9990_VRAM_MASK;
            if (done) { finish(v); return false; }
        }
    }
    if (v->cmd.op == V9990_CMD_BMLL) {
        v9990_cmd_state_t *c = &v->cmd;
        const unsigned bpp = cmd_bpp(v);
        const uint8_t lop = v->regs[V9990_R_CMD_LOP];
        while (budget--) {
            if (bpp == 16) {                    /* VRAM0 = low byte, VRAM1 = high */
                uint8_t *s0 = v9990_vram_ptr(v, c->src_addr), *s1 = v9990_vram_ptr(v, c->src_addr + 0x40000u);
                uint8_t *d0 = v9990_vram_ptr(v, c->dst_addr), *d1 = v9990_vram_ptr(v, c->dst_addr + 0x40000u);
                uint16_t s = (uint16_t)(*s0 | (*s1 << 8)), d = (uint16_t)(*d0 | (*d1 << 8));
                uint16_t r = d;
                if (!((lop & V9990_LOP_TP) && s == 0)) r = (uint16_t)logop(lop, s, d);
                uint16_t wm = reg16(v, V9990_R_CMD_WM0);
                r = (uint16_t)((d & ~wm) | (r & wm));
                *d0 = (uint8_t)r;
                *d1 = (uint8_t)(r >> 8);
                c->src_addr = (c->src_addr + 1) & 0x3FFFFu;
                c->dst_addr = (c->dst_addr + 1) & 0x3FFFFu;
            } else {                            /* bytes in the bitmap (Bx) layout */
                uint8_t s = v9990_vram_read_bx(v, c->src_addr);
                uint32_t phys = v9990_vram_transform_bx(c->dst_addr);
                uint8_t *p = v9990_vram_ptr(v, phys);
                uint8_t wm = v->regs[V9990_R_CMD_WM0 + ((phys & 0x40000u) ? 1 : 0)];
                uint8_t r = logop_byte(lop, s, *p, bpp);
                *p = (uint8_t)((*p & ~wm) | (r & wm));
                c->src_addr = (c->src_addr + 1) & V9990_VRAM_MASK;
                c->dst_addr = (c->dst_addr + 1) & V9990_VRAM_MASK;
            }
            if (--c->count == 0) { finish(v); return false; }
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

/* A read of P#2: the latched byte goes to the CPU (v9990_peek_port); the
 * next one is latched, or the command ends after its last byte. */
V9990_HOT uint8_t v9990_cmd_read_data(v9990_t *v) {
    uint8_t value = v->cmd_read_latch;
    if (!(v->status & V9990_ST_TR)) return value;
    if (v->cmd.op != V9990_CMD_LMCM && v->cmd.op != V9990_CMD_POINT) return value;
    if (v->cmd.end_after_read) {
        finish(v);
        return value;
    }
    if (v->cmd.op == V9990_CMD_LMCM && v->cmd.data_phase == 0 && v->cmd.remaining_y == 0) {
        finish(v);                              /* 16bpp: that was the last high byte */
        return value;
    }
    read_next(v);
    return value;
}
