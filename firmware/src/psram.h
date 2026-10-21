#ifndef PSRAM_H
#define PSRAM_H

#include <stddef.h>
#include <stdint.h>

/* Cached, memory mapped PSRAM window (QMI chip select 1). */
#define PSRAM_BASE ((uint8_t *)0x11000000u)

/* Detects and configures the PSRAM. Returns its size in bytes, 0 if absent. */
size_t psram_init(void);

#endif
