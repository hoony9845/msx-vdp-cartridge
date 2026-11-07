#ifndef DVI_OUT_H
#define DVI_OUT_H

#include <stdbool.h>
#include <stdint.h>

#define DVI_WIDTH        640
#define DVI_HEIGHT       480
#define DVI_LINE_BUFFERS 4          /* power of two */

/* The emulated VDP as seen by the video output (callbacks run in the DVI
 * DMA IRQ on core0). */
typedef struct {
    void *vdp;
    unsigned (*height)(const void *vdp);    /* picture lines, latched per frame */
    void (*vblank_start)(void *vdp);
    void (*vblank_end)(void *vdp);
    /* Render-ahead mode (just_in_time = false): line(n) when line n starts
     * on screen; the renderer runs up to DVI_LINE_BUFFERS - 1 lines ahead.
     * Just-in-time mode: line(n) when line n - 1 starts on screen, and line n
     * is rendered during the last output line of n - 1, so a register
     * written by a line interrupt handler shows on the next line. */
    void (*line)(void *vdp, unsigned n);
    void (*hblank)(void *vdp, bool on);     /* may be NULL */
    bool just_in_time;
} dvi_vdp_t;

/* One output line as sent to HSTX: the horizontal sync command list, then
 * the pixels, in one DMA block (every line is a block of the same length,
 * see dvi_out.c). */
#define DVI_LINE_CMD_WORDS 9
typedef struct {
    uint32_t cmd[DVI_LINE_CMD_WORDS];   /* set by dvi_out_start() */
    uint16_t px[DVI_WIDTH];
} dvi_line_t;

/* Scanline hand-off between the DVI IRQ and the renderer (both on core0). */
typedef struct {
    dvi_line_t line[DVI_LINE_BUFFERS];  /* indexed by src_line % N */
    dvi_line_t border;                  /* backdrop-filled line */
    volatile int32_t  src_line;     /* VDP line being scanned out, -1 outside */
    volatile int32_t  pos;          /* output line - picture top (may be < 0) */
    volatile uint32_t frame;        /* incremented at the end of each picture */
    volatile uint32_t src_height;   /* latched per frame */
    volatile uint32_t scale;        /* output lines per VDP line (1 or 2) */
    /* renderer timing (USB serial "stat") */
    volatile uint32_t render_lines, render_us, render_max_us;
    volatile uint32_t render_late;  /* lines rendered after their scan-out began */
    volatile uint32_t dma_late;     /* IRQs that found several lines sent (late IRQ) */
    volatile uint32_t dma_restarts; /* output restarted: frame period was wrong */
} dvi_lines_t;

extern dvi_lines_t dvi_lines;

/* Set up HSTX + DMA for 640x480@60 and start output. clk_hstx must be
 * 126MHz (see main.c). Must be called on the core that will take the IRQ. */
void dvi_out_start(const dvi_vdp_t *vdp);

/* USB serial "stat": clocks, HSTX and DMA state of the output. */
void dvi_print_debug(void);

#endif
