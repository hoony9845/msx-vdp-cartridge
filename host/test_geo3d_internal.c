/*
 * geo3d's internals against plain references (this file includes geo3d.c
 * for its static functions):
 *   - floor_div's 32-bit path against 64-bit floor division;
 *   - the face row spans (flat_span / tex_span from face_rows' corners)
 *     against face_span as it was before (64-bit throughout, the corners
 *     read for every row), every row of random faces in order and with
 *     jumps, and face_rows' skip of faces off the screen (all rows empty);
 *   - the heap sort of the painter's order against qsort.
 *
 *   usage: test_geo3d_internal [faces] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include "geo3d.c"

static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static int64_t ref_floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* ---- face_span before 38fb0f8 (with ref_floor_div) ---- */
typedef struct { int64_t x, u, v; bool set; } ref_cand_t;

static void ref_cand(ref_cand_t *l, ref_cand_t *r, int64_t x, int64_t u, int64_t v) {
    if (!l->set || x < l->x) { l->x = x; l->u = u; l->v = v; l->set = true; }
    if (!r->set || x > r->x) { r->x = x; r->u = u; r->v = v; r->set = true; }
}

static unsigned ref_face_span(const geo3d_t *g, const geo3d_vis_t *vis, int32_t y, uint8_t *b) {
    const geo3d_face_t *f = &g->fmem[vis->face];
    bool textured = g->textured && (f->base & 0x80);
    const uint8_t *uv = g->tmem[vis->face];
    int64_t xs[4], ys[4], us[4], vs[4];
    for (int k = 0; k < 4; k++) {
        const geo3d_proj_t *p = &g->pmem[f->idx[k] & MEM_MASK];
        xs[k] = p->sx;
        ys[k] = p->sy;
        us[k] = textured ? uv[2 * k] : 0;
        vs[k] = textured ? uv[2 * k + 1] : 0;
    }
    ref_cand_t L = {0}, R = {0};
    for (int k = 0; k < 4; k++) {
        int a = k, c = (k + 1) & 3;
        if (ys[a] == ys[c]) {
            if (y == ys[a]) {
                ref_cand(&L, &R, xs[a], us[a] * 256, vs[a] * 256);
                ref_cand(&L, &R, xs[c], us[c] * 256, vs[c] * 256);
            }
        } else {
            int64_t lo = ys[a] < ys[c] ? ys[a] : ys[c];
            int64_t hi = ys[a] < ys[c] ? ys[c] : ys[a];
            if (lo <= y && y <= hi) {
                int64_t dy = y - ys[a], den = ys[c] - ys[a];
                int64_t x = xs[a] + ref_floor_div(dy * (xs[c] - xs[a]), den);
                int64_t u = 0, v = 0;
                if (textured) {
                    u = us[a] * 256 + ref_floor_div(dy * (us[c] - us[a]) * 256, den);
                    v = vs[a] * 256 + ref_floor_div(dy * (vs[c] - vs[a]) * 256, den);
                }
                ref_cand(&L, &R, x, u, v);
            }
        }
    }
    if (!L.set) return 0;
    int64_t w = g->cfg[16];
    int64_t xl = L.x, xr = R.x;
    int64_t cl = xl > 0 ? xl : 0, cr = xr < w - 1 ? xr : w - 1;
    if (cl > cr) return 0;
    unsigned dy = (unsigned)((y + g->ypage) & 0x7FF);
    if (!textured) {
        int64_t nx = cr - cl;
        b[0] = (uint8_t)cl;  b[1] = (uint8_t)((cl >> 8) & 1);
        b[2] = (uint8_t)dy;  b[3] = (uint8_t)(dy >> 8);
        b[4] = (uint8_t)nx;  b[5] = (uint8_t)((nx >> 8) & 7);
        b[6] = 0;            b[7] = 0;
        b[8] = vis->color;   b[9] = 0;
        b[10] = (uint8_t)(0x70 | (g->lop & 0xF));
        return 11;
    }
    int64_t du = 0, dv = 0;
    if (xr != xl) {
        int64_t d = sat64(xr - xl, -131071, 131071);
        du = sat64(ref_floor_div(sat64(R.u - L.u, -131071, 131071), d), -32767, 32767);
        dv = sat64(ref_floor_div(sat64(R.v - L.v, -131071, 131071), d), -32767, 32767);
    }
    int64_t off = sat64(cl - xl, -131071, 131071);
    int64_t su = L.u + off * du, sv = L.v + off * dv;
    unsigned sx = (unsigned)((g->texx + asr(su, 8)) & 0xFFF);
    unsigned sy = (unsigned)((g->texy + vis->level * g->tstride + asr(sv, 8)) & 0x1FFF);
    int64_t nxp = cr - cl + 1;
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

static int test_floor_div(void) {
    static const int64_t edge[] = { 0, 1, -1, 2, -2, 7, -7, INT32_MAX, INT32_MIN,
                                    (int64_t)INT32_MAX + 1, (int64_t)INT32_MIN - 1, 65535, -65535 };
    const unsigned ne = sizeof edge / sizeof edge[0];
    for (unsigned i = 0; i < ne; i++)
        for (unsigned j = 0; j < ne; j++)
            if (edge[j] && floor_div(edge[i], edge[j]) != ref_floor_div(edge[i], edge[j])) {
                printf("FAIL floor_div %lld %lld\n", (long long)edge[i], (long long)edge[j]);
                return 1;
            }
    for (unsigned n = 0; n < 2000000; n++) {
        int64_t a = (int64_t)(int32_t)rnd() >> (rnd() % 32), b = (int64_t)(int32_t)rnd() >> (rnd() % 32);
        if (rnd() & 1) a *= (int64_t)(rnd() % 70000);
        if (!b) continue;
        if (floor_div(a, b) != ref_floor_div(a, b)) {
            printf("FAIL floor_div %lld %lld\n", (long long)a, (long long)b);
            return 1;
        }
    }
    return 0;
}

static geo3d_t g;

static void random_face(void) {
    g.textured = (rnd() & 7) != 0;
    g.cfg[16] = (rnd() & 3) ? 256 : 1 + rnd() % 600;
    g.cfg[17] = (rnd() & 3) ? 212 : 1 + rnd() % 500;
    g.texx = (uint16_t)(rnd() & 0xFFF); g.texy = (uint16_t)(rnd() & 0x1FFF);
    g.tstride = (uint8_t)rnd(); g.ypage = (uint16_t)(rnd() & 0x7FF); g.lop = (uint8_t)(rnd() & 15);
    unsigned kind = rnd() & 15;             /* 0-1 full int16, 2-3 near the 32-bit path's bounds */
    bool big = kind < 2, edge = kind == 2 || kind == 3;
    unsigned fi = rnd() & MEM_MASK;
    for (int k = 0; k < 4; k++) {
        unsigned vi = (fi * 4 + (unsigned)k) & MEM_MASK;
        g.fmem[fi].idx[k] = (uint16_t)vi;
        int32_t x = big ? (int16_t)rnd() : (int32_t)(rnd() % 500) - 120;
        int32_t y = big ? (int16_t)rnd() : (int32_t)(rnd() % 500) - 150;
        if (edge) {
            static const int32_t lim[6] = { -16385, -16384, -16383, 16382, 16383, 16384 };
            if (rnd() & 1) x = lim[rnd() % 6];
            if (rnd() & 1) y = lim[rnd() % 6];
        }
        if (k && (rnd() & 3) == 0) y = g.pmem[(fi * 4 + (unsigned)k - 1) & MEM_MASK].sy;   /* horizontal */
        if (k && (rnd() & 3) == 0) x = g.pmem[(fi * 4 + (unsigned)k - 1) & MEM_MASK].sx;   /* equal x */
        g.pmem[vi].sx = (int16_t)x;
        g.pmem[vi].sy = (int16_t)y;
        g.tmem[fi][2 * k] = (uint8_t)rnd();
        g.tmem[fi][2 * k + 1] = (uint8_t)rnd();
    }
    g.fmem[fi].base = (uint8_t)((rnd() & 7) ? 0x80 | (rnd() & 0x7F) : rnd() & 0x7F);
    g.vis[0].face = (uint16_t)fi;
    g.vis[0].color = (uint8_t)rnd();
    g.vis[0].level = (uint8_t)(rnd() & 7);
}

static int test_spans(unsigned faces, unsigned long *rows, unsigned long *drawn, unsigned long *skipped,
                      unsigned long *rowform) {
    for (unsigned n = 0; n < faces; n++) {
        random_face();
        face_rows(&g, &g.vis[0]);
        /* the rows face_rows would draw, or (skipped faces) its own bounds */
        int32_t ymin = 32767, ymax = -32768;
        for (int k = 0; k < 4; k++) {
            int32_t y = g.pmem[g.fmem[g.vis[0].face].idx[k] & MEM_MASK].sy;
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
        int32_t y0 = ymin < 0 ? 0 : ymin, y1 = ymax < g.cfg[17] - 1 ? ymax : g.cfg[17] - 1;
        bool skip = g.row_end < g.row && y0 <= y1;
        if (skip) (*skipped)++;
        for (int32_t y = y0; y <= y1; y++) {
            uint8_t a[GEO3D_CMD_MAX], b[GEO3D_CMD_MAX];
            memset(a, 0xAA, sizeof a);
            memset(b, 0xAA, sizeof b);
            unsigned la = ref_face_span(&g, &g.vis[0], y, a);
            unsigned lb = face_span(&g, &g.vis[0], y, b, NULL);
            (*rows)++;
            if (la) (*drawn)++;
            if (la != lb || memcmp(a, b, sizeof a) || (skip && la)) {
                printf("FAIL span: face %u row %d: %u / %u%s\n", n, y, la, lb, skip && la ? " (skipped)" : "");
                return 1;
            }
            /* the row form (geo3d_next_command_ex) packs to the same bytes */
            geo3d_lrmm_t r;
            unsigned lr = face_span(&g, &g.vis[0], y, b, &r);
            if (g.ftex ? (lr != (la ? GEO3D_LRMM_ROW : 0u)) : (lr != la)) {
                printf("FAIL row form: face %u row %d: %u / %u\n", n, y, la, lr);
                return 1;
            }
            if (g.ftex && la) {
                uint8_t c[GEO3D_CMD_MAX];
                c[0] = (uint8_t)r.sx;  c[1] = (uint8_t)(r.sx >> 8);
                c[2] = (uint8_t)r.sy;  c[3] = (uint8_t)(r.sy >> 8);
                c[4] = (uint8_t)r.dx;  c[5] = (uint8_t)(r.dx >> 8);
                c[6] = (uint8_t)r.dy;  c[7] = (uint8_t)(r.dy >> 8);
                c[8] = (uint8_t)r.nx;  c[9] = (uint8_t)(r.nx >> 8);
                c[10] = 1;             c[11] = 0;
                c[12] = r.color;       c[13] = 0;
                c[14] = (uint8_t)r.vx; c[15] = (uint8_t)(r.vx >> 8);
                c[16] = (uint8_t)r.vy; c[17] = (uint8_t)(r.vy >> 8);
                c[18] = r.cmd;
                if (memcmp(a, c, GEO3D_CMD_MAX)) {
                    printf("FAIL row form bytes: face %u row %d\n", n, y);
                    return 1;
                }
                (*rowform)++;
            }
            if ((rnd() & 63) == 0) y += (int32_t)(rnd() & 7);       /* jump */
        }
    }
    return 0;
}

static int test_sort(void) {
    static geo3d_vis_t a[GEO3D_NMEM], b[GEO3D_NMEM];
    for (unsigned t = 0; t < 20000; t++) {
        unsigned n = (rnd() & 3) ? rnd() % 40 : rnd() % GEO3D_NMEM;
        unsigned off = rnd() & MEM_MASK;
        for (unsigned i = 0; i < n; i++) {
            a[i].key = (rnd() & 1) ? (int32_t)(rnd() % 8) - 4 : (int32_t)rnd();   /* many equal keys */
            a[i].face = (uint16_t)((off + i) & MEM_MASK);
            a[i].color = (uint8_t)rnd();
            a[i].level = (uint8_t)rnd();
        }
        memcpy(b, a, sizeof a[0] * n);
        qsort(a, n, sizeof a[0], cmp_vis);
        for (unsigned i = n / 2; i-- > 0;) sift_down(b, i, n);       /* as setup() */
        for (unsigned m = n; m > 1;) {
            m--;
            geo3d_vis_t x = b[0]; b[0] = b[m]; b[m] = x;
            sift_down(b, 0, m);
        }
        if (memcmp(a, b, sizeof a[0] * n)) { printf("FAIL sort: array %u (%u)\n", t, n); return 1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    unsigned faces = argc > 1 ? (unsigned)atoi(argv[1]) : 100000;
    rs = argc > 2 ? (uint32_t)atoi(argv[2]) : 1;
    unsigned long rows = 0, drawn = 0, skipped = 0, rowform = 0;
    if (test_floor_div() || test_spans(faces, &rows, &drawn, &skipped, &rowform) || test_sort()) return 1;
    printf("test_geo3d_internal: floor_div, %lu face rows (%lu drawn, %lu textured in row form, %lu faces "
           "skipped off the screen) and the painter's sort identical to the references\n",
           rows, drawn, rowform, skipped);
    if (skipped == 0) { printf("FAIL: no face was skipped\n"); return 1; }
    if (rowform == 0) { printf("FAIL: no textured row\n"); return 1; }
    return 0;
}
