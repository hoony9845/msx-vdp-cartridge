#ifndef BUS_H
#define BUS_H

#include "v9990.h"

/* Configure GPIOs, PIO state machines and DMA for the MSX bus. */
void bus_init(v9990_t *v);

/* core1 entry point: processes bus events and runs the command engine. */
void bus_core1_main(void);

#endif
