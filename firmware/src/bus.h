#ifndef BUS_H
#define BUS_H

#include "v9968.h"
#include "v9990.h"
#include "vdp_select.h"

/* Configure GPIOs, PIO state machines and DMA for the MSX bus.
 * `vdp` is a v9990_t or a v9968_t, as `chip` says. */
void bus_init(vdp_chip_t chip, void *vdp);

/* core1 entry point: processes bus events and runs the command engine. */
void bus_core1_main(void);

#endif
