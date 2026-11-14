/*
 * Portable V9968 core (HRA!'s MSX2++ VDP, a V9958 superset) with the geo3d
 * coprocessor of the V9968 cartridge. No Pico SDK dependencies.
 *
 * Ports, relative to the cartridge's 8-port block (88h on the cartridge):
 *   0 VRAM data   1 control/status   2 palette   3 indirect register
 *   4 V9968 interrupt control         5 geo3d index/status   7 geo3d data
 *
 * Threading model on the RP2350 (same as the V9990 core):
 *   core1 : v9968_write_port / v9968_read_port, v9968_cmd_step
 *   core0 : v9968_render_line, v9968_video_* (timing events)
 * Only core1 mutates registers; core0 reads them and sets the status and
 * interrupt flags with atomic bit operations.
 *
 * Reference: HRA!'s RTL (hra1129/V9968_Cartridge, fpga/.../src/v9968) and
 * the V9968 programmer's manual; geo3d follows the bit-exact Python model of
 * alexmoncks/V9968_Cartridge (branch geo3d-v2.0, geo3d/sim/gen_scenes.py).
 *
 * Follows HRA!'s V9968 RTL (Copyright (C) 2025 Takayuki Hara) and is
 * distributed under its conditions, including the non-commercial clause;
 * see THIRD_PARTY_NOTICES.md.
 */
#ifndef V9968_H
#define V9968_H

#include <stdbool.h>
#include <stdint.h>
#include "geo3d.h"

#ifndef V9968_HOT
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define V9968_HOT __attribute__((section(".time_critical.v9968")))
#else
#define V9968_HOT
#endif
#endif

#define V9968_VRAM_SIZE   0x40000u          /* 256KB */
#define V9968_VRAM_MASK   (V9968_VRAM_SIZE - 1u)

#define V9968_SET_BITS(field, mask) \
    ((void)__atomic_fetch_or(&(field), (uint8_t)(mask), __ATOMIC_RELAXED))
#define V9968_CLR_BITS(field, mask) \
    ((void)__atomic_fetch_and(&(field), (uint8_t)~(mask), __ATOMIC_RELAXED))

/* Ports */
#define V9968_PORT_VRAM        0
#define V9968_PORT_CTRL        1
#define V9968_PORT_PALETTE     2
#define V9968_PORT_INDIRECT    3
#define V9968_PORT_INT         4
#define V9968_PORT_GEO_INDEX   5
#define V9968_PORT_GEO_DATA    7

/* Interrupt sources (port 4 bits) */
#define V9968_INT_FRAME        0x01     /* F  : S#0 bit7 */
#define V9968_INT_LINE         0x02     /* FH : S#1 bit0 */
#define V9968_INT_CMD_END      0x04     /* CE end (V9968) */

/* S#0 sprite bits kept by the renderer */
#define V9968_S0_5S            0x40
#define V9968_S0_C             0x20

/* S#2 bits */
#define V9968_S2_CE            0x01
#define V9968_S2_EO            0x02
#define V9968_S2_BD            0x10
#define V9968_S2_HR            0x20
#define V9968_S2_VR            0x40
#define V9968_S2_TR            0x80

/* Screen modes ({M5,M4,M3,M2,M1}, decoded) */
typedef enum {
    V9968_MODE_G1, V9968_MODE_G2, V9968_MODE_G3, V9968_MODE_G4,
    V9968_MODE_G5, V9968_MODE_G6, V9968_MODE_G7,
    V9968_MODE_T1, V9968_MODE_T2, V9968_MODE_MC,
    V9968_MODE_NONE
} v9968_mode_t;

/* Command engine state (see v9968_cmd.c; names follow vdp_command.v) */
typedef struct {
    /* registers as the RTL keeps them */
    uint16_t reg_sx;        /* 12 bits */
    uint32_t sy;            /* 13.8 fixed point, 21 bits (written by R#34/35) */
    uint16_t reg_dx;        /* 9 bits */
    uint16_t dy;            /* 11 bits (written by R#38/39) */
    uint16_t reg_nx, reg_ny;/* 11 bits */
    uint8_t  color;         /* R#44 */
    uint8_t  arg;           /* R#45 */
    uint8_t  op;            /* R#46 high nibble of the running command */
    uint8_t  lop;
    uint16_t vx, vy;        /* LRMM unit vector, signed 8.8 */
    uint16_t wsx, wex;      /* LRMM window, 9 bits */
    uint16_t wsy, wey;      /* 11 bits */
    uint32_t font_addr;     /* LFMM: 18-bit linear source (R#32-R#34) */

    /* execution */
    uint8_t  state;
    uint8_t  pixel_step;    /* 1, 2 or 4 */
    bool     wide;          /* 512-pixel mode at start */
    uint32_t sx, sx2, sy2;  /* 12.8 / 13.8 fixed point */
    uint16_t dx;
    uint16_t nx, ny, nyb;
    uint8_t  bit_count;
    uint8_t  source;        /* pixel to write (after the window/font) */
    uint8_t  fore_color;
    uint8_t  font_byte;
    bool     transfer_ready;/* TR */
    bool     read_color;    /* S#7 was read during LMCM */
    bool     border_detect; /* BD */
    uint8_t  status_color;  /* S#7 (S#8/S#9 show SX itself) */
    bool     rtl_lmcm;      /* LMCM reads (DX, DY) like vdp_command.v (tests) */
    bool     lmcm_lag;      /* rtl_lmcm: the read uses the counters before the */
    uint16_t lmcm_dx, lmcm_dy;  /* last advance, as in the RTL */
} v9968_cmd_state_t;

typedef struct v9968 {
    uint8_t  *vram;          /* 256KB, physical layout (G6/G7 interleave) */

    uint8_t  regs[64];       /* R#0-R#27 as stored (A17 masks applied) */
    uint8_t  latch;          /* first byte of a port 1 pair / palette */
    bool     second;         /* port 1: waiting for the second byte */
    uint8_t  reg_ptr;        /* R#17 */
    bool     reg_noinc;      /* R#17 bit7 */
    bool     lock_ext;       /* port 4 bit7: R#20/R#21 locked */

    uint32_t vram_addr;
    bool     vram_noinc;     /* first access after a write address */

    uint8_t  pal_index, pal_phase, pal_r, pal_g;
    uint16_t palette[256];   /* 0RRRRRGGGGGBBBBB, 5 bits each */
    volatile uint32_t pal_gen;  /* counts palette writes (renderer colour cache) */
    bool     gamma;          /* apply the cartridge's output gamma */

    volatile uint8_t int_flags;   /* V9968_INT_* */
    volatile uint8_t sprite_status; /* S#0 bits 6..0 (5S, C, 5th number) */
    volatile uint16_t col_x, col_y; /* S#3-S#6 */
    volatile uint8_t video_status;  /* S#2 VR/HR/EO, written by the video side */

    v9968_cmd_state_t cmd;

    /* video timing state (core0) */
    volatile uint16_t cur_line;    /* display line being rendered */
    uint8_t  field;                /* toggles every frame */
    uint8_t  blink_base, blink_counter;
    bool     blink_page;           /* ff_interleaving_page (1 = "on" period) */
    uint8_t  shuffle_start;        /* sprite priority shuffle (R#25 SPS) */
    bool     hr_toggle;            /* host builds: HR flips on every S#2 read */

    geo3d_t *geo;                  /* the caller's (~45KB, so copies of the state stay small) */
} v9968_t;

/* ---- lifecycle ---- */
void v9968_init(v9968_t *v, uint8_t *vram, geo3d_t *geo);
void v9968_reset(v9968_t *v);

/* ---- bus interface (port = offset 0..7 in the cartridge block) ---- */
V9968_HOT void    v9968_write_port(v9968_t *v, uint8_t port, uint8_t data);
V9968_HOT uint8_t v9968_read_port(v9968_t *v, uint8_t port);
/* a read the CPU was answered with `value` (the firmware's read table) */
V9968_HOT uint8_t v9968_read_port_seen(v9968_t *v, uint8_t port, uint8_t value);
V9968_HOT uint8_t v9968_peek_port(const v9968_t *v, uint8_t port);

static inline bool v9968_irq_pending(const v9968_t *v) {
    uint8_t en = (uint8_t)(((v->regs[1] >> 5) & 1) |          /* IE0 */
                           ((v->regs[0] >> 3) & 2) |          /* IE1 */
                           ((v->regs[20] >> 4) & 4));         /* CEIE */
    return (v->int_flags & en) != 0;
}

/* ---- register helpers ---- */
v9968_mode_t v9968_mode(const v9968_t *v);
static inline bool v9968_v58(const v9968_t *v)  { return v->regs[21] & 1; }
static inline bool v9968_sp3(const v9968_t *v)  { return v->regs[20] & 0x08; }
static inline bool v9968_epal(const v9968_t *v) { return v->regs[20] & 0x10; }
/* G6/G7 without sprite mode 3 store VRAM interleaved (two banks). */
bool v9968_interleave(const v9968_t *v);
static inline uint32_t v9968_phys(bool interleave, uint32_t a) {
    if (!interleave) return a & V9968_VRAM_MASK;
    return (a & 0x20000u) | ((a & 1u) << 16) | ((a >> 1) & 0xFFFFu);
}
unsigned v9968_display_height(const v9968_t *v);   /* 192, 212, 384 or 424 */
unsigned v9968_lines_per_field(const v9968_t *v);  /* 192 or 212 */

/* ---- video timing events (called by the video output driver) ---- */
void v9968_video_vblank_start(v9968_t *v);
void v9968_video_vblank_end(v9968_t *v);
/* Called when the VDP is about to render display line `line` (0..): fires
 * the line interrupt of the previous line and the frame interrupt. */
void v9968_video_line(v9968_t *v, unsigned line);

/* ---- rendering ---- */
V9968_HOT void v9968_render_line(v9968_t *v, unsigned line,
                                 uint16_t *out, unsigned out_width);
uint16_t v9968_backdrop_rgb565(const v9968_t *v);

/* ---- command engine (v9968_cmd.c) ---- */
void v9968_cmd_write_reg(v9968_t *v, unsigned reg, uint8_t data);
/* Runs the command engine for up to `budget` clocks of the 85.9MHz engine
 * clock; returns the clocks used (0 when idle or waiting for the CPU). */
V9968_HOT unsigned v9968_cmd_step(v9968_t *v, unsigned budget);
/* Tests: run LRMM through the general per-pixel code only (no fast path). */
extern bool v9968_cmd_reference;
/* Optional: v9968_cmd_step returns early (between pixels, after at least 32
 * clocks) once this word differs from its value at the call, so a long
 * budget does not keep the caller from other work (the firmware watches the
 * bus event ring's DMA write address). */
extern const volatile uint32_t *v9968_cmd_watch;
/* Feeds pending geo3d commands to the idle command engine, then runs it. */
V9968_HOT unsigned v9968_service(v9968_t *v, unsigned budget);
static inline bool v9968_cmd_busy(const v9968_t *v) { return v->cmd.state != 0; }
void v9968_cmd_read_color(v9968_t *v);      /* S#7 read */
void v9968_cmd_clear_bd(v9968_t *v);        /* S#9 read */

#endif
