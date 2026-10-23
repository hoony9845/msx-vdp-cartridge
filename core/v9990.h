/*
 * Portable V9990 core: no Pico SDK dependencies.
 * Builds both on the host (tests) and on the RP2350 (firmware).
 *
 * Threading model on the RP2350:
 *   core1 : v9990_write_port / v9990_read_port (bus events), v9990_cmd_step
 *   core0 : v9990_render_line, v9990_video_* (timing events)
 * Only core1 mutates registers; core0 just reads them and sets the status/
 * interrupt flags through the v9990_video_* functions.
 */
#ifndef V9990_H
#define V9990_H

#include <stdbool.h>
#include <stdint.h>
#include "v9990_regs.h"

#ifndef V9990_HOT
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define V9990_HOT __attribute__((section(".time_critical.v9990")))
#else
#define V9990_HOT
#endif
#endif

/* status / int_flags are touched by both cores: use atomic bit operations. */
#define V9990_SET_BITS(field, mask) \
    ((void)__atomic_fetch_or(&(field), (uint8_t)(mask), __ATOMIC_RELAXED))
#define V9990_CLR_BITS(field, mask) \
    ((void)__atomic_fetch_and(&(field), (uint8_t)~(mask), __ATOMIC_RELAXED))
#define V9990_XOR_BITS(field, mask) \
    ((void)__atomic_fetch_xor(&(field), (uint8_t)(mask), __ATOMIC_RELAXED))

typedef enum {
    V9990_MODE_P1, V9990_MODE_P2,
    V9990_MODE_B1, V9990_MODE_B2, V9990_MODE_B3, V9990_MODE_B4,
    V9990_MODE_B5, V9990_MODE_B6,
    V9990_MODE_B7,      /* undocumented: DCKM=2 without HSCN, 1024 wide */
    V9990_MODE_STANDBY
} v9990_display_mode_t;

typedef enum {
    V9990_COLOR_BP2, V9990_COLOR_BP4, V9990_COLOR_BP6, V9990_COLOR_BD8,
    V9990_COLOR_BYJK, V9990_COLOR_BYJKP, V9990_COLOR_BYUV, V9990_COLOR_BYUVP,
    V9990_COLOR_BD16
} v9990_color_mode_t;

typedef struct {
    uint8_t r, g, b;   /* 5 bits each; bit7 of r = YS (transparent) */
} v9990_palette_entry_t;

/* Command engine state (see v9990_cmd.c) */
typedef struct {
    uint8_t  op;            /* active command (0 = idle) */
    uint16_t sx, sy, dx, dy, nx, ny;
    uint16_t x, y;          /* current destination position */
    uint16_t src_x, src_y;  /* current source position (LMMM) */
    uint16_t remaining_x, remaining_y;
    uint16_t data_word;     /* LMMC/LMCM 16bpp byte assembly */
    uint8_t  data_phase;
} v9990_cmd_state_t;

typedef struct v9990 {
    uint8_t  *vram;         /* 512KB, physical layout (see v9990_vram.h) */

    uint8_t  regs[V9990_NUM_REGS];
    uint8_t  reg_select;    /* P#4 */
    uint8_t  sys_ctrl;      /* P#7 */

    volatile uint8_t status;    /* P#5; VR/HR/EO written by video timing */
    volatile uint8_t int_flags; /* P#6 */

    uint32_t vram_write_ptr;
    uint32_t vram_read_ptr;
    uint8_t  vram_read_latch;   /* prefetched byte returned by next P#0 read */

    v9990_palette_entry_t palette[V9990_PALETTE_ENTRIES];
    uint16_t palette_rgb565[V9990_PALETTE_ENTRIES]; /* cache for the renderer */

    uint8_t  cmd_read_latch;    /* next byte for P#2 read (LMCM/POINT) */
    v9990_cmd_state_t cmd;

    /* Video timing state (written by core0's video driver) */
    volatile uint16_t cur_line;       /* display line being started */
    /* Writing the low byte of a vertical scroll register restarts that
     * layer's line counter at the current line (verified on hardware
     * according to openMSX); the high bytes are latched per frame. */
    volatile uint16_t line_offset_a, line_offset_b;
    uint8_t  scroll_ay_high, scroll_by_high;
} v9990_t;

/* ---- lifecycle ---- */
void v9990_init(v9990_t *v, uint8_t *vram);
void v9990_reset(v9990_t *v);

/* ---- bus interface (port = low nibble 0..15) ---- */
V9990_HOT void    v9990_write_port(v9990_t *v, uint8_t port, uint8_t data);
/* Read with side effects (auto increment, prefetch...). */
V9990_HOT uint8_t v9990_read_port(v9990_t *v, uint8_t port);
/* Value the next read of `port` will return, without side effects.
 * The firmware pre-loads these into the PIO read-response table. */
V9990_HOT uint8_t v9990_peek_port(const v9990_t *v, uint8_t port);

static inline bool v9990_irq_pending(const v9990_t *v) {
    return (v->int_flags & v->regs[V9990_R_INT_ENABLE] & 0x07) != 0;
}

/* ---- display mode helpers ---- */
v9990_display_mode_t v9990_display_mode(const v9990_t *v);
v9990_color_mode_t   v9990_color_mode(const v9990_t *v);
unsigned v9990_bpp(const v9990_t *v);
unsigned v9990_image_width(const v9990_t *v);   /* VRAM image width in pixels */
unsigned v9990_display_width(const v9990_t *v);
unsigned v9990_display_height(const v9990_t *v);

/* ---- video timing events (called by the video output driver) ---- */
void v9990_video_vblank_start(v9990_t *v);
void v9990_video_vblank_end(v9990_t *v);
void v9990_video_line_start(v9990_t *v, unsigned line);  /* display line 0.. */

/* ---- rendering ---- */
/* Render display line `line` into `out` as RGB565, `out_width` pixels wide
 * (the image is scaled/centered to fit, border = backdrop color). */
V9990_HOT void v9990_render_line(const v9990_t *v, unsigned line,
                                 uint16_t *out, unsigned out_width);
uint16_t v9990_backdrop_rgb565(const v9990_t *v);

/* ---- command engine (v9990_cmd.c) ---- */
void v9990_cmd_start(v9990_t *v);
/* Advance the running command by up to `budget` pixel operations.
 * Returns true while the command is still executing. */
V9990_HOT bool v9990_cmd_step(v9990_t *v, unsigned budget);
void v9990_cmd_write_data(v9990_t *v, uint8_t data);
uint8_t v9990_cmd_read_data(v9990_t *v);

#endif
