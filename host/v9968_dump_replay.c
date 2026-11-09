/*
 * Render one V9968 picture from the firmware's serial "dump" (the log of a
 * terminal session, any text around the dump is skipped) and time it:
 *
 *   v9968_dump_replay <log> [out.ppm]
 *
 * The picture is what the cartridge was showing, so renderer changes can be
 * timed and checked on exactly the scene that was slow on the hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "v9968.h"
static uint8_t vram[V9968_VRAM_SIZE];
static v9968_t vdp;
static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e9+t.tv_nsec; }
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <log> [out.ppm]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r"); if (!f) { perror(argv[1]); return 1; }
    v9968_init(&vdp, vram);
    static char buf[1 << 16]; int in = 0;
    while (fgets(buf, sizeof buf, f)) {
        char *p = strstr(buf, "DUMP begin"); if (p) { in = 1; continue; }
        if (!in) continue;
        if (strstr(buf, "DUMP end")) break;
        if (!strncmp(buf, "regs", 4)) { char *q = buf + 4; for (int i = 0; i < 64; i++) vdp.regs[i] = (uint8_t)strtoul(q, &q, 16); }
        else if (!strncmp(buf, "pal", 3)) { char *q = buf + 3; for (int i = 0; i < 256; i++) vdp.palette[i] = (uint16_t)strtoul(q, &q, 16); vdp.pal_gen++; }
        else if (!strncmp(buf, "misc", 4)) { unsigned a,b,c,d,e; sscanf(buf + 4, "%u %u %u %u %u", &a,&b,&c,&d,&e); vdp.field=(uint8_t)a; vdp.blink_page=b; vdp.shuffle_start=(uint8_t)c; vdp.gamma=d; vdp.sprite_status=(uint8_t)e; }
        else if (buf[0] == 'V') { unsigned a; char hex[200]; if (sscanf(buf, "V %x %128s", &a, hex) == 2) for (int i = 0; i < 64; i++) { unsigned x; sscanf(hex + 2*i, "%2x", &x); vram[a + i] = (uint8_t)x; } }
    }
    unsigned h = v9968_display_height(&vdp);
    static uint16_t line[640 * 480];
    double tot = 0, mx = 0;
    for (unsigned y = 0; y < h; y++) { double t0 = now_ns(); v9968_render_line(&vdp, y, line + y * 640, 640); double d = now_ns() - t0; tot += d; if (d > mx) mx = d; }
    printf("mode %d height %u: avg %.0f ns max %.0f ns per line\n", (int)v9968_mode(&vdp), h, tot / h, mx);
    if (argc > 2) { FILE *o = fopen(argv[2], "wb"); fprintf(o, "P6\n640 %u\n255\n", h);
        for (unsigned i = 0; i < 640 * h; i++) { uint16_t c = line[i]; fputc(((c >> 11) & 31) * 255 / 31, o); fputc(((c >> 5) & 63) * 255 / 63, o); fputc((c & 31) * 255 / 31, o); } fclose(o); }
    return 0;
}
