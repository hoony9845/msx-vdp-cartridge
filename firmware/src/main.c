/*
 * MSX V9990 (GFX9000 compatible) cartridge emulator for RP2350.
 *
 *   core0 : DVI output IRQ + scanline renderer
 *   core1 : MSX bus events + command engine (bus.c)
 *   VRAM  : 512KB in external PSRAM
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
#include "v9990.h"

#define SYS_CLOCK_KHZ  252000u      /* = 10 x 25.2MHz DVI pixel clock */
#define HSTX_CLOCK_HZ  126000000u   /* 2 bits per clock, 5 clocks per pixel */

static v9990_t vdp;

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

/* Render V9990 lines just ahead of the DVI scan-out. */
static void __not_in_flash_func(render_loop)(void) {
    uint32_t frame = dvi_lines.frame;
    uint32_t next = 0;

    for (;;) {
        uint32_t f = dvi_lines.frame;
        if (f != frame) {
            frame = f;
            next = 0;
            uint16_t bd = v9990_backdrop_rgb565(&vdp);
            for (uint i = 0; i < DVI_WIDTH; i++) dvi_lines.border[i] = bd;
        }
        int32_t cur = dvi_lines.src_line;
        if (next < dvi_lines.src_height &&
            (int32_t)next <= cur + (DVI_LINE_BUFFERS - 1)) {
            v9990_render_line(&vdp, next,
                              dvi_lines.line[next & (DVI_LINE_BUFFERS - 1)],
                              DVI_WIDTH);
            next++;
        }
    }
}

int main(void) {
    init_clocks();
    stdio_init_all();

    size_t psram_size = psram_init();
    printf("V9990 emulator: clk_sys %lu Hz, PSRAM %u KB\n",
           (unsigned long)clock_get_hz(clk_sys), (unsigned)(psram_size / 1024));
    if (psram_size < V9990_VRAM_SIZE) {
        printf("PSRAM not found: halting\n");
        for (;;) tight_loop_contents();
    }

    v9990_init(&vdp, PSRAM_BASE);
    bus_init(&vdp);
    multicore_launch_core1(bus_core1_main);

    dvi_out_start(&vdp);
    render_loop();
}
