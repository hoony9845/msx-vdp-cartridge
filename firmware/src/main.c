/*
 * MSX VDP cartridge emulator for RP2350: V9990 (GFX9000 compatible) or
 * V9968 + geo3d (V9968 cartridge compatible), chosen at boot (vdp_select.h).
 *
 *   core0 : DVI output IRQ + scanline renderer, USB serial commands
 *   core1 : MSX bus events + command engine (bus.c)
 *   VRAM  : V9990 512KB in external PSRAM, V9968 256KB in internal SRAM
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
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
/* 256KB of SRAM: the V9968's VRAM, or VRAM0 (physical 0x00000-0x3FFFF) of
 * the V9990, whose VRAM1 is in PSRAM. Half of all V9990 VRAM traffic, and
 * the P1 layer A patterns and sprites, then skip the PSRAM. */
static uint8_t v9968_vram[V9968_VRAM_SIZE] __attribute__((aligned(4)));
static_assert(V9968_VRAM_SIZE == V9990_VRAM_SIZE / 2, "VRAM0 must fit the SRAM buffer");
static dvi_vdp_t video;

/* Slow the flash down before raising clk_sys (must run from RAM). */
static void __no_inline_not_in_flash_func(set_flash_divisor)(uint32_t clkdiv, uint32_t rxdelay) {
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

static unsigned __not_in_flash_func(v9990_height)(const void *v) { return v9990_display_height(v); }
static void __not_in_flash_func(v9990_vbs)(void *v) { v9990_video_vblank_start(v); }
static void __not_in_flash_func(v9990_vbe)(void *v) { v9990_video_vblank_end(v); }
static void __not_in_flash_func(v9990_line)(void *v, unsigned n) { v9990_video_line_start(v, n); }

/* HR (P#5 bit 5): software waits for it, e.g. in a line interrupt handler
 * with interrupts disabled, so it must toggle. Published to the bus read
 * table at once: core1 refreshes it only once per loop pass. */
static void __not_in_flash_func(v9990_hblank)(void *v, bool on) {
    v9990_t *p = v;
    if (on) V9990_SET_BITS(p->status, V9990_ST_HR);
    else    V9990_CLR_BITS(p->status, V9990_ST_HR);
    bus_refresh_status();
}

/* Renderer row fetch: one PSRAM burst instead of a cache miss per 8 bytes. */
static void __not_in_flash_func(v9990_fetch)(const v9990_t *v, uint32_t phys,
                                             uint8_t *dst, uint32_t len) {
    if (phys & 0x40000u) psram_stream_read(dst, v->vram1 + (phys & 0x3FFFFu), len);
    else {                                       /* VRAM0: SRAM (memcpy is in flash) */
        const uint32_t *s = (const uint32_t *)(v->vram + phys);
        uint32_t *d = (uint32_t *)dst;
        for (uint32_t i = 0; i < len / 4u; i++) d[i] = s[i];
    }
}

/* USB serial "bench": time the parts of rendering one line of the current
 * V9990 picture (on a copy of the VDP state), to find where time goes. */
static v9990_t bench_vdp;
static uint16_t bench_out[DVI_WIDTH];
static uint8_t bench_buf[512] __attribute__((aligned(4)));

static uint32_t __not_in_flash_func(bench_render)(const v9990_t *v, unsigned line) {
    uint32_t t0 = timer_hw->timerawl;
    for (int i = 0; i < 20; i++) v9990_render_line(v, line, bench_out, DVI_WIDTH);
    return (timer_hw->timerawl - t0) * 10u / 20u;       /* 0.1 us units */
}

static uint32_t __not_in_flash_func(bench_fetch)(uint32_t phys, uint32_t len) {
    uint32_t t0 = timer_hw->timerawl;
    for (int i = 0; i < 20; i++) v9990_fetch(&v9990, phys, bench_buf, len);
    return (timer_hw->timerawl - t0) * 10u / 20u;
}

void main_bench(void) {
    if (chip != VDP_CHIP_V9990) { printf("bench: V9990 only\n"); return; }
    unsigned line = dvi_lines.src_height / 2;
    uint32_t irq = save_and_disable_interrupts();
    bench_vdp = v9990;
    uint32_t full = bench_render(&bench_vdp, line);
    bench_vdp.regs[V9990_R_CONTROL] |= V9990_R8_SPD;
    uint32_t nospr = bench_render(&bench_vdp, line);
    bench_vdp.regs[V9990_R_CONTROL] = v9990.regs[V9990_R_CONTROL];
    bench_vdp.vram1 = bench_vdp.vram;                    /* all in SRAM: CPU time only */
    bench_vdp.vram_fetch = NULL;
    uint32_t sram = bench_render(&bench_vdp, line);
    bench_vdp = v9990;
    bench_vdp.regs[V9990_R_CONTROL] &= (uint8_t)~0x80;   /* display off: backdrop only */
    uint32_t blank = bench_render(&bench_vdp, line);
    uint32_t f0 = bench_fetch(0x3FE00u, 500), f1 = bench_fetch(0x7C000u, 128);
    uint32_t f1b = bench_fetch(0x40000u, 512);
    restore_interrupts(irq);
    printf("bench line %u (IRQs off, x0.1us): full %lu, no sprites %lu, all-SRAM %lu, blank %lu; "
           "fetch VRAM0 500B %lu, VRAM1 128B %lu, VRAM1 512B %lu\n", line,
           (unsigned long)full, (unsigned long)nospr, (unsigned long)sram, (unsigned long)blank,
           (unsigned long)f0, (unsigned long)f1, (unsigned long)f1b);
}

static unsigned __not_in_flash_func(v9968_height)(const void *v) { return v9968_display_height(v); }
static void __not_in_flash_func(v9968_vbs)(void *v) { v9968_video_vblank_start(v); }
static void __not_in_flash_func(v9968_vbe)(void *v) { v9968_video_vblank_end(v); }
static void __not_in_flash_func(v9968_line)(void *v, unsigned n) { v9968_video_line(v, n); }
static void __not_in_flash_func(v9968_hblank)(void *v, bool on) {
    v9968_t *p = v;
    if (on) V9968_SET_BITS(p->video_status, V9968_S2_HR);
    else    V9968_CLR_BITS(p->video_status, V9968_S2_HR);
}

static void __not_in_flash_func(set_border)(uint16_t bd) {
    for (uint i = 0; i < DVI_WIDTH; i++) dvi_lines.border.px[i] = bd;
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
        /* VDP line on screen now (negative before the picture). After the
         * picture (bottom border) the new frame waits for the blank. */
        int32_t pos = dvi_lines.pos, scale = (int32_t)dvi_lines.scale;
        int32_t height = (int32_t)dvi_lines.src_height;
        int32_t cur = pos >= height * scale ? -DVI_LINE_BUFFERS - 1
                    : pos >= 0 ? pos / scale : -((-pos + scale - 1) / scale);
        if (next < dvi_lines.src_height &&
            (int32_t)next <= cur + (DVI_LINE_BUFFERS - 1)) {
            if ((int32_t)next <= cur) dvi_lines.render_late++;
            uint32_t t0 = timer_hw->timerawl;
            v9990_render_line(&v9990, next,
                              dvi_lines.line[next & (DVI_LINE_BUFFERS - 1)].px,
                              DVI_WIDTH);
            uint32_t dt = timer_hw->timerawl - t0;
            dvi_lines.render_lines++;
            dvi_lines.render_us += dt;
            if (dt > dvi_lines.render_max_us) dvi_lines.render_max_us = dt;
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
                              dvi_lines.line[next & (DVI_LINE_BUFFERS - 1)].px,
                              DVI_WIDTH);
            next++;
        }
    }
}

/* GP27/GP28 drive /INT and /WAIT through the 74LVC07 and reset to
 * pull-down, which asserts both: release them before anything else, so
 * the MSX keeps running even if the VDP never starts (e.g. no PSRAM). */
static void release_bus_outputs(void) {
    gpio_init(PIN_INT);  gpio_put(PIN_INT, 1);  gpio_set_dir(PIN_INT, GPIO_OUT);
    gpio_init(PIN_WAIT); gpio_put(PIN_WAIT, 1); gpio_set_dir(PIN_WAIT, GPIO_OUT);
}

/* -DMSX_BOOT_TRACE=1: wait for the USB terminal, then report each boot step. */
#if MSX_BOOT_TRACE
#include "pico/stdio_usb.h"
#include "hardware/xip_cache.h"
#if defined(WAVESHARE_CORE2350B_LED_PIN)
#define TRACE_LED WAVESHARE_CORE2350B_LED_PIN
#else
#define TRACE_LED PICO_DEFAULT_LED_PIN
#endif
#define TRACE(...) do { printf("[boot] " __VA_ARGS__); printf("\n"); stdio_flush(); sleep_ms(50); } while (0)
static void trace_wait_terminal(void) {
    gpio_init(TRACE_LED);
    gpio_set_dir(TRACE_LED, GPIO_OUT);
    for (int i = 0; i < 600 && !stdio_usb_connected(); i++) {
        gpio_xor_mask64(1ull << TRACE_LED);
        sleep_ms(100);
    }
    gpio_put(TRACE_LED, 1);
    sleep_ms(200);
}
#else
#define TRACE(...) do { } while (0)
static void trace_wait_terminal(void) { }
#endif

int main(void) {
    release_bus_outputs();
    init_clocks();
    chip = vdp_select_boot();
    stdio_init_all();
    trace_wait_terminal();
    TRACE("chip %s, clk_sys %lu", vdp_chip_name(chip), (unsigned long)clock_get_hz(clk_sys));

    printf("MSX VDP emulator: %s, clk_sys %lu Hz\n", vdp_chip_name(chip),
           (unsigned long)clock_get_hz(clk_sys));
#if MSX_VDP_SWITCH
    printf("chosen by the cartridge switch SW1 (change it with the power off)\n");
#else
    printf("type 'chip v9990' or 'chip v9968' to switch\n");
#endif

    if (chip == VDP_CHIP_V9968) {
        v9968_init(&v9968, v9968_vram);
        bus_init(chip, &v9968);
        video = (dvi_vdp_t){ &v9968, v9968_height, v9968_vbs, v9968_vbe, v9968_line,
                             v9968_hblank, true };
    } else {
        TRACE("psram_init");
        size_t psram_size = psram_init();
        TRACE("psram %u KB, KGD %02x EID %02x", (unsigned)(psram_size / 1024),
              psram_id_kgd, psram_id_eid);
        printf("PSRAM %u KB\n", (unsigned)(psram_size / 1024));
        if (psram_size < V9990_VRAM_SIZE) {
            /* Repeat the message: the boot banner is gone before a
             * terminal can be opened. */
            for (absolute_time_t t = get_absolute_time();;) {
                if (time_reached(t)) {
                    printf("PSRAM not found (%u KB): V9990 halted, the MSX bus is "
                           "not answered (use the V9968)\n",
                           (unsigned)(psram_size / 1024));
                    t = make_timeout_time_ms(1000);
                }
                vdp_select_poll(chip);
            }
        }
        TRACE("psram test");
#if MSX_BOOT_TRACE
        {
            volatile uint32_t *m = (volatile uint32_t *)PSRAM_BASE;
            for (uint32_t i = 0; i < 4; i++) m[i * 32768] = 0xA5000000u | i;
            xip_cache_clean_all();
            for (uint32_t i = 0; i < 4; i++) TRACE("  [%lu] = %08lx", (unsigned long)i * 131072, (unsigned long)m[i * 32768]);
        }
#endif
        TRACE("v9990_init");
        v9990_init_banks(&v9990, v9968_vram, PSRAM_BASE);
        psram_stream_init();
        v9990.vram_fetch = v9990_fetch;
        TRACE("bus_init");
        bus_init(chip, &v9990);
        video = (dvi_vdp_t){ &v9990, v9990_height, v9990_vbs, v9990_vbe, v9990_line,
                             v9990_hblank, false };
    }
    TRACE("core1 launch");
    multicore_launch_core1(bus_core1_main);

    TRACE("dvi_out_start");
    dvi_out_start(&video);
    TRACE("render loop");
    if (chip == VDP_CHIP_V9968) render_loop_v9968();
    else                        render_loop_v9990();
}
