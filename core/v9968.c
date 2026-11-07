/*
 * V9968 core: ports, registers, palette, status, interrupts, timing events.
 * Behaviour follows vdp_cpu_interface.v and vdp_timing_control_ssg.v.
 */
#include <string.h>
#include "v9968.h"

/* MSX2 power-on palette (3-bit RGB), the same for every 16-entry block. */
static const uint8_t default_palette[16][3] = {
    {0, 0, 0}, {0, 0, 0}, {1, 6, 1}, {3, 7, 3}, {1, 1, 7}, {2, 3, 7}, {5, 1, 1}, {2, 6, 7},
    {7, 1, 1}, {7, 3, 3}, {6, 6, 1}, {6, 6, 4}, {1, 4, 1}, {6, 2, 5}, {5, 5, 5}, {7, 7, 7},
};

static inline uint8_t expand3(uint8_t c) { return (uint8_t)((c << 2) | (c >> 1)); }

static void set_palette(v9968_t *v, unsigned i, unsigned r, unsigned g, unsigned b) {
    v->palette[i] = (uint16_t)((r << 10) | (g << 5) | b);
}

void v9968_init(v9968_t *v, uint8_t *vram) {
    memset(v, 0, sizeof(*v));
    v->vram = vram;
    v->gamma = true;
    memset(vram, 0, V9968_VRAM_SIZE);
    v9968_reset(v);
}

void v9968_reset(v9968_t *v) {
    uint8_t *vram = v->vram;
    bool gamma = v->gamma, rtl_lmcm = v->cmd.rtl_lmcm;
    memset(v, 0, sizeof(*v));
    v->vram = vram;
    v->gamma = gamma;
    v->cmd.rtl_lmcm = rtl_lmcm;
    v->regs[8] = 0x08;          /* VR = 1 (64K-bit DRAM type) */
    v->regs[21] = 0x01;         /* V58: V9958 compatible */
    v->lock_ext = true;
    v->blink_page = true;
    v->cmd.wex = 0x1FF;
    v->cmd.wey = 0x7FF;
    v->cmd.pixel_step = 1;
    for (unsigned i = 0; i < 256; i++) {
        const uint8_t *c = default_palette[i & 15];
        set_palette(v, i, expand3(c[0]), expand3(c[1]), expand3(c[2]));
    }
    geo3d_reset(&v->geo);
}

/* ------------------------------------------------------------------ */
/* Mode helpers                                                        */
/* ------------------------------------------------------------------ */

v9968_mode_t v9968_mode(const v9968_t *v) {
    unsigned m = ((v->regs[0] >> 1) & 7u) << 2 |       /* M5 M4 M3 */
                 ((v->regs[1] >> 3) & 1u) << 1 |       /* M2 */
                 ((v->regs[1] >> 4) & 1u);             /* M1 */
    switch (m) {
    case 0x00: return V9968_MODE_G1;
    case 0x04: return V9968_MODE_G2;
    case 0x08: return V9968_MODE_G3;
    case 0x0C: return V9968_MODE_G4;
    case 0x10: return V9968_MODE_G5;
    case 0x14: return V9968_MODE_G6;
    case 0x1C: return V9968_MODE_G7;
    case 0x01: case 0x05: return V9968_MODE_T1;
    case 0x09: return V9968_MODE_T2;
    case 0x02: case 0x06: return V9968_MODE_MC;
    default:   return V9968_MODE_NONE;
    }
}

bool v9968_interleave(const v9968_t *v) {
    v9968_mode_t m = v9968_mode(v);
    return !v9968_sp3(v) && (m == V9968_MODE_G6 || m == V9968_MODE_G7);
}

unsigned v9968_lines_per_field(const v9968_t *v) {
    return (v->regs[9] & 0x80) ? 212 : 192;
}

/* Interlace with even/odd pages (IL + EO) or the flat interlace (FIL) is
 * shown as one progressive picture of twice the lines. */
static bool interlaced_picture(const v9968_t *v) {
    return (v->regs[20] & 0x20) || ((v->regs[9] & 0x0C) == 0x0C);
}

V9968_HOT unsigned v9968_display_height(const v9968_t *v) {
    unsigned h = v9968_lines_per_field(v);
    return interlaced_picture(v) ? h * 2 : h;
}

/* ------------------------------------------------------------------ */
/* VRAM access from the CPU                                            */
/* ------------------------------------------------------------------ */

/* MXC (R#45 bit6 without the extended commands) hides VRAM from the CPU. */
static inline bool cpu_vram_masked(const v9968_t *v) {
    return v9968_v58(v) && (v->cmd.arg & 0x40);
}

static void advance_vram_addr(v9968_t *v) {
    if (v->vram_noinc) {
        v->vram_noinc = false;
        return;
    }
    uint32_t a = v->vram_addr;
    uint32_t n = a + 1;
    unsigned m = ((v->regs[0] >> 2) & 3u);              /* M5 M4 */
    if (m == 0 || !(v->regs[8] & 0x08))                 /* TMS9918 modes or VR=0 */
        a = (a & ~0x3FFFu) | (n & 0x3FFFu);
    else if (!v9968_v58(v))
        a = n & 0x3FFFFu;
    else
        a = n & 0x1FFFFu;
    v->vram_addr = a;
}

static uint8_t vram_peek(const v9968_t *v) {
    if (cpu_vram_masked(v)) return 0xFF;
    return v->vram[v9968_phys(v9968_interleave(v), v->vram_addr)];
}

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static void palette_write(v9968_t *v, uint8_t data) {
    if (v9968_epal(v)) {
        switch (v->pal_phase) {
        case 0: v->pal_r = data & 0x1F; v->pal_phase = 1; return;
        case 1: v->pal_g = data & 0x1F; v->pal_phase = 2; return;
        default:
            set_palette(v, v->pal_index, v->pal_r, v->pal_g, data & 0x1F);
            v->pal_phase = 0;
            v->pal_index++;
            return;
        }
    }
    if (v->pal_phase == 0) {
        v->latch = data;            /* shares the port 1 data latch */
        v->pal_phase = 1;
        return;
    }
    uint8_t r = (v->latch >> 4) & 7, b = v->latch & 7, g = data & 7;
    set_palette(v, v->pal_index, expand3(r), expand3(g), expand3(b));
    v->pal_phase = 0;
    v->pal_index = (v->pal_index + 1) & 15;
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static void write_reg(v9968_t *v, unsigned reg, uint8_t data, bool indirect) {
    reg &= 63;
    if (reg >= 32) {
        v9968_cmd_write_reg(v, reg, data);
        return;
    }
    bool v58 = v9968_v58(v);
    switch (reg) {
    case 0:
        v->regs[0] = data;
        V9968_CLR_BITS(v->int_flags, V9968_INT_LINE);
        break;
    case 2:  v->regs[2] = v58 ? (data & 0x7F) : data; break;
    case 4:  v->regs[4] = v58 ? (data & 0x3F) : (data & 0x7F); break;
    case 6:  v->regs[6] = v58 ? (data & 0x3F) : (data & 0x7F); break;
    case 10: v->regs[10] = v58 ? (data & 0x07) : (data & 0x0F); break;
    case 11: v->regs[11] = v58 ? (data & 0x03) : (data & 0x07); break;
    case 14:
        v->regs[14] = data;
        if (!(v->regs[8] & 0x08))
            v->vram_addr &= 0x3FFFu;
        else
            v->vram_addr = (v->vram_addr & 0x3FFFu) |
                           ((uint32_t)(data & (v58 ? 0x07 : 0x0F)) << 14);
        break;
    case 15: v->regs[15] = data & 0x0F; break;
    case 16:
        v->regs[16] = data;
        v->pal_index = v9968_epal(v) ? data : (data & 15);
        v->pal_phase = 0;
        break;
    case 17:
        if (indirect) break;                    /* port 3 cannot reach R#17 */
        v->regs[17] = data;
        v->reg_ptr = data & 0x3F;
        v->reg_noinc = data & 0x80;
        break;
    case 19:
        v->regs[19] = data;
        V9968_CLR_BITS(v->int_flags, V9968_INT_LINE);
        break;
    case 20:
        if (!v->lock_ext) v->regs[20] = data;
        break;
    case 21:
        if (v->lock_ext) break;
        v->regs[21] = data & 1;
        if (data & 1) {                         /* back to V9958: A17 = 0 */
            v->regs[2] &= 0x7F;
            v->regs[4] &= 0x3F;
            v->regs[6] &= 0x3F;
            v->regs[10] &= 0x07;
            v->regs[11] &= 0x03;
        }
        break;
    case 22: case 24:
        break;
    default:
        v->regs[reg] = data;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Status registers                                                    */
/* ------------------------------------------------------------------ */

static uint8_t status_peek(const v9968_t *v) {
    switch (v->regs[15]) {
    case 0:
        return (uint8_t)(((v->int_flags & V9968_INT_FRAME) ? 0x80 : 0) |
                         (v->sprite_status & 0x7F));
    case 1:
        return (uint8_t)((v9968_v58(v) ? 0x04 : 0x06) |
                         ((v->int_flags & V9968_INT_LINE) ? 1 : 0));
    case 2: {
        uint8_t s = 0x0C | (v->video_status & (V9968_S2_VR | V9968_S2_HR | V9968_S2_EO));
        if (v9968_cmd_busy(v))         s |= V9968_S2_CE;
        if (v->cmd.border_detect)      s |= V9968_S2_BD;
        if (v->cmd.transfer_ready)     s |= V9968_S2_TR;
        if (v->hr_toggle)              s ^= V9968_S2_HR;
        return s;
    }
    case 3: return (uint8_t)v->col_x;
    case 4: return (uint8_t)(0xFE | ((v->col_x >> 8) & 1));
    case 5: return (uint8_t)v->col_y;
    case 6: return (uint8_t)(0xFC | ((v->col_y >> 8) & 3));
    case 7: return v->cmd.status_color;
    case 8: return (uint8_t)(v->cmd.sx >> 8);           /* live SX (S#8/S#9) */
    case 9: return (uint8_t)(0xFE | ((v->cmd.sx >> 16) & 1));
    default: return 0xFF;
    }
}

static void status_read(v9968_t *v) {
    switch (v->regs[15]) {
    case 0:
        V9968_CLR_BITS(v->int_flags, V9968_INT_FRAME);
        V9968_CLR_BITS(v->sprite_status, V9968_S0_5S | V9968_S0_C);
        break;
    case 1:
        V9968_CLR_BITS(v->int_flags, V9968_INT_LINE);
        break;
#if !(defined(PICO_ON_DEVICE) && PICO_ON_DEVICE)
    case 2:
        /* No beam on the host: let HR polling loops make progress. */
        v->hr_toggle = !v->hr_toggle;
        break;
#endif
    case 5:
        v->col_x = 0;
        v->col_y = 0;
        break;
    case 7:
        v9968_cmd_read_color(v);
        break;
    case 9:
        v9968_cmd_clear_bd(v);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

V9968_HOT void v9968_write_port(v9968_t *v, uint8_t port, uint8_t data) {
    switch (port & 7) {
    case V9968_PORT_VRAM:
        v->second = false;
        advance_vram_addr(v);
        if (!cpu_vram_masked(v))
            v->vram[v9968_phys(v9968_interleave(v), v->vram_addr)] = data;
        break;
    case V9968_PORT_CTRL:
        if (!v->second) {
            v->latch = data;
            v->second = true;
            break;
        }
        v->second = false;
        if (data & 0x80) {
            write_reg(v, data & 0x3F, v->latch, false);
        } else {
            v->vram_addr = (v->vram_addr & ~0x3FFFu) |
                           ((uint32_t)(data & 0x3F) << 8) | v->latch;
            v->vram_noinc = (data & 0x40) != 0;
        }
        break;
    case V9968_PORT_PALETTE:
        palette_write(v, data);
        break;
    case V9968_PORT_INDIRECT: {
        unsigned reg = v->reg_ptr;
        if (!v->reg_noinc) v->reg_ptr = (v->reg_ptr + 1) & 0x3F;
        write_reg(v, reg, data, true);
        break;
    }
    case V9968_PORT_INT:
        V9968_CLR_BITS(v->int_flags, data & 7);
        v->lock_ext = (data & 0x80) != 0;
        break;
    case V9968_PORT_GEO_INDEX:
        geo3d_write(&v->geo, 0, data);
        break;
    case V9968_PORT_GEO_DATA:
        geo3d_write(&v->geo, 1, data);
        break;
    default:
        break;
    }
}

V9968_HOT uint8_t v9968_peek_port(const v9968_t *v, uint8_t port) {
    switch (port & 7) {
    case V9968_PORT_VRAM:      return vram_peek(v);
    case V9968_PORT_CTRL:      return status_peek(v);
    case V9968_PORT_INT:
        return (uint8_t)((v->lock_ext ? 0x80 : 0) | (v->int_flags & 7));
    case V9968_PORT_GEO_INDEX: return geo3d_status(&v->geo);
    case V9968_PORT_GEO_DATA:  return geo3d_peek_data(&v->geo);
    default:                   return 0xFF;
    }
}

V9968_HOT uint8_t v9968_read_port(v9968_t *v, uint8_t port) {
    uint8_t value = v9968_peek_port(v, port);
    switch (port & 7) {
    case V9968_PORT_VRAM:
        v->second = false;
        advance_vram_addr(v);
        break;
    case V9968_PORT_CTRL:
        v->second = false;
        status_read(v);
        break;
    case V9968_PORT_GEO_DATA:
        geo3d_read_data(&v->geo);
        break;
    case V9968_PORT_GEO_INDEX:
        break;
    default:
        v->second = false;
        break;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Video timing                                                        */
/* ------------------------------------------------------------------ */

V9968_HOT void v9968_video_vblank_start(v9968_t *v) {
    V9968_SET_BITS(v->video_status, V9968_S2_VR);
}

/* Start of a frame: field, blink and sprite shuffle bookkeeping. */
V9968_HOT void v9968_video_vblank_end(v9968_t *v) {
    V9968_CLR_BITS(v->video_status, V9968_S2_VR);
    v->field ^= 1;
    if (v->field) V9968_SET_BITS(v->video_status, V9968_S2_EO);
    else          V9968_CLR_BITS(v->video_status, V9968_S2_EO);

    /* blink: R#13 periods count in units of 5 frames (w_10frame) */
    uint8_t period = v->regs[13];
    bool tick = (v->blink_base == 4);
    v->blink_base = tick ? 0 : (uint8_t)(v->blink_base + 1);
    if (period == 0) {
        v->blink_counter = 0;
        v->blink_page = true;
    } else if (tick) {
        uint8_t next = v->blink_page ? (period >> 4) : (period & 15);
        if (v->blink_counter == 0) {
            v->blink_counter = next;
            if (next != 0) v->blink_page = !v->blink_page;
        } else {
            v->blink_counter--;
        }
    }

    if (!(v->regs[25] & 0x80))
        v->shuffle_start = 0;
    else if (v9968_interleave(v))
        v->shuffle_start = (uint8_t)(((v->shuffle_start + 18) & 0x3E));
    else
        v->shuffle_start = (uint8_t)((v->shuffle_start + 17) & 0x3F);
}

V9968_HOT void v9968_video_line(v9968_t *v, unsigned line) {
    unsigned field_lines = v9968_lines_per_field(v);
    if (interlaced_picture(v)) {
        if (line & 1) return;           /* both fields share one line time */
        line >>= 1;
    }
    v->cur_line = (uint16_t)line;

    /* The line interrupt of line N fires in the blanking after N, i.e.
     * just before N + 1 is drawn (vdp_timing_control_ssg.v). */
    if (line > 0) {
        unsigned prev = line - 1;
        unsigned y = (v->regs[20] & 0x04) ? prev : ((prev + v->regs[23]) & 0xFF);
        if ((y & 0xFF) == v->regs[19] && prev <= 234)
            V9968_SET_BITS(v->int_flags, V9968_INT_LINE);
    }
    /* The frame interrupt fires as the last line of the field starts. */
    if (line == field_lines - 1) {
        V9968_SET_BITS(v->int_flags, V9968_INT_FRAME);
        V9968_CLR_BITS(v->int_flags, V9968_INT_LINE);
    }
}
