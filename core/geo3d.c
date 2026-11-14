/*
 * geo3d v2.0 (see geo3d.h). Every function mirrors the Python reference
 * model; Python's floor division and arithmetic shifts are kept exactly.
 */
#include <stdlib.h>
#include <string.h>
#include "geo3d.h"

#define MEM_MASK (GEO3D_NMEM - 1u)

static inline int64_t sat64(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* Python's a // b (b != 0); in 32 bits when both fit (the M33 divides
 * those in hardware, 64-bit division is a library call) */
static inline int64_t floor_div(int64_t a, int64_t b) {
    if (a == (int32_t)a && b == (int32_t)b && a != INT32_MIN) {
        int32_t a32 = (int32_t)a, b32 = (int32_t)b;
        int32_t q32 = a32 / b32;
        if ((a32 % b32 != 0) && ((a32 < 0) != (b32 < 0))) q32--;
        return q32;
    }
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* Python's a >> n on negative numbers floors as well */
static inline int64_t asr(int64_t a, unsigned n) {
    return a >= 0 ? a >> n : -((-a - 1) >> n) - 1;
}

static inline int16_t s16(unsigned v) { return (int16_t)(uint16_t)v; }

/* ------------------------------------------------------------------ */
/* Core                                                                */
/* ------------------------------------------------------------------ */

geo3d_proj_t geo3d_transform(const int32_t cfg[18], const int16_t v[3]) {
    const int32_t *m = cfg, *t = cfg + 9;
    int64_t f = cfg[12], cx = cfg[13], cy = cfg[14], znear = cfg[15], w = cfg[16], h = cfg[17];
    int64_t c[3];
    for (int r = 0; r < 3; r++) {
        int64_t acc = (int64_t)m[3 * r] * v[0] + (int64_t)m[3 * r + 1] * v[1] +
                      (int64_t)m[3 * r + 2] * v[2];
        c[r] = sat64(asr(acc, 14) + t[r], -131071, 131071);
    }
    geo3d_proj_t p = { 0, 0, (int16_t)sat64(c[2], -32768, 32767), 0 };
    int64_t zc = c[2];
    if (zc < znear || zc <= 0) {
        p.flags = 0x01;
        return p;
    }
    int64_t q[2];
    uint8_t flags = 0;
    for (int axis = 0; axis < 2; axis++) {
        int64_t a = c[axis] < 0 ? -c[axis] : c[axis];
        int64_t num = a * f, qq;
        if (num >= (zc << 15)) {
            qq = 32767;
            flags |= (uint8_t)(1u << (1 + axis));
        } else {
            qq = num / zc;
        }
        q[axis] = c[axis] < 0 ? -qq : qq;
    }
    int64_t sx = sat64(cx + q[0], -32768, 32767);
    int64_t sy = sat64(cy - q[1], -32768, 32767);
    unsigned outc = (sx < 0) | ((sx >= w) << 1) | ((sy < 0) << 2) | ((sy >= h) << 3);
    p.sx = (int16_t)sx;
    p.sy = (int16_t)sy;
    p.flags = (uint8_t)(flags | (outc << 4));
    return p;
}

/* ------------------------------------------------------------------ */
/* Edges: clipping by bisection and the LINE command                   */
/* ------------------------------------------------------------------ */

static unsigned ocode(int32_t x, int32_t y, int32_t w, int32_t h) {
    return (x < 0) | ((x >= w) << 1) | ((y < 0) << 2) | ((y >= h) << 3);
}

static inline int32_t mid(int32_t a, int32_t b) { return (int32_t)asr((int64_t)a + b, 1); }

enum { CLIP_SKIP, CLIP_CULL, CLIP_DRAW };

static void bisect(int32_t ix, int32_t iy, int32_t ox, int32_t oy, int32_t w, int32_t h,
                   int32_t *rx, int32_t *ry) {
    for (int i = 0; i < 16; i++) {
        int32_t mx = mid(ix, ox), my = mid(iy, oy);
        if (ocode(mx, my, w, h) == 0) { ix = mx; iy = my; }
        else                          { ox = mx; oy = my; }
    }
    *rx = ix;
    *ry = iy;
}

static int clip_edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t w, int32_t h,
                     int32_t seg[4]) {
    unsigned ca = ocode(ax, ay, w, h), cb = ocode(bx, by, w, h);
    if (ca & cb) return CLIP_CULL;
    int32_t px, py;
    if (ca == 0)      { px = ax; py = ay; }
    else if (cb == 0) { px = bx; py = by; }
    else {
        int32_t lx = ax, ly = ay, hx = bx, hy = by;
        bool found = false;
        for (int i = 0; i < 16; i++) {
            int32_t mx = mid(lx, hx), my = mid(ly, hy);
            unsigned cm = ocode(mx, my, w, h);
            if (cm == 0) { px = mx; py = my; found = true; break; }
            if (ocode(lx, ly, w, h) & cm)      { lx = mx; ly = my; }
            else if (cm & ocode(hx, hy, w, h)) { hx = mx; hy = my; }
            else return CLIP_SKIP;
        }
        if (!found) return CLIP_SKIP;
    }
    if (ca == 0) { seg[0] = ax; seg[1] = ay; }
    else         bisect(px, py, ax, ay, w, h, &seg[0], &seg[1]);
    if (cb == 0) { seg[2] = bx; seg[3] = by; }
    else         bisect(px, py, bx, by, w, h, &seg[2], &seg[3]);
    return CLIP_DRAW;
}

static int classify_edge(const geo3d_t *g, unsigned k, int32_t seg[4]) {
    const uint16_t *e = g->emem[(g->efirst + k) & MEM_MASK];
    const geo3d_proj_t *pa = &g->pmem[e[0] & MEM_MASK], *pb = &g->pmem[e[1] & MEM_MASK];
    if ((pa->flags & 7) || (pb->flags & 7)) return CLIP_SKIP;
    return clip_edge(pa->sx, pa->sy, pb->sx, pb->sy, g->cfg[16], g->cfg[17], seg);
}

static unsigned line_bytes(const geo3d_t *g, const int32_t s[4], uint8_t *b) {
    int32_t x1 = s[0], y1 = s[1];
    int32_t dx = s[2] - x1, dy = s[3] - y1;
    int32_t ax = abs(dx), ay = abs(dy);
    bool maj = ay > ax;
    int32_t nx = maj ? ay : ax, ny = maj ? ax : ay;
    unsigned dyf = (unsigned)(((y1 & 0x7FF) + g->ypage) & 0x7FF);
    b[0] = (uint8_t)x1;            b[1] = (uint8_t)((x1 >> 8) & 1);
    b[2] = (uint8_t)dyf;           b[3] = (uint8_t)(dyf >> 8);
    b[4] = (uint8_t)nx;            b[5] = (uint8_t)((nx >> 8) & 7);
    b[6] = (uint8_t)ny;            b[7] = (uint8_t)((ny >> 8) & 7);
    b[8] = g->color;
    b[9] = (uint8_t)(((dy < 0) << 3) | ((dx < 0) << 2) | maj);
    b[10] = (uint8_t)(0x70 | (g->lop & 0xF));
    return 11;
}

/* ------------------------------------------------------------------ */
/* Faces                                                               */
/* ------------------------------------------------------------------ */

static int cmp_vis(const void *a, const void *b) {
    const geo3d_vis_t *x = a, *y = b;
    if (x->key != y->key) return x->key > y->key ? -1 : 1;    /* farthest first */
    return (int)x->face - (int)y->face;                      /* then lower face */
}

static void classify_faces(geo3d_t *g) {
    const int32_t *m = g->cfg;
    int64_t lm[3];
    for (int j = 0; j < 3; j++) {
        int64_t s = 0;
        for (int i = 0; i < 3; i++) s += (int64_t)m[3 * i + j] * g->light[i];
        lm[j] = sat64(asr(s, 14), -131071, 131071);
    }
    g->nvis = 0;
    for (unsigned k = 0; k < g->nface; k++) {
        unsigned fi = (g->ffirst + k) & MEM_MASK;
        const geo3d_face_t *f = &g->fmem[fi];
        const geo3d_proj_t *p[4];
        bool skip = false;
        for (int i = 0; i < 4; i++) {
            p[i] = &g->pmem[f->idx[i] & MEM_MASK];
            if (p[i]->flags & 7) skip = true;
        }
        if (skip) { g->cnt_skip++; continue; }
        int64_t area = (int64_t)(p[1]->sx - p[0]->sx) * (p[2]->sy - p[0]->sy) -
                       (int64_t)(p[1]->sy - p[0]->sy) * (p[2]->sx - p[0]->sx);
        if (area <= 0) { g->cnt_cull++; continue; }
        int64_t shade = asr(lm[0] * f->n[0] + lm[1] * f->n[1] + lm[2] * f->n[2], 14);
        int64_t lvl = shade <= 0 ? 0 : asr(shade * 7, 14);
        if (lvl > 6) lvl = 6;
        geo3d_vis_t *vis = &g->vis[g->nvis++];
        vis->key = p[0]->z + p[1]->z + p[2]->z + p[3]->z;
        vis->face = (uint16_t)fi;
        vis->color = (uint8_t)(f->base + lvl);
        vis->level = (uint8_t)lvl;
    }
    qsort(g->vis, g->nvis, sizeof(g->vis[0]), cmp_vis);
    g->cnt_draw = g->nvis;
}

static void face_rows(geo3d_t *g, const geo3d_vis_t *vis) {
    const geo3d_face_t *f = &g->fmem[vis->face];
    int32_t ymin = 32767, ymax = -32768;
    g->ftex = g->textured && (f->base & 0x80);
    const uint8_t *uv = g->tmem[vis->face];
    for (int k = 0; k < 4; k++) {
        g->fx[k] = g->pmem[f->idx[k] & MEM_MASK].sx;
        g->fy[k] = g->pmem[f->idx[k] & MEM_MASK].sy;
        g->fu[k] = g->ftex ? uv[2 * k] : 0;
        g->fv[k] = g->ftex ? uv[2 * k + 1] : 0;
    }
    for (int k = 0; k < 4; k++) {
        int32_t y = g->fy[k];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    g->row = ymin < 0 ? 0 : ymin;
    g->row_end = ymax < g->cfg[17] - 1 ? ymax : g->cfg[17] - 1;
}

/* A row of an untextured face: only the leftmost and rightmost edge
 * crossings, from the corners face_rows() took. */
static unsigned flat_span(const geo3d_t *g, const geo3d_vis_t *vis, int32_t y, uint8_t *b) {
    const int32_t *xs = g->fx, *ys = g->fy;
    int32_t xl = 0, xr = 0;
    bool set = false;
    for (int k = 0; k < 4; k++) {
        int a = k, c = (k + 1) & 3;
        int32_t ya = ys[a], yc = ys[c], x0, x1;
        if (ya == yc) {
            if (y != ya) continue;
            x0 = xs[a];
            x1 = xs[c];
            if (x0 > x1) { int32_t t = x0; x0 = x1; x1 = t; }
        } else {
            int32_t lo = ya < yc ? ya : yc, hi = ya < yc ? yc : ya;
            if (y < lo || y > hi) continue;
            int32_t dy = y - ya, den = yc - ya;
            x0 = x1 = xs[a] + (int32_t)floor_div((int64_t)dy * (xs[c] - xs[a]), den);
        }
        if (!set || x0 < xl) xl = x0;
        if (!set || x1 > xr) xr = x1;
        set = true;
    }
    if (!set) return 0;
    int32_t w = g->cfg[16];
    int32_t cl = xl > 0 ? xl : 0, cr = xr < w - 1 ? xr : w - 1;
    if (cl > cr) return 0;
    unsigned dy = (unsigned)((y + g->ypage) & 0x7FF);
    int32_t nx = cr - cl;
    b[0] = (uint8_t)cl;  b[1] = (uint8_t)((cl >> 8) & 1);
    b[2] = (uint8_t)dy;  b[3] = (uint8_t)(dy >> 8);
    b[4] = (uint8_t)nx;  b[5] = (uint8_t)((nx >> 8) & 7);
    b[6] = 0;            b[7] = 0;
    b[8] = vis->color;   b[9] = 0;
    b[10] = (uint8_t)(0x70 | (g->lop & 0xF));
    return 11;
}

/* A row of a textured face: the edge crossings are candidates in edge
 * order (the first of equal x wins on each side), with their u, v; 32
 * bits where the values fit (x, u, v), 64 for the products. */
static unsigned tex_span(const geo3d_t *g, const geo3d_vis_t *vis, int32_t y, uint8_t *b) {
    const int32_t *xs = g->fx, *ys = g->fy, *us = g->fu, *vs = g->fv;
    int32_t lx = 0, lu = 0, lv = 0, rx = 0, ru = 0, rv = 0;
    bool set = false;
#define CAND(X, U, V) do { int32_t x_ = (X), u_ = (U), v_ = (V); \
        if (!set || x_ < lx) { lx = x_; lu = u_; lv = v_; } \
        if (!set || x_ > rx) { rx = x_; ru = u_; rv = v_; } \
        set = true; } while (0)
    for (int k = 0; k < 4; k++) {
        int a = k, c = (k + 1) & 3;
        int32_t ya = ys[a], yc = ys[c];
        if (ya == yc) {
            if (y != ya) continue;
            CAND(xs[a], us[a] * 256, vs[a] * 256);
            CAND(xs[c], us[c] * 256, vs[c] * 256);
        } else {
            int32_t lo = ya < yc ? ya : yc, hi = ya < yc ? yc : ya;
            if (y < lo || y > hi) continue;
            int32_t dy = y - ya, den = yc - ya;
            int32_t x = xs[a] + (int32_t)floor_div((int64_t)dy * (xs[c] - xs[a]), den);
            int32_t u = us[a] * 256 + (int32_t)floor_div((int64_t)dy * ((us[c] - us[a]) * 256), den);
            int32_t v = vs[a] * 256 + (int32_t)floor_div((int64_t)dy * ((vs[c] - vs[a]) * 256), den);
            CAND(x, u, v);
        }
    }
#undef CAND
    if (!set) return 0;
    int32_t w = g->cfg[16];
    int32_t cl = lx > 0 ? lx : 0, cr = rx < w - 1 ? rx : w - 1;
    if (cl > cr) return 0;
    unsigned dy = (unsigned)((y + g->ypage) & 0x7FF);
    int64_t du = 0, dv = 0;
    if (rx != lx) {
        int64_t d = sat64(rx - lx, -131071, 131071);
        du = sat64(floor_div(sat64((int64_t)ru - lu, -131071, 131071), d), -32767, 32767);
        dv = sat64(floor_div(sat64((int64_t)rv - lv, -131071, 131071), d), -32767, 32767);
    }
    int64_t off = sat64(cl - lx, -131071, 131071);
    int64_t su = lu + off * du, sv = lv + off * dv;
    unsigned sx = (unsigned)((g->texx + asr(su, 8)) & 0xFFF);
    unsigned sy = (unsigned)((g->texy + vis->level * g->tstride + asr(sv, 8)) & 0x1FFF);
    int32_t nxp = cr - cl + 1;
    b[0] = (uint8_t)sx;        b[1] = (uint8_t)(sx >> 8);
    b[2] = (uint8_t)sy;        b[3] = (uint8_t)(sy >> 8);
    b[4] = (uint8_t)cl;        b[5] = (uint8_t)((cl >> 8) & 1);
    b[6] = (uint8_t)dy;        b[7] = (uint8_t)(dy >> 8);
    b[8] = (uint8_t)nxp;       b[9] = (uint8_t)((nxp >> 8) & 7);
    b[10] = 1;                 b[11] = 0;
    b[12] = vis->color;        b[13] = 0;
    b[14] = (uint8_t)du;       b[15] = (uint8_t)(du >> 8);
    b[16] = (uint8_t)dv;       b[17] = (uint8_t)(dv >> 8);
    b[18] = (uint8_t)(0x30 | (g->lop & 0xF));
    return 19;
}

/* The span of one row of the current face (face_rows() took its corners);
 * returns the command length or 0. */
static unsigned face_span(const geo3d_t *g, const geo3d_vis_t *vis, int32_t y, uint8_t *b) {
    return g->ftex ? tex_span(g, vis, y, b) : flat_span(g, vis, y, b);
}

/* ------------------------------------------------------------------ */
/* RUN                                                                 */
/* ------------------------------------------------------------------ */

static void run(geo3d_t *g, uint8_t ctrl) {
    if (g->running) return;                 /* the RTL ignores RUN while busy */
    for (unsigned k = 0; k < g->nvert; k++) {
        unsigned n = (g->vfirst + k) & MEM_MASK;
        g->pmem[n] = geo3d_transform(g->cfg, g->vmem[n]);
        g->result = g->pmem[n];
    }
    g->cnt_skip = g->cnt_draw = g->cnt_cull = 0;
    g->faces = (ctrl & 2) != 0;
    g->textured = (ctrl & 4) != 0;
    g->next_item = 0;
    if (g->faces) {
        classify_faces(g);
        if (g->nvis) face_rows(g, &g->vis[0]);
    } else {
        int32_t seg[4];
        g->nseg = 0;
        for (unsigned k = 0; k < g->nedge; k++) {
            switch (classify_edge(g, k, seg)) {
            case CLIP_SKIP: g->cnt_skip++; break;
            case CLIP_CULL: g->cnt_cull++; break;
            default:
                g->cnt_draw++;
                for (int i = 0; i < 4; i++) g->seg[g->nseg][i] = (int16_t)seg[i];
                g->nseg++;
                break;
            }
        }
    }
    g->running = true;
}

unsigned geo3d_next_command(geo3d_t *g, uint8_t bytes[GEO3D_CMD_MAX]) {
    if (!g->running) return 0;
    if (!g->faces) {
        if (g->next_item >= g->nseg) return 0;
        const int16_t *s = g->seg[g->next_item++];
        int32_t seg[4] = { s[0], s[1], s[2], s[3] };
        return line_bytes(g, seg, bytes);
    }
    while (g->next_item < g->nvis) {
        const geo3d_vis_t *vis = &g->vis[g->next_item];
        while (g->row <= g->row_end) {
            unsigned n = face_span(g, vis, g->row++, bytes);
            if (n) return n;
        }
        if (++g->next_item < g->nvis) face_rows(g, &g->vis[g->next_item]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Register window                                                     */
/* ------------------------------------------------------------------ */

void geo3d_reset(geo3d_t *g) {
    memset(g, 0, sizeof(*g));
    g->rptr = 0x30;
    g->color = 15;
    g->texy = 512;
    g->light[2] = -16384;
    g->cfg[12] = 256;               /* F, CX, CY, ZNEAR, W, H */
    g->cfg[13] = 128;
    g->cfg[14] = 106;
    g->cfg[15] = 16;
    g->cfg[16] = 256;
    g->cfg[17] = 212;
}

static inline void lo8(uint16_t *r, uint8_t b) { *r = (uint16_t)((*r & 0x300) | b); }
static inline void hi2(uint16_t *r, uint8_t b) { *r = (uint16_t)((*r & 0xFF) | ((b & 3) << 8)); }

/* A write pointer after an entry: 10 bits in extended mode, else only the
 * low byte counts (wraps at 256) as in v1.0.0. */
static void bump(const geo3d_t *g, uint16_t *p) {
    *p = g->ext ? (uint16_t)((*p + 1) & 0x3FF) : (uint16_t)((*p & 0x300) | ((*p + 1) & 0xFF));
}

static void stream_write(geo3d_t *g, unsigned idx, uint8_t b) {
    g->sbuf[g->sbc++] = b;
    const uint8_t *s = g->sbuf;
    switch (idx) {
    case 0x50:
        if (g->sbc < 6) return;
        for (int k = 0; k < 3; k++)
            g->vmem[g->vaddr & MEM_MASK][k] = s16(s[2 * k] | (s[2 * k + 1] << 8));
        bump(g, &g->vaddr);
        break;
    case 0x51:
        if (g->sbc < (g->ext ? 3 : 2)) return;
        if (g->ext) {
            g->emem[g->eaddr & MEM_MASK][0] = (uint16_t)(s[0] | ((s[2] & 3) << 8));
            g->emem[g->eaddr & MEM_MASK][1] = (uint16_t)(s[1] | (((s[2] >> 2) & 3) << 8));
        } else {
            g->emem[g->eaddr & MEM_MASK][0] = s[0];
            g->emem[g->eaddr & MEM_MASK][1] = s[1];
        }
        bump(g, &g->eaddr);
        break;
    case 0x52: {
        if (g->sbc < (g->ext ? 12 : 11)) return;
        geo3d_face_t *f = &g->fmem[g->faddr & MEM_MASK];
        const uint8_t *r = s + (g->ext ? 5 : 4);
        for (int k = 0; k < 4; k++)
            f->idx[k] = (uint16_t)(s[k] | (g->ext ? ((s[4] >> (2 * k)) & 3) << 8 : 0));
        for (int k = 0; k < 3; k++)
            f->n[k] = s16(r[2 * k] | (r[2 * k + 1] << 8));
        f->base = r[6];
        bump(g, &g->faddr);
        break;
    }
    case 0x53:
        if (g->sbc < 8) return;
        memcpy(g->tmem[g->taddr & MEM_MASK], s, 8);
        bump(g, &g->taddr);
        break;
    default:
        break;
    }
    g->sbc = 0;
}

static uint8_t next_idx(uint8_t i) {
    if ((i >> 4) == 4)    return (uint8_t)(0x40 | ((i + 1) & 0xF));
    if ((i >> 2) == 0x15) return (uint8_t)(0x54 | ((i + 1) & 0x3));
    if ((i >> 3) == 0x0B) return (uint8_t)(0x58 | ((i + 1) & 0x7));
    if ((i >> 3) == 0x0C) return (uint8_t)(0x60 | ((i + 1) & 0x7));
    if ((i >> 3) == 0x0D) return (uint8_t)(0x68 | ((i + 1) & 0x7));
    if (i == 0x29)        return 0x24;
    if (i < 0x40)         return (uint8_t)(i + 1);
    return i;
}

void geo3d_write(geo3d_t *g, int sel, uint8_t b) {
    if (sel == 0) {
        g->widx = g->rptr = b;
        g->sbc = 0;
        return;
    }
    uint8_t i = g->widx;
    if (i < 0x2A) {
        if (!(i & 1)) {
            g->lo = b;
        } else {
            unsigned word = (unsigned)(b << 8) | g->lo;
            unsigned wi = i >> 1;
            if (wi < 18) {
                g->cfg[wi] = (wi == 12) ? (int32_t)word : s16(word);
            } else {
                g->imm[wi - 18] = s16(word);
                if (wi == 20) {                 /* VZ high byte: compute */
                    g->result = geo3d_transform(g->cfg, g->imm);
                    g->rptr = 0x30;
                }
            }
        }
    } else {
        switch (i) {
        case 0x40: lo8(&g->vaddr, b); break;
        case 0x41: lo8(&g->eaddr, b); break;
        case 0x42: lo8(&g->nvert, b); break;
        case 0x43: lo8(&g->nedge, b); break;
        case 0x44: g->color = b; break;
        case 0x45: g->lop = b & 0xF; break;
        case 0x46: g->ypage = (uint16_t)((g->ypage & 0x700) | b); break;
        case 0x47: g->ypage = (uint16_t)((g->ypage & 0xFF) | ((b & 7) << 8)); break;
        case 0x48: if (b & 1) run(g, b); break;
        case 0x50: case 0x51: case 0x52: case 0x53: stream_write(g, i, b); break;
        case 0x54: lo8(&g->vfirst, b); break;
        case 0x55: hi2(&g->vfirst, b); break;
        case 0x56: lo8(&g->ffirst, b); break;
        case 0x57: hi2(&g->ffirst, b); break;
        case 0x58: lo8(&g->faddr, b); break;
        case 0x59: lo8(&g->nface, b); break;
        case 0x5A: case 0x5C: case 0x5E: g->lo = b; break;
        case 0x5B: g->light[0] = s16((unsigned)(b << 8) | g->lo); break;
        case 0x5D: g->light[1] = s16((unsigned)(b << 8) | g->lo); break;
        case 0x5F: g->light[2] = s16((unsigned)(b << 8) | g->lo); break;
        case 0x60: g->texx = (uint16_t)((g->texx & 0x100) | b); break;
        case 0x61: g->texx = (uint16_t)((g->texx & 0xFF) | ((b & 1) << 8)); break;
        case 0x62: g->texy = (uint16_t)((g->texy & 0x1F00) | b); break;
        case 0x63: g->texy = (uint16_t)((g->texy & 0xFF) | ((b & 0x1F) << 8)); break;
        case 0x64: g->tstride = b; break;
        case 0x65: lo8(&g->taddr, b); break;
        case 0x66: lo8(&g->efirst, b); break;
        case 0x67: hi2(&g->efirst, b); break;
        case 0x68: hi2(&g->vaddr, b); break;
        case 0x69: hi2(&g->eaddr, b); break;
        case 0x6A: hi2(&g->nvert, b); break;
        case 0x6B: hi2(&g->nedge, b); break;
        case 0x6C: hi2(&g->faddr, b); break;
        case 0x6D: hi2(&g->nface, b); break;
        case 0x6E: hi2(&g->taddr, b); break;
        case 0x6F: g->ext = b & 1; break;
        default: break;
        }
    }
    g->widx = next_idx(i);
}

uint8_t geo3d_status(const geo3d_t *g) {
    return (uint8_t)((g->result.flags & 0xF0) | (g->running ? 0x05 : 0));
}

uint8_t geo3d_peek_data(const geo3d_t *g) {
    const geo3d_proj_t *r = &g->result;
    switch (g->rptr) {
    case 0x30: return (uint8_t)r->sx;
    case 0x31: return (uint8_t)((uint16_t)r->sx >> 8);
    case 0x32: return (uint8_t)r->sy;
    case 0x33: return (uint8_t)((uint16_t)r->sy >> 8);
    case 0x34: return (uint8_t)r->z;
    case 0x35: return (uint8_t)((uint16_t)r->z >> 8);
    case 0x36: return r->flags;
    case 0x40: return (uint8_t)g->vaddr;
    case 0x41: return (uint8_t)g->eaddr;
    case 0x42: return (uint8_t)g->nvert;
    case 0x43: return (uint8_t)g->nedge;
    case 0x44: return g->color;
    case 0x45: return g->lop;
    case 0x46: return (uint8_t)g->ypage;
    case 0x47: return (uint8_t)(g->ypage >> 8);
    case 0x48: return geo3d_status(g);
    case 0x49: return GEO3D_ID;
    case 0x4A: return (uint8_t)g->cnt_skip;
    case 0x4B: return (uint8_t)(g->cnt_skip >> 8);
    case 0x4C: return (uint8_t)g->cnt_draw;
    case 0x4D: return (uint8_t)(g->cnt_draw >> 8);
    case 0x4E: return (uint8_t)g->cnt_cull;
    case 0x4F: return (uint8_t)(g->cnt_cull >> 8);
    case 0x54: return (uint8_t)g->vfirst;
    case 0x55: return (uint8_t)(g->vfirst >> 8);
    case 0x56: return (uint8_t)g->ffirst;
    case 0x57: return (uint8_t)(g->ffirst >> 8);
    case 0x58: return (uint8_t)g->faddr;
    case 0x59: return (uint8_t)g->nface;
    case 0x65: return (uint8_t)g->taddr;
    case 0x66: return (uint8_t)g->efirst;
    case 0x67: return (uint8_t)(g->efirst >> 8);
    case 0x68: return (uint8_t)(g->vaddr >> 8);
    case 0x69: return (uint8_t)(g->eaddr >> 8);
    case 0x6A: return (uint8_t)(g->nvert >> 8);
    case 0x6B: return (uint8_t)(g->nedge >> 8);
    case 0x6C: return (uint8_t)(g->faddr >> 8);
    case 0x6D: return (uint8_t)(g->nface >> 8);
    case 0x6E: return (uint8_t)(g->taddr >> 8);
    case 0x6F: return (uint8_t)((GEO3D_AW << 4) | g->ext);
    default:   return 0xFF;
    }
}

void geo3d_read_data(geo3d_t *g) {
    uint8_t r = g->rptr;
    if (r == 0x36)                 g->rptr = 0x30;
    else if ((r >> 4) == 4)        g->rptr = (uint8_t)(0x40 | ((r + 1) & 0xF));
    else if (r >= 0x30 && r < 0x36) g->rptr = (uint8_t)(r + 1);
}
