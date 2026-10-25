/*
 * Runs a geo3d stimulus file (the format of geo3d/sim/gen_*scenes.py in
 * alexmoncks/V9968_Cartridge) through core/geo3d.c and prints the same log
 * as the reference model's replay(), to diff against its *_expect.txt:
 *
 *   W <sel> <byte>   port write (sel 0 = index, 1 = data)
 *   R                wait for the RUN: logs every command (L = LINE, M = LRMM)
 *   K                reads SKIPPED/DRAWN/CULLED from 4Ah
 *   G <idx> <n>      reads n registers from idx
 *   F <n>            frame marker, copied
 *
 * Usage: geo3d_replay stim.txt > log.txt
 */
#include <stdio.h>
#include <string.h>
#include "geo3d.h"

static geo3d_t geo;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s stim.txt\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    geo3d_reset(&geo);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned a, b;
        switch (line[0]) {
        case 'W':
            if (sscanf(line + 1, "%u %x", &a, &b) == 2) geo3d_write(&geo, (int)a, (uint8_t)b);
            break;
        case 'R': {
            uint8_t c[GEO3D_CMD_MAX];
            unsigned n;
            while ((n = geo3d_next_command(&geo, c)) != 0) {
                putchar(n == GEO3D_CMD_MAX ? 'M' : 'L');
                for (unsigned k = 0; k < n; k++) printf(" %02x", c[k]);
                putchar('\n');
            }
            geo.running = false;
            break;
        }
        case 'K': {
            geo3d_write(&geo, 0, 0x4A);
            unsigned v[6];
            for (int k = 0; k < 6; k++) {
                v[k] = geo3d_peek_data(&geo);
                geo3d_read_data(&geo);
            }
            printf("K %04x %04x %04x\n", v[0] | v[1] << 8, v[2] | v[3] << 8, v[4] | v[5] << 8);
            break;
        }
        case 'G':
            if (sscanf(line + 1, "%x %u", &a, &b) == 2) {
                geo3d_write(&geo, 0, (uint8_t)a);
                printf("G %02x", a);
                for (unsigned k = 0; k < b; k++) {
                    printf(" %02x", geo3d_peek_data(&geo));
                    geo3d_read_data(&geo);
                }
                putchar('\n');
            }
            break;
        case 'F':
            fputs(line, stdout);
            break;
        default:
            break;
        }
    }
    fclose(f);
    return 0;
}
