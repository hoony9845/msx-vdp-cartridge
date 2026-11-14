/*
 * geo3d v2.0: the 3D geometry coprocessor of the V9968 cartridge.
 *
 * A port of the bit-exact Python reference model of the RTL
 * (alexmoncks/V9968_Cartridge, branch geo3d-v2.0, geo3d/sim/gen_scenes.py
 * and gen_vectors.py; MIT, Copyright (c) 2026 Alex Moncks). The register
 * window, the read-back path and the status byte follow geo3d_engine.v.
 *
 * A RUN transforms the vertices and classifies edges or faces at once; the
 * LINE / LRMM commands are then produced one at a time, whenever the V9968
 * command engine is idle (geo3d_next_command), as the RTL's issuer does.
 * Like the openMSX device, the geometry itself takes no emulated time.
 *
 * The ported model is MIT, Copyright (c) 2026 Alex Moncks; see
 * THIRD_PARTY_NOTICES.md.
 */
#ifndef GEO3D_H
#define GEO3D_H

#include <stdbool.h>
#include <stdint.h>

#define GEO3D_AW      10
#define GEO3D_NMEM    (1u << GEO3D_AW)
#define GEO3D_ID      0x20          /* index 0x49: v2.0 (v1.0.0 reads 0xFF) */
#define GEO3D_CMD_MAX 19            /* LRMM: R#32-R#45, R#47-R#50, R#46 */

typedef struct {
    int16_t sx, sy, z;
    uint8_t flags;                  /* NEAR OVFX OVFY - outcodes(7:4) */
} geo3d_proj_t;

typedef struct {
    uint16_t idx[4];
    int16_t  n[3];
    uint8_t  base;
} geo3d_face_t;

typedef struct {
    int32_t  key;
    uint16_t face;
    uint8_t  color, level;
} geo3d_vis_t;

typedef struct {
    /* register window */
    int32_t  cfg[18];               /* M00..M22, TX TY TZ, F CX CY ZNEAR W H */
    int16_t  imm[3];                /* immediate vertex VX VY VZ */
    geo3d_proj_t result;            /* 0x30-0x36: last core result */
    uint8_t  widx, rptr, lo;
    uint16_t vaddr, eaddr, nvert, nedge, faddr, nface, taddr;
    uint16_t vfirst, efirst, ffirst;
    uint8_t  color, lop;
    uint16_t ypage;
    bool     ext;
    int16_t  light[3];
    uint16_t texx, texy;
    uint8_t  tstride;
    uint8_t  sbuf[12];              /* stream assembly (vertex/edge/face/tex) */
    uint8_t  sbc;
    uint16_t cnt_skip, cnt_draw, cnt_cull;

    /* model RAMs */
    int16_t  vmem[GEO3D_NMEM][3];
    uint16_t emem[GEO3D_NMEM][2];
    geo3d_face_t fmem[GEO3D_NMEM];
    uint8_t  tmem[GEO3D_NMEM][8];
    geo3d_proj_t pmem[GEO3D_NMEM];  /* projections outlive the RUN */

    /* RUN in progress */
    bool     running;
    bool     faces, textured;
    uint16_t next_item;             /* edge k, or index into vis[] */
    uint16_t nvis;
    int32_t  row, row_end;          /* face spans: current/last row */
    int32_t  fx[4], fy[4];          /* the current face's projected corners */
    geo3d_vis_t vis[GEO3D_NMEM];
    /* wireframe RUN: the edges to draw, clipped once by the RUN (the Z80
     * must not write the model while the RUN is busy) */
    int16_t  seg[GEO3D_NMEM][4];
    uint16_t nseg;
} geo3d_t;

void geo3d_reset(geo3d_t *g);
void geo3d_write(geo3d_t *g, int sel, uint8_t data);    /* sel 0 = index */
uint8_t geo3d_status(const geo3d_t *g);                 /* index port read */
uint8_t geo3d_peek_data(const geo3d_t *g);
void geo3d_read_data(geo3d_t *g);                       /* advance rptr */

/* Next VDP command of the RUN: writes the register values (in issue order,
 * see geo3d_command_reg) to `bytes` and returns 11 (LINE) or 19 (LRMM); 0
 * when the RUN has nothing left to draw. */
unsigned geo3d_next_command(geo3d_t *g, uint8_t bytes[GEO3D_CMD_MAX]);
static inline unsigned geo3d_command_reg(unsigned len, unsigned k) {
    if (len != GEO3D_CMD_MAX) return 36 + k;
    return k < 14 ? 32 + k : k < 18 ? 33 + k : 46;
}

/* The core: matrix, translation, perspective (gen_vectors.py model()). */
geo3d_proj_t geo3d_transform(const int32_t cfg[18], const int16_t v[3]);

#endif
