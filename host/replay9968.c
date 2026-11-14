/*
 * Replay a V9968 port trace through the core and write screenshots: the
 * V9968 counterpart of replay.c, for tools/compare/v9968.
 *
 *   usage: replay9968 <trace.txt> <out_dir>
 *
 * Trace format (one command per line, '#' starts a comment):
 *   w <port> <value>   OUT to port 0x88-0x8F (V9968 0x88-0x8C, geo3d 0x8D/0x8F)
 *   r <port>           IN from a port (applies the read side effects)
 *   frame              one video frame (line and frame interrupts happen)
 *   wait_ce            wait until the command engine and geo3d are idle
 *   shot <name>        render the current frame to <out_dir>/<name>.ppm
 *
 * Pictures are rendered without the cartridge's output gamma, like openMSX.
 * V9968_CMD_REFERENCE=1 runs LRMM without its fast path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v9968.h"

#define OUT_W 640
#define OUT_H 480

static uint8_t vram[V9968_VRAM_SIZE];
static v9968_t vdp;
static FILE *geo_log;       /* REPLAY_GEO3D_CMDS: geo3d's commands, tb_cmd.v format */

/* Commands and geo3d run until done or waiting for CPU data. */
static void run_cmd(void) {
    for (int i = 0; i < 1000000; i++) {
        if (geo_log && vdp.geo.running && !v9968_cmd_busy(&vdp)) {
            /* issue the command here to log it (what v9968_service does) */
            uint8_t b[GEO3D_CMD_MAX];
            unsigned n = geo3d_next_command(&vdp.geo, b);
            if (n == 0) vdp.geo.running = false;
            for (unsigned k = 0; k < n; k++) {
                unsigned r = geo3d_command_reg(n, k);
                fprintf(geo_log, "W %u %02x\n", r, b[k]);
                v9968_cmd_write_reg(&vdp, r, b[k]);
            }
            if (n) fputs("E\n", geo_log);
        }
        if (!v9968_service(&vdp, 1u << 20) && !vdp.geo.running) break;
    }
}

static void end_frame(void) {
    run_cmd();
    v9968_video_vblank_end(&vdp);
    unsigned h = v9968_display_height(&vdp);
    for (unsigned y = 0; y < h; y++) v9968_video_line(&vdp, y);
    v9968_video_vblank_start(&vdp);
}

static int write_shot(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);

    unsigned h = v9968_display_height(&vdp);
    unsigned scale = h <= OUT_H / 2 ? 2 : 1;
    unsigned top = (OUT_H - h * scale) / 2;
    uint16_t line[OUT_W];
    unsigned char rgb[OUT_W * 3];
    for (unsigned y = 0; y < OUT_H; y++) {
        if (y < top || y >= top + h * scale) {
            for (unsigned x = 0; x < OUT_W; x++) line[x] = v9968_backdrop_rgb565(&vdp);
        } else if ((y - top) % scale == 0) {
            v9968_render_line(&vdp, (y - top) / scale, line, OUT_W);
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

    /* sidecar with the display resolution in dots, used by compare.py */
    v9968_mode_t m = v9968_mode(&vdp);
    unsigned w = (m == V9968_MODE_G5 || m == V9968_MODE_G6 || m == V9968_MODE_T2) ? 512 : 256;
    snprintf(path, sizeof(path), "%s/%s.size", dir, name);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%u %u\n", w, h);
        fclose(f);
    }
    if (getenv("REPLAY_DUMP_VRAM")) {
        snprintf(path, sizeof(path), "%s/%s.vram", dir, name);
        f = fopen(path, "wb");
        if (f) {
            fwrite(vram, 1, V9968_VRAM_SIZE, f);
            fclose(f);
        }
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
    v9968_init(&vdp, vram);
    vdp.gamma = false;
    v9968_cmd_reference = getenv("V9968_CMD_REFERENCE") != NULL;
    v9968_video_vblank_end(&vdp);
    if (getenv("REPLAY_GEO3D_CMDS")) geo_log = fopen(getenv("REPLAY_GEO3D_CMDS"), "w");

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
            v9968_write_port(&vdp, (uint8_t)(a & 7), (uint8_t)b);
            run_cmd();
            writes++;
        } else if (strcmp(cmd, "r") == 0 && sscanf(buf, "%*s %li", (long *)&a) == 1) {
            run_cmd();
            (void)v9968_read_port(&vdp, (uint8_t)(a & 7));
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
