/*
 * V9990 core: ports, registers, palette, mode decoding, timing events.
 */
#include <string.h>
#include "v9990.h"
#include "v9990_vram.h"

static uint16_t palette_to_rgb565(const v9990_palette_entry_t *p) {
    uint16_t r = p->r & 0x1F, g = p->g & 0x1F, b = p->b & 0x1F;
    return (uint16_t)((r << 11) | (g << 6) | ((g >> 4) << 5) | b);
}

void v9990_init_banks(v9990_t *v, uint8_t *vram0, uint8_t *vram1) {
    memset(v, 0, sizeof(*v));
    v->vram = vram0;
    v->vram1 = vram1;
    memset(vram0, 0, V9990_VRAM_SIZE / 2);
    memset(vram1, 0, V9990_VRAM_SIZE / 2);
    v9990_reset(v);
}

void v9990_init(v9990_t *v, uint8_t *vram) {
    v9990_init_banks(v, vram, vram + V9990_VRAM_SIZE / 2);
}

void v9990_reset(v9990_t *v) {
    uint8_t *vram = v->vram, *vram1 = v->vram1;
    uint8_t sys = v->sys_ctrl;
    void (*fetch)(const struct v9990 *, uint32_t, uint8_t *, uint32_t) = v->vram_fetch;
    memset(v, 0, sizeof(*v));
    v->vram = vram;
    v->vram1 = vram1;
    v->sys_ctrl = sys;
    v->vram_fetch = fetch;
    for (unsigned i = 0; i < V9990_PALETTE_ENTRIES; i++)
        v->palette_rgb565[i] = palette_to_rgb565(&v->palette[i]);
}

/* ------------------------------------------------------------------ */
/* VRAM pointers                                                       */
/* ------------------------------------------------------------------ */

static void update_write_ptr(v9990_t *v) {
    v->vram_write_ptr = (v->regs[V9990_R_VRAM_WRITE_ADDR0] |
                         (v->regs[V9990_R_VRAM_WRITE_ADDR1] << 8) |
                         ((v->regs[V9990_R_VRAM_WRITE_ADDR2] & 0x07u) << 16));
}

static void prefetch_read(v9990_t *v) {
    v->vram_read_latch = v9990_vram_read(v, v->vram_read_ptr);
}

static void update_read_ptr(v9990_t *v) {
    v->vram_read_ptr = (v->regs[V9990_R_VRAM_READ_ADDR0] |
                        (v->regs[V9990_R_VRAM_READ_ADDR1] << 8) |
                        ((v->regs[V9990_R_VRAM_READ_ADDR2] & 0x07u) << 16));
}

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static void palette_advance(v9990_t *v) {
    if (v->regs[V9990_R_PALETTE_CTRL] & 0x10) return;   /* PLTAIH */
    uint8_t p = (uint8_t)(v->regs[V9990_R_PALETTE_PTR] + 1);
    if ((p & 3) == 3) p++;
    v->regs[V9990_R_PALETTE_PTR] = p;
}

static uint8_t palette_peek(const v9990_t *v) {
    uint8_t p = v->regs[V9990_R_PALETTE_PTR];
    const v9990_palette_entry_t *e = &v->palette[p >> 2];
    switch (p & 3) {
    case 0:  return e->r;
    case 1:  return e->g;
    case 2:  return e->b;
    default: return 0;
    }
}

static void palette_write(v9990_t *v, uint8_t data) {
    uint8_t p = v->regs[V9990_R_PALETTE_PTR];
    v9990_palette_entry_t *e = &v->palette[p >> 2];
    switch (p & 3) {
    case 0: e->r = data & 0x9F; break;   /* bit7 = YS */
    case 1: e->g = data & 0x1F; break;
    case 2: e->b = data & 0x1F; break;
    default: break;
    }
    v->palette_rgb565[p >> 2] = palette_to_rgb565(e);
    palette_advance(v);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static const uint8_t reg_write_mask[32] = V9990_REG_WRITE_MASKS;

static void write_reg(v9990_t *v, uint8_t reg, uint8_t data) {
    reg &= V9990_REGSEL_MASK;
    if (reg < 32) data &= reg_write_mask[reg];
    v->regs[reg] = data;
    switch (reg) {
    case V9990_R_VRAM_WRITE_ADDR2:          /* pointer only updates on R#2 */
        update_write_ptr(v);
        break;
    case V9990_R_VRAM_READ_ADDR2:           /* pointer only updates on R#5 */
        update_read_ptr(v);
        prefetch_read(v);
        break;
    case V9990_R_SCROLL_AY_LO:              /* restart layer A at this line */
        v->line_offset_a = v->cur_line;
        break;
    case V9990_R_SCROLL_BY_LO:              /* restart layer B at this line */
        v->line_offset_b = v->cur_line;
        break;
    case V9990_R_CMD_OP:
        v9990_cmd_start(v);
        break;
    default:
        break;
    }
}

static uint8_t read_reg(const v9990_t *v, uint8_t reg) {
    reg &= V9990_REGSEL_MASK;
    /* TODO: model write-only registers and SRCH result registers. */
    return v->regs[reg];
}

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

V9990_HOT void v9990_write_port(v9990_t *v, uint8_t port, uint8_t data) {
    switch (port & 0x0F) {
    case V9990_PORT_VRAM_DATA:
        v9990_vram_write(v, v->vram_write_ptr, data);
        if (!(v->regs[V9990_R_VRAM_WRITE_ADDR2] & 0x80))      /* AII */
            v->vram_write_ptr = (v->vram_write_ptr + 1) & V9990_VRAM_MASK;
        break;
    case V9990_PORT_PALETTE_DATA:
        palette_write(v, data);
        break;
    case V9990_PORT_CMD_DATA:
        v9990_cmd_write_data(v, data);
        break;
    case V9990_PORT_REG_DATA:
        write_reg(v, v->reg_select, data);
        if (!(v->reg_select & V9990_REGSEL_WII))
            v->reg_select = (v->reg_select & 0xC0) |
                            ((v->reg_select + 1) & V9990_REGSEL_MASK);
        break;
    case V9990_PORT_REG_SELECT:
        v->reg_select = data;
        break;
    case V9990_PORT_INT_FLAG:
        V9990_CLR_BITS(v->int_flags, data);
        break;
    case V9990_PORT_SYSTEM_CTRL:
        v->sys_ctrl = data;
        if (data & V9990_SYS_SRS) v9990_reset(v);
        break;
    default:
        break;
    }
}

V9990_HOT uint8_t v9990_peek_port(const v9990_t *v, uint8_t port) {
    switch (port & 0x0F) {
    case V9990_PORT_VRAM_DATA:    return v->vram_read_latch;
    case V9990_PORT_PALETTE_DATA: return palette_peek(v);
    case V9990_PORT_CMD_DATA:     return v->cmd_read_latch;
    case V9990_PORT_REG_DATA:     return read_reg(v, v->reg_select);
    case V9990_PORT_STATUS:
        return (uint8_t)(v->status |
                         ((v->sys_ctrl & V9990_SYS_MCS) ? V9990_ST_MSC : 0));
    case V9990_PORT_INT_FLAG:     return v->int_flags;
    default:                      return 0xFF;
    }
}

V9990_HOT uint8_t v9990_read_port(v9990_t *v, uint8_t port) {
    uint8_t value = v9990_peek_port(v, port);
    switch (port & 0x0F) {
    case V9990_PORT_VRAM_DATA:
        if (!(v->regs[V9990_R_VRAM_READ_ADDR2] & 0x80))       /* AII */
            v->vram_read_ptr = (v->vram_read_ptr + 1) & V9990_VRAM_MASK;
        prefetch_read(v);
        break;
    case V9990_PORT_PALETTE_DATA:
        palette_advance(v);
        break;
    case V9990_PORT_CMD_DATA:
        (void)v9990_cmd_read_data(v);
        break;
    case V9990_PORT_REG_DATA:
        if (!(v->reg_select & V9990_REGSEL_RII))
            v->reg_select = (v->reg_select & 0xC0) |
                            ((v->reg_select + 1) & V9990_REGSEL_MASK);
        break;
    default:
        break;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Display mode decoding                                               */
/* ------------------------------------------------------------------ */

/* Display mode from R#6/R#7 and the MCS bit of P#7 (manual 8.3). */
V9990_HOT v9990_display_mode_t v9990_display_mode(const v9990_t *v) {
    uint8_t r6 = v->regs[V9990_R_SCREEN_MODE0];
    uint8_t r7 = v->regs[V9990_R_SCREEN_MODE1];
    bool mcs = (v->sys_ctrl & V9990_SYS_MCS) != 0;
    switch (V9990_DSPM(r6)) {
    case 0: return V9990_MODE_P1;
    case 1: return V9990_MODE_P2;
    case 3: return V9990_MODE_STANDBY;
    default: break;
    }
    if (r7 & V9990_R7_HSCN)                       /* 31kHz modes */
        return (r7 & V9990_R7_C25M) ? V9990_MODE_B6 : V9990_MODE_B5;
    switch (V9990_DCKM(r6)) {
    case 0:  return V9990_MODE_B1;                /* XTAL/4: 256 */
    case 1:  return mcs ? V9990_MODE_B2           /* MCKIN/2: 384 */
                        : V9990_MODE_B3;          /* XTAL/2: 512 */
    default: return mcs ? V9990_MODE_B4           /* MCKIN: 768 */
                        : V9990_MODE_B7;          /* XTAL: 1024 (undocumented) */
    }
}

V9990_HOT v9990_color_mode_t v9990_color_mode(const v9990_t *v) {
    uint8_t r6 = v->regs[V9990_R_SCREEN_MODE0];
    uint8_t r13 = v->regs[V9990_R_PALETTE_CTRL];
    bool yae = (r13 & 0x20) != 0;
    switch (V9990_CLRM(r6)) {
    case 0: return V9990_COLOR_BP2;
    case 1: return V9990_COLOR_BP4;
    case 3: return V9990_COLOR_BD16;
    default: break;
    }
    switch (r13 >> 6) {                 /* PLTM */
    case 0:  return V9990_COLOR_BP6;
    case 1:  return V9990_COLOR_BD8;
    case 2:  return yae ? V9990_COLOR_BYJKP : V9990_COLOR_BYJK;
    default: return yae ? V9990_COLOR_BYUVP : V9990_COLOR_BYUV;
    }
}

V9990_HOT unsigned v9990_bpp(const v9990_t *v) {
    return 2u << V9990_CLRM(v->regs[V9990_R_SCREEN_MODE0]);
}

V9990_HOT unsigned v9990_image_width(const v9990_t *v) {
    return 256u << V9990_XIMM(v->regs[V9990_R_SCREEN_MODE0]);
}

V9990_HOT unsigned v9990_display_width(const v9990_t *v) {
    switch (v9990_display_mode(v)) {
    case V9990_MODE_P1: case V9990_MODE_B1: return 256;
    case V9990_MODE_B2: return 384;
    case V9990_MODE_P2: case V9990_MODE_B3: return 512;
    case V9990_MODE_B4: return 768;
    case V9990_MODE_B5: case V9990_MODE_B6: return 640;
    case V9990_MODE_B7: return 1024;
    default: return 256;
    }
}

V9990_HOT unsigned v9990_display_height(const v9990_t *v) {
    unsigned h;
    switch (v9990_display_mode(v)) {
    case V9990_MODE_B5: return 400;
    case V9990_MODE_B6: return 480;
    case V9990_MODE_B2: case V9990_MODE_B4: h = 240; break;
    default: h = 212; break;
    }
    /* EO + IL: twice the vertical resolution (not in B5/B6) */
    if ((v->regs[V9990_R_SCREEN_MODE1] & (V9990_R7_IL | V9990_R7_EO)) ==
        (V9990_R7_IL | V9990_R7_EO))
        h *= 2;
    return h;
}

/* ------------------------------------------------------------------ */
/* Video timing events                                                 */
/* ------------------------------------------------------------------ */

V9990_HOT void v9990_video_vblank_start(v9990_t *v) {
    V9990_SET_BITS(v->status, V9990_ST_VR);
    V9990_SET_BITS(v->int_flags, V9990_INT_VI);
}

V9990_HOT void v9990_video_vblank_end(v9990_t *v) {
    /* new frame: line counters restart, high scroll bytes are latched */
    v->frame++;
    v->cur_line = 0;
    v->line_offset_a = 0;
    v->line_offset_b = 0;
    v->scroll_ay_high = v->regs[V9990_R_SCROLL_AY_HI];
    v->scroll_by_high = v->regs[V9990_R_SCROLL_BY_HI];
    V9990_CLR_BITS(v->status, V9990_ST_VR);
    if (v->regs[V9990_R_SCREEN_MODE1] & V9990_R7_EO)
        V9990_XOR_BITS(v->status, V9990_ST_EO);
}

V9990_HOT void v9990_video_line_start(v9990_t *v, unsigned line) {
    v->cur_line = (uint16_t)line;
    unsigned il = v->regs[V9990_R_INT_LINE_LO] |
                  ((v->regs[V9990_R_INT_LINE_HI] & 0x03u) << 8);
    /* TODO: IEHM / horizontal position (R#12), HR status bit */
    if (line == il) V9990_SET_BITS(v->int_flags, V9990_INT_HI);
}
