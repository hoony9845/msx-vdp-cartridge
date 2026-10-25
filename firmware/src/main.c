/*
 * MSX VDP cartridge emulator for RP2350: V9990 (GFX9000 compatible) or
 * V9968 + geo3d (V9968 cartridge compatible), chosen at boot (vdp_select.h).
 *
 *   core0 : DVI output IRQ + scanline renderer, USB serial commands
 *   core1 : MSX bus events + command engine (bus.c)
 *   VRAM  : V9990 512KB in external PSRAM, V9968 256KB in internal SRAM
 */
#include <stdio.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "board_pins.h"
#include "bus.h"
#include "dvi_out.h"
#include "psram.h"
#include "vdp_select.h"

#define SYS_CLOCK_KHZ  252000u      /* = 10 x 25.2MHz DVI pixel clock */
#define HSTX_CLOCK_HZ  126000000u   /* 2 bits per clock, 5 clocks per pixel */

static vdp_chip_t chip;
static v9990_t v9990;
static v9968_t v9968;
static uint8_t v9968_vram[V9968_VRAM_SIZE] __attribute__((aligned(4)));
static dvi_vdp_t video;

/* Slow the flash down before raising clk_sys (must run from RAM). */
static void __not_in_flash_func(set_flash_divisor)(uint32_t clkdiv, uint32_t rxdelay) {
    uint32_t t = qmi_hw->m[0].timing;
    t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    t |= clkdiv << QMI_M0_TIMING_CLKDIV_LSB | rxdelay << QMI_M0_TIMING_RXDELAY_LSB;
    qmi_hw->m[0].timing = t;
}

static void init_clocks(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_flash_divisor(3, 2);                     /* 84MHz flash at 252MHz */
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    SYS_CLOCK_KHZ * 1000u, HSTX_CLOCK_HZ);
}

/* ---- video callbacks ---- */

static unsigned v9990_height(const void *v) { return v9990_display_height(v); }
static void v9990_vbs(void *v) { v9990_video_vblank_start(v); }
static void v9990_vbe(void *v) { v9990_video_vblank_end(v); }
static void v9990_line(void *v, unsigned n) { v9990_video_line_start(v, n); }

static unsigned v9968_height(const void *v) { return v9968_display_height(v); }
static void v9968_vbs(void *v) { v9968_video_vblank_start(v); }
static void v9968_vbe(void *v) { v9968_video_vblank_end(v); }
static void v9968_line(void *v, unsigned n) { v9968_video_line(v, n); }
static void __not_in_flash_func(v9968_hblank)(void *v, bool on) {
    v9968_t *p = v;
    if (on) V9968_SET_BITS(p->video_status, V9968_S2_HR);
    else    V9968_CLR_BITS(p->video_status, V9968_S2_HR);
}

static void __not_in_flash_func(set_border)(uint16_t bd) {
    for (uint i = 0; i < DVI_WIDTH; i++) dvi_lines.border[i] = bd;
}

/* V9990: render lines just ahead of the DVI scan-out. */
static void __not_in_flash_func(render_loop_v9990)(void) {
    uint32_t frame = dvi_lines.frame;
    uint32_t next = 0;

    for (;;) {
        uint32_t f = dvi_lines.frame;
        if (f != frame) {
            frame = f;
            next = 0;
            set_border(v9990_backdrop_rgb565(&v9990));
            vdp_select_poll(chip);
        }
        int32_t cur = dvi_lines.src_line;
        if (next < dvi_lines.src_height &&
            (int32_t)next <= cur + (DVI_LINE_BUFFERS - 1)) {
            v9990_render_line(&v9990, next,
                              dvi_lines.line[next & (DVI_LINE_BUFFERS - 1)],
                              DVI_WIDTH);
            next++;
        }
    }
}

/* V9968: render line n during the last output line of n - 1, after the
 * line interrupt of n - 1 had (most of) a line time to change registers. */
static void __not_in_flash_func(render_loop_v9968)(void) {
    uint32_t frame = dvi_lines.frame;
    uint32_t next = 0;

    for (;;) {
        uint32_t f = dvi_lines.frame;
        if (f != frame) {
            frame = f;
            next = 0;
            set_border(v9968_backdrop_rgb565(&v9968));
            vdp_select_poll(chip);
        }
        int32_t pos = dvi_lines.pos;
        int32_t scale = (int32_t)dvi_lines.scale;
        uint32_t height = dvi_lines.src_height;
        if (next < height && pos >= (int32_t)next * scale - 1 &&
            pos < (int32_t)height * scale) {
            v9968_render_line(&v9968, next,
                              dvi_lines.line[next & (DVI_LINE_BUFFERS - 1)],
                              DVI_WIDTH);
            next++;
        }
    }
}

int main(void) {
    init_clocks();
    chip = vdp_select_boot();
    stdio_init_all();

    printf("MSX VDP emulator: %s, clk_sys %lu Hz\n", vdp_chip_name(chip),
           (unsigned long)clock_get_hz(clk_sys));
    printf("type 'chip v9990' or 'chip v9968' to switch\n");

    if (chip == VDP_CHIP_V9968) {
        v9968_init(&v9968, v9968_vram);
        bus_init(chip, &v9968);
        video = (dvi_vdp_t){ &v9968, v9968_height, v9968_vbs, v9968_vbe, v9968_line,
                             v9968_hblank, true };
    } else {
        size_t psram_size = psram_init();
        printf("PSRAM %u KB\n", (unsigned)(psram_size / 1024));
        if (psram_size < V9990_VRAM_SIZE) {
            printf("PSRAM not found: halting (use 'chip v9968' for the V9968)\n");
            for (;;) vdp_select_poll(chip);
        }
        v9990_init(&v9990, PSRAM_BASE);
        bus_init(chip, &v9990);
        video = (dvi_vdp_t){ &v9990, v9990_height, v9990_vbs, v9990_vbe, v9990_line,
                             NULL, false };
    }
    multicore_launch_core1(bus_core1_main);

    dvi_out_start(&video);
    if (chip == VDP_CHIP_V9968) render_loop_v9968();
    else                        render_loop_v9990();
}
