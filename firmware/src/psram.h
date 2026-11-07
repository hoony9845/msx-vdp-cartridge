#ifndef PSRAM_H
#define PSRAM_H

#include <stddef.h>
#include <stdint.h>

/* Cached, memory mapped PSRAM window (QMI chip select 1). */
#define PSRAM_BASE ((uint8_t *)0x11000000u)

/* Detects and configures the PSRAM. Returns its size in bytes, 0 if absent. */
size_t psram_init(void);

/* ID bytes read by psram_init() (KGD 0x5D = PSRAM present). */
extern uint8_t psram_id_kgd, psram_id_eid;

/* USB serial "psram [sweep]": uncached write/read test of the first 512KB
 * (destroys the VRAM); "sweep" also tries every RX delay. */
void psram_diag(bool sweep);

/* Streaming bulk read (DMA from the XIP stream FIFO); call psram_stream_init()
 * once. Word aligned src/dst/len use the stream, anything else is copied. */
void psram_stream_init(void);
void psram_stream_read(void *dst, const void *src, uint32_t len);

#endif
