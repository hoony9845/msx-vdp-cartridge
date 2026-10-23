/* Helpers that mimic what an MSX program does with OUT/IN on ports 0x60-0x6F. */
#ifndef MSX_IO_H
#define MSX_IO_H

#include "v9990.h"

static inline void out(v9990_t *v, uint8_t port, uint8_t data) {
    v9990_write_port(v, port & 0x0F, data);
}

static inline uint8_t in(v9990_t *v, uint8_t port) {
    return v9990_read_port(v, port & 0x0F);
}

static inline void set_reg(v9990_t *v, uint8_t reg, uint8_t data) {
    out(v, 0x64, reg);
    out(v, 0x63, data);
}

static inline void set_vram_write_addr(v9990_t *v, uint32_t addr) {
    out(v, 0x64, 0);
    out(v, 0x63, addr & 0xFF);
    out(v, 0x63, (addr >> 8) & 0xFF);
    out(v, 0x63, (addr >> 16) & 0x07);
}

static inline void set_vram_read_addr(v9990_t *v, uint32_t addr) {
    out(v, 0x64, 3);
    out(v, 0x63, addr & 0xFF);
    out(v, 0x63, (addr >> 8) & 0xFF);
    out(v, 0x63, (addr >> 16) & 0x07);
}

static inline void set_palette(v9990_t *v, uint8_t index,
                               uint8_t r, uint8_t g, uint8_t b) {
    set_reg(v, 14, (uint8_t)(index << 2));
    out(v, 0x61, r);
    out(v, 0x61, g);
    out(v, 0x61, b);
}

/* Run the command engine until it is done or waits for CPU data. */
static inline void run_cmd(v9990_t *v) {
    while (v9990_cmd_step(v, 4096) && !(v->status & V9990_ST_TR)) {}
}

#endif
