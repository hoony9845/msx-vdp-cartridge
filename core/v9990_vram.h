/*
 * VRAM address mapping.
 *
 * The V9990 has two 256KB VRAM banks. We store VRAM in "physical" layout
 * (same convention as openMSX) so that switching display modes shows the
 * same data the real chip would:
 *   - P1 : CPU address == physical address
 *   - Bx : even bytes in bank 0, odd bytes in bank 1 (interleaved)
 *   - P2 : PGT interleaved (lowest bit selects the bank), SPAT 0x7BE00 in
 *          VRAM0, PNT 0x7C000 in VRAM1 (manual 10.2.14)
 */
#ifndef V9990_VRAM_H
#define V9990_VRAM_H

#include "v9990.h"

static inline uint32_t v9990_vram_transform_bx(uint32_t addr) {
    return ((addr & 1u) << 18) | ((addr & 0x7FFFEu) >> 1);
}

static inline uint32_t v9990_vram_transform_p2(uint32_t addr) {
    if (addr < 0x78000u) return v9990_vram_transform_bx(addr);
    if (addr < 0x7C000u) return addr - 0x3C000u;
    return addr;
}

/* CPU (linear) address -> physical address for the current mode. */
static inline uint32_t v9990_vram_phys(const v9990_t *v, uint32_t addr) {
    addr &= V9990_VRAM_MASK;
    switch (V9990_DSPM(v->regs[V9990_R_SCREEN_MODE0])) {
    case 0:  return addr;                            /* P1 */
    case 1:  return v9990_vram_transform_p2(addr);   /* P2 */
    default: return v9990_vram_transform_bx(addr);   /* bitmap / standby */
    }
}

/* Byte at physical address `phys` (bit 18 selects the bank). */
static inline uint8_t *v9990_vram_ptr(const v9990_t *v, uint32_t phys) {
    return (phys & 0x40000u) ? v->vram1 + (phys & 0x3FFFFu) : v->vram + (phys & 0x3FFFFu);
}

static inline uint8_t v9990_vram_read(const v9990_t *v, uint32_t addr) {
    return *v9990_vram_ptr(v, v9990_vram_phys(v, addr));
}

static inline void v9990_vram_write(v9990_t *v, uint32_t addr, uint8_t data) {
    *v9990_vram_ptr(v, v9990_vram_phys(v, addr)) = data;
}

/* Bitmap-mode linear access, used by the renderer and command engine:
 * even addresses in VRAM0, odd ones in VRAM1. */
static inline uint8_t v9990_vram_read_bx(const v9990_t *v, uint32_t addr) {
    return ((addr & 1u) ? v->vram1 : v->vram)[(addr & V9990_VRAM_MASK) >> 1];
}

static inline void v9990_vram_write_bx(v9990_t *v, uint32_t addr, uint8_t data) {
    ((addr & 1u) ? v->vram1 : v->vram)[(addr & V9990_VRAM_MASK) >> 1] = data;
}

#endif
