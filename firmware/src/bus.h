#ifndef BUS_H
#define BUS_H

#include "v9968.h"
#include "v9990.h"
#include "vdp_select.h"

/* Configure GPIOs, PIO state machines and DMA for the MSX bus.
 * `vdp` is a v9990_t or a v9968_t, as `chip` says. */
void bus_init(vdp_chip_t chip, void *vdp);

/* Re-publish P#5 after the video timing changed a status bit (core0 IRQ). */
void bus_refresh_status(void);

/* core1 entry point: processes bus events and runs the command engine. */
void bus_core1_main(void);

/* USB serial "stat": core1 health and V9990/V9968 bus state ("stat reset"
 * clears the counters and maxima). Runs on core0. */
void bus_print_stats(bool reset);

/* USB serial "vram": CRC32 of each 64KB VRAM block (V9990). */
void bus_print_vram_crc(void);

/* USB serial "out <port> <value>" (hex): V9990 port write for tests. */
void bus_debug_out(unsigned port, unsigned value);

#endif
