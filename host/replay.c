/*
 * Replay a V9990 port trace through the core and write screenshots.
 *
 *   usage: replay <trace.txt> <out_dir>
 *
 * Trace format (one command per line, '#' starts a comment):
 *   w <port> <value>   OUT to port 0x60-0x6F (numbers in C syntax, e.g. 0x63)
 *   r <port>           IN from a port (applies the read side effects)
 *   frame              end of a video frame (vblank, line counters restart)
 *   wait_ce            wait until the command engine is idle
 *   shot <name>        render the current frame to <out_dir>/<name>.ppm
 *
 * The same files are executed in openMSX by tools/compare/run_openmsx.tcl and
 * produced from real software by tools/compare/record.tcl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v9990.h"

#define OUT_W 640
#define OUT_H 480

static uint8_t vram[V9990_VRAM_SIZE];
static v9990_t vdp;

static void run_cmd(void) {
    while (v9990_cmd_step(&vdp, 1u << 20)) {
        if (vdp.cmd.op == V9990_CMD_LMMC) break;   /* waits for CPU data */
    }
}

static void end_frame(void) {
    run_cmd();
    v9990_video_vblank_start(&vdp);
    v9990_video_vblank_end(&vdp);
}

static int write_shot(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);

    unsigned h = v9990_display_height(&vdp);
    unsigned scale = h <= OUT_H / 2 ? 2 : 1;
    unsigned top = (OUT_H - h * scale) / 2;
    uint16_t line[OUT_W];
    unsigned char rgb[OUT_W * 3];
    for (unsigned y = 0; y < OUT_H; y++) {
        if (y < top || y >= top + h * scale) {
            for (unsigned x = 0; x < OUT_W; x++) line[x] = v9990_backdrop_rgb565(&vdp);
        } else {
            unsigned src = (y - top) / scale;
            if ((y - top) % scale == 0) v9990_video_line_start(&vdp, src);
            v9990_render_line(&vdp, src, line, OUT_W);
        }
        for (unsigned x = 0; x < OUT_W; x++) {
            uint16_t c = line[x];
            rgb[x * 3 + 0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
            rgb[x * 3 + 1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
            rgb[x * 3 + 2] = (unsigned char)((c & 0x1F) * 255 / 31);
        }
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    fclose(f);
    /* sidecar with the V9990 display resolution, used by compare.py */
    snprintf(path, sizeof(path), "%s/%s.size", dir, name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u %u\n", v9990_display_width(&vdp), h);
        fclose(f);
    }
    printf("shot %s/%s.ppm\n", dir, name);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <trace.txt> <out_dir>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    v9990_init(&vdp, vram);
    v9990_video_vblank_end(&vdp);

    char buf[512];
    unsigned lineno = 0, writes = 0, reads = 0, frames = 0;
    while (fgets(buf, sizeof(buf), in)) {
        lineno++;
        char *hash = strchr(buf, '#');
        if (hash) *hash = 0;
        char cmd[32], arg1[256];
        unsigned long a = 0, b = 0;
        int n = sscanf(buf, "%31s %255s", cmd, arg1);
        if (n < 1) continue;
        if (strcmp(cmd, "w") == 0 && sscanf(buf, "%*s %li %li", (long *)&a, (long *)&b) == 2) {
            v9990_write_port(&vdp, (uint8_t)(a & 0x0F), (uint8_t)b);
            if ((a & 0x0F) == V9990_PORT_CMD_DATA || (a & 0x0F) == V9990_PORT_REG_DATA) run_cmd();
            writes++;
        } else if (strcmp(cmd, "r") == 0 && sscanf(buf, "%*s %li", (long *)&a) == 1) {
            run_cmd();
            (void)v9990_read_port(&vdp, (uint8_t)(a & 0x0F));
            reads++;
        } else if (strcmp(cmd, "frame") == 0) {
            end_frame();
            frames++;
        } else if (strcmp(cmd, "wait_ce") == 0) {
            run_cmd();
        } else if (strcmp(cmd, "shot") == 0 && n == 2) {
            run_cmd();
            if (write_shot(argv[2], arg1)) return 1;
        } else {
            fprintf(stderr, "%s:%u: cannot parse: %s", argv[1], lineno, buf);
            return 1;
        }
    }
    fclose(in);
    printf("replayed %u writes, %u reads, %u frames\n", writes, reads, frames);
    return 0;
}
