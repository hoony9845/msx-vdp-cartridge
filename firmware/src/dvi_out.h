#ifndef DVI_OUT_H
#define DVI_OUT_H

#include <stdint.h>
#include "v9990.h"

#define DVI_WIDTH        640
#define DVI_HEIGHT       480
#define DVI_LINE_BUFFERS 4          /* power of two */

/* Scanline hand-off between the DVI IRQ and the renderer (both on core0). */
typedef struct {
    uint16_t line[DVI_LINE_BUFFERS][DVI_WIDTH];  /* indexed by src_line % N */
    uint16_t border[DVI_WIDTH];                  /* backdrop-filled line */
    volatile int32_t  src_line;     /* V9990 line being scanned out, -1 outside */
    volatile uint32_t frame;        /* incremented at the end of each picture */
    volatile uint32_t src_height;   /* latched per frame */
} dvi_lines_t;

extern dvi_lines_t dvi_lines;

/* Set up HSTX + DMA for 640x480@60 and start output. clk_hstx must be
 * 126MHz (see main.c). Must be called on the core that will take the IRQ. */
void dvi_out_start(v9990_t *v);

#endif
