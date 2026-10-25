/* Helpers that mimic what an MSX program does with OUT/IN on the V9968
 * cartridge ports 0x88-0x8F. */
#ifndef MSX_IO9968_H
#define MSX_IO9968_H

#include "v9968.h"

static inline void vout(v9968_t *v, uint8_t port, uint8_t data) {
    v9968_write_port(v, port & 7, data);
}

static inline uint8_t vin(v9968_t *v, uint8_t port) {
    return v9968_read_port(v, port & 7);
}

static inline void vreg(v9968_t *v, uint8_t reg, uint8_t data) {
    vout(v, 0x89, data);
    vout(v, 0x89, (uint8_t)(0x80 | reg));
}

static inline uint8_t vstatus(v9968_t *v, uint8_t s) {
    vreg(v, 15, s);
    return vin(v, 0x89);
}

static inline void vwrite_addr(v9968_t *v, uint32_t a) {
    vreg(v, 14, (uint8_t)((a >> 14) & 0x0F));
    vout(v, 0x89, (uint8_t)a);
    vout(v, 0x89, (uint8_t)(0x40 | ((a >> 8) & 0x3F)));
}

static inline void vread_addr(v9968_t *v, uint32_t a) {
    vreg(v, 14, (uint8_t)((a >> 14) & 0x0F));
    vout(v, 0x89, (uint8_t)a);
    vout(v, 0x89, (uint8_t)((a >> 8) & 0x3F));
}

/* Unlock R#20/R#21 and select the V9968 mode (V58 = 0). */
static inline void v9968_native(v9968_t *v, uint8_t r20) {
    vout(v, 0x8C, 0x00);
    vreg(v, 21, 0x00);
    vreg(v, 20, r20);
}

/* SCREEN 5 (GRAPHIC4), 212 lines, display on, page 0 */
static inline void screen5(v9968_t *v) {
    vreg(v, 0, 0x06);
    vreg(v, 1, 0x40);
    vreg(v, 2, 0x1F);
    vreg(v, 8, 0x08);
    vreg(v, 9, 0x80);
}

/* SCREEN 8 (GRAPHIC7), 212 lines, display on, page 0 */
static inline void screen8(v9968_t *v) {
    vreg(v, 0, 0x0E);
    vreg(v, 1, 0x40);
    vreg(v, 2, 0x1F);
    vreg(v, 8, 0x08);
    vreg(v, 9, 0x80);
}

/* Run commands (and geo3d) until done or waiting for the CPU. */
static inline void run9968(v9968_t *v) {
    for (int i = 0; i < 1000000; i++) {
        if (!v9968_service(v, 1u << 20) && !v->geo.running) break;
    }
}

#endif
