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
#include "hardware/pll.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"
#include "board_pins.h"
#include "bus.h"
#include "dvi_out.h"
#include "psram.h"
#include "vdp_select.h"

/* Clocks (overclocked: the V9968 sprite mode 3 renderer needs it):
 *   pll_sys 252MHz -> clk_hstx /2 = 126MHz (10 x 25.2MHz DVI pixel clock)
 *   pll_usb 384MHz -> clk_sys, and clk_usb /8 = 48MHz
 * clk_hstx has only an integer divider without duty-cycle correction, and
 * HSTX shifts on both edges: clk_sys/3 (378MHz) gave a 33% duty cycle and
 * mostly black pictures on the monitor; an even divider keeps it at 50%. */
#define PLL_SYS_KHZ    252000u
#define SYS_CLOCK_KHZ  384000u
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
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_flash_divisor(5, 3);                     /* 76.8MHz flash at 384MHz */
    set_sys_clock_khz(PLL_SYS_KHZ, true);        /* clk_sys = pll_sys for now */
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    PLL_SYS_KHZ * 1000u, HSTX_CLOCK_HZ);
    /* pll_usb 1536MHz VCO / 4 / 1 = 384MHz (USB is not running yet) */
    pll_init(pll_usb, 1, 1536u * MHZ, 4, 1);
    clock_configure(clk_usb, 0, CLOCKS_CLK_USB_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    SYS_CLOCK_KHZ * 1000u, 48u * MHZ);
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    SYS_CLOCK_KHZ * 1000u, SYS_CLOCK_KHZ * 1000u);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    SYS_CLOCK_KHZ * 1000u, SYS_CLOCK_KHZ * 1000u);
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

uint32_t __not_in_flash_func(v9968_prof_now)(void) { return timer_hw->timerawl; }
/* DWT cycle counter of this core (enabled by "bench" on core0) */
uint32_t __not_in_flash_func(v9968_prof_cycles)(void) { return *(volatile uint32_t *)0xE0001004u; }
static void enable_cycle_counter(void) {
    *(volatile uint32_t *)0xE000EDFCu |= 1u << 24;          /* DEMCR.TRCENA */
    *(volatile uint32_t *)0xE0001000u |= 1u;                /* DWT_CTRL.CYCCNTENA */
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

#ifdef V9968_PROF
extern volatile uint32_t v9968_prof_us[6];
extern volatile uint32_t v9968_prof_cnt[5];
extern volatile uint32_t v9968_prof_cyc[3];
#endif
static uint32_t bench_cnt[8];             /* per frame: v9968_prof_cnt, v9968_prof_cyc */
static v9968_t bench_v9968;

/* V9968: every line of the current picture once, on a copy of the VDP
 * state, with IRQs off (core1 keeps running) and then with sprites off. */
static void __not_in_flash_func(bench_v9968_frame)(uint32_t res[8]) {
    unsigned h = v9968_display_height(&bench_v9968);
#ifdef V9968_PROF
    uint32_t p0[6], c0[8];
    for (int i = 0; i < 6; i++) p0[i] = v9968_prof_us[i];
    for (int i = 0; i < 5; i++) c0[i] = v9968_prof_cnt[i];
    for (int i = 0; i < 3; i++) c0[5 + i] = v9968_prof_cyc[i];
#endif
    uint32_t t0 = timer_hw->timerawl, mx = 0;
    for (unsigned y = 0; y < h; y++) {
        uint32_t t = timer_hw->timerawl;
        v9968_render_line(&bench_v9968, y, bench_out, DVI_WIDTH);
        t = timer_hw->timerawl - t;
        if (t > mx) mx = t;
    }
    res[0] = (timer_hw->timerawl - t0) * 10u / h;        /* 0.1 us units */
    res[1] = mx;
#ifdef V9968_PROF
    for (int i = 0; i < 6; i++) res[2 + i] = (v9968_prof_us[i] - p0[i]) * 10u / h;
    for (int i = 0; i < 5; i++) bench_cnt[i] = v9968_prof_cnt[i] - c0[i];
    for (int i = 0; i < 3; i++) bench_cnt[5 + i] = v9968_prof_cyc[i] - c0[5 + i];
#else
    for (int i = 0; i < 6; i++) res[2 + i] = 0;
#endif
}

/* USB serial "dump": the V9968 state the renderer reads, for replaying
 * one picture on the PC (host/v9968_dump_replay). The command engine is held while
 * VRAM goes out; all-zero 64-byte blocks are left out. */
void main_dump(void) {
    if (chip != VDP_CHIP_V9968) { printf("dump: V9968 only\n"); return; }
    extern volatile bool bus_cmd_hold;
    bus_cmd_hold = true;
    busy_wait_us(500);
    printf("DUMP begin\nregs");
    for (int i = 0; i < 64; i++) printf(" %02x", v9968.regs[i]);
    printf("\npal");
    for (int i = 0; i < 256; i++) printf(" %04x", v9968.palette[i]);
    printf("\nmisc %u %u %u %u %u\n", (unsigned)v9968.field, (unsigned)v9968.blink_page,
           (unsigned)v9968.shuffle_start, (unsigned)v9968.gamma, (unsigned)v9968.sprite_status);
    for (uint32_t a = 0; a < V9968_VRAM_SIZE; a += 64) {
        const uint8_t *p = v9968_vram + a;
        bool zero = true;
        for (int i = 0; i < 64; i++) if (p[i]) { zero = false; break; }
        if (zero) continue;
        printf("V %05lx ", (unsigned long)a);
        for (int i = 0; i < 64; i++) printf("%02x", p[i]);
        printf("\n");
    }
    printf("DUMP end\n");
    bus_cmd_hold = false;
}

/* LRMM speed: a 128x128 copy of the top-left corner onto itself (VX = 1,
 * VY = 0, so VRAM keeps its contents) on a copy of the VDP state, in G4
 * (r0 = 06h) or FG4 in G7 (r0 = 0Eh, ARG bit 7), with the command engine
 * held; FG4 with sprite mode 3 like LUMEN/FORGE (no G7 interleave). Results
 * in ns per pixel; the cartridge takes ~140 (12 clocks). */
#define BENCH_LRMM_PX (128u * 128u)

static void bench_lrmm_start(uint8_t r0, uint8_t arg) {
    v9968_t *b = &bench_v9968;
    b->regs[0] = r0;
    b->regs[20] = (uint8_t)((b->regs[20] & ~0x08) | 0x01 |   /* HS */
                            ((arg & 0x80) ? 0x08 : 0));         /* SP3 with FG4 */
    b->regs[21] = 0;                                           /* V9968 mode */
    b->geo.running = false;
    static const uint8_t r[][2] = {
        {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, 0}, {37, 0}, {38, 0}, {39, 0},
        {40, 128}, {41, 0}, {42, 128}, {43, 0}, {47, 0}, {48, 1}, {49, 0}, {50, 0},
        {51, 0}, {52, 0}, {53, 0}, {54, 0}, {55, 0xFF}, {56, 1}, {57, 0xFF}, {58, 7},
    };
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) v9968_cmd_write_reg(b, r[i][0], r[i][1]);
    v9968_cmd_write_reg(b, 45, arg);
    v9968_cmd_write_reg(b, 46, 0x30);
}

/* chunk 0: one call; otherwise calls of `chunk` clocks until done, with the
 * bus event watch (as core1 runs it) or without. ns per pixel. */
static uint32_t __not_in_flash_func(bench_lrmm)(uint8_t r0, uint8_t arg, unsigned chunk, bool ref,
                                                bool watch) {
    const volatile uint32_t *w = v9968_cmd_watch;
    v9968_cmd_watch = watch ? w : NULL;
    bench_lrmm_start(r0, arg);
    v9968_cmd_reference = ref;
    uint32_t t0 = timer_hw->timerawl;
    if (chunk == 0) v9968_cmd_step(&bench_v9968, 1u << 30);
    else while (v9968_cmd_busy(&bench_v9968)) v9968_service(&bench_v9968, chunk);
    uint32_t t = timer_hw->timerawl - t0;
    v9968_cmd_reference = false;
    v9968_cmd_watch = w;
    return t * 1000u / BENCH_LRMM_PX;
}

static void main_bench_v9968(void) {
    uint32_t full[8], nospr[8];
    enable_cycle_counter();
    uint32_t irq = save_and_disable_interrupts();
    bench_v9968 = v9968;
    bench_v9968_frame(full);
    uint32_t cnt[8];
    for (int i = 0; i < 8; i++) cnt[i] = bench_cnt[i];
    bench_v9968.regs[8] |= 0x02;                          /* SPD */
    bench_v9968_frame(nospr);
    restore_interrupts(irq);

    extern volatile bool bus_cmd_hold;
    bus_cmd_hold = true;
    busy_wait_us(500);
    irq = save_and_disable_interrupts();
    bench_v9968 = v9968;
    uint32_t g4 = bench_lrmm(0x06, 0x00, 0, false, false);
    uint32_t g4c = bench_lrmm(0x06, 0x00, 32, false, false);
    uint32_t g4w = bench_lrmm(0x06, 0x00, 512, false, true);
    uint32_t g4r = bench_lrmm(0x06, 0x00, 0, true, false);
    uint32_t g4rc = bench_lrmm(0x06, 0x00, 32, true, false);
    uint32_t f4 = bench_lrmm(0x0E, 0x80, 0, false, false);
    uint32_t f4w = bench_lrmm(0x0E, 0x80, 512, false, true);
    restore_interrupts(irq);
    bus_cmd_hold = false;
    printf("bench lrmm (ns/pixel; one call / 32-clock calls / 512-clock calls watching the bus): "
           "G4 %lu / %lu / %lu (general code %lu / %lu), FG4 %lu / - / %lu\n",
           (unsigned long)g4, (unsigned long)g4c, (unsigned long)g4w, (unsigned long)g4r,
           (unsigned long)g4rc, (unsigned long)f4, (unsigned long)f4w);
    printf("bench v9968 (IRQs off, per line x0.1us): full avg %lu max %lu us "
           "[bg %lu col %lu spr %lu (select %lu draw %lu) out %lu]; no sprites avg %lu max %lu us "
           "[bg %lu col %lu spr %lu out %lu]\n",
           (unsigned long)full[0], (unsigned long)full[1], (unsigned long)full[2],
           (unsigned long)full[3], (unsigned long)full[4], (unsigned long)full[6],
           (unsigned long)full[7], (unsigned long)full[5],
           (unsigned long)nospr[0], (unsigned long)nospr[1], (unsigned long)nospr[2],
           (unsigned long)nospr[3], (unsigned long)nospr[4], (unsigned long)nospr[5]);
    printf("bench v9968 frame: %lu sprites selected, %lu mode 3 drawn, %lu sprite dots, %lu translucent, "
           "%lu mix cache misses; cycles: draw start %lu, mode 3 runs %lu, composing %lu\n",
           (unsigned long)cnt[4], (unsigned long)cnt[0], (unsigned long)cnt[1], (unsigned long)cnt[2],
           (unsigned long)cnt[3], (unsigned long)cnt[5], (unsigned long)cnt[6], (unsigned long)cnt[7]);
}

void main_bench(void) {
    if (chip == VDP_CHIP_V9968) { main_bench_v9968(); return; }
    if (chip != VDP_CHIP_V9990) { printf("bench: V9990/V9968 only\n"); return; }
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

/* V9968: render line n from the last output line of n - 1 - V9968_LINES_AHEAD
 * on. The just-in-time start (last output line of n - 1, after the line
 * interrupt of n - 1) leaves 31.7 us, which pictures full of mode 3 sprites
 * overrun (LUMEN FORGE: up to ~90 us a line at 384MHz, in runs); starting
 * two VDP lines early absorbs most of it, and register changes made from a
 * line interrupt show that many lines later than on the cartridge. */
#define V9968_LINES_AHEAD 2           /* at most DVI_LINE_BUFFERS - 2 */

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
        /* not before the frame starts (vblank end, at output line -scale) */
        int32_t start = ((int32_t)next - V9968_LINES_AHEAD) * scale - 1;
        if (start < -scale) start = -scale;
        if (next < height && pos >= start && pos < (int32_t)height * scale) {
            /* late: line `next` is already on screen when its rendering starts */
            if (pos >= (int32_t)next * scale) dvi_lines.render_late++;
            uint32_t t0 = timer_hw->timerawl;
            v9968_render_line(&v9968, next,
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
