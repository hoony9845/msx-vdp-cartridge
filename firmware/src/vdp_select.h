/*
 * Which VDP the cartridge emulates: V9990 (GFX9000, ports 60h-67h) or
 * V9968 + geo3d (the V9968 cartridge block at offsets 8-Fh of the decoded
 * port range: 88h-8Fh when SW1 sets the decoder to 8xh, 68h-6Fh on a rev 0.1
 * board whose decoder is fixed at 6xh).
 *
 * PCB rev 0.2 has a slide switch (SW1) that sets both the port decoder
 * (60h-6Fh or 88h-8Fh) and GP22; the firmware follows it (CMake option
 * MSX_VDP_SWITCH, on by default). Rev 0.1 boards have no switch: build with
 * -DMSX_VDP_SWITCH=OFF and the choice is a firmware setting instead, the
 * build default (MSX_VDP_DEFAULT) unless one was saved in the last flash
 * sector, changed over USB serial ("chip v9990" / "chip v9968"), which
 * saves it and reboots.
 */
#ifndef VDP_SELECT_H
#define VDP_SELECT_H

typedef enum {
    VDP_CHIP_V9990 = 0,
    VDP_CHIP_V9968 = 1,
} vdp_chip_t;

/* Reads the saved setting (writes a pending one first). Call at boot,
 * before the second core and the video output start. */
vdp_chip_t vdp_select_boot(void);

const char *vdp_chip_name(vdp_chip_t chip);

/* Polls USB serial for a command; never returns if the chip changes. */
void vdp_select_poll(vdp_chip_t current);

#endif
