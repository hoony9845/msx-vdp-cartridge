/*
 * 640x480@60 DVI output over HSTX with scanline buffers.
 * Based on pico-examples/hstx/dvi_out_hstx_encoder, changed to RGB565 and
 * to feed VDP lines (line doubled when the picture is <= 240 lines tall).
 *
 * Every line is one DMA block of the same length (sync commands, then
 * pixels or padding); a control channel starts each block from a ring of
 * line addresses that the IRQ keeps 16 lines ahead, so a late IRQ cannot
 * desynchronise the HSTX command stream.
 *
 * The DMA IRQ also drives the VDP's timing: vblank start/end and line
 * events, through the callbacks of dvi_vdp_t.
 */
#include <assert.h>
#include <string.h>
#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "board_pins.h"
#include "dvi_out.h"

/* ---- 640x480@60 timing (25.2MHz pixel clock) ---- */
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS DVI_WIDTH

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  DVI_HEIGHT
#define MODE_V_BLANK_LINES   (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH)
#define MODE_V_TOTAL_LINES   (MODE_V_BLANK_LINES + MODE_V_ACTIVE_LINES)

/* ---- TMDS control symbols ---- */
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V1_H1,
    HSTX_CMD_NOP
};


static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

dvi_lines_t dvi_lines;

static const dvi_vdp_t *vdp;

/* Every output line is one DMA block of LINE_WORDS words, so the data
 * channel's count never changes and the control channel only writes the
 * next block's address (READ_ADDR_TRIG). Blank lines are padded with RAW
 * data words, keeping their 800 clocks. */
#define LINE_WORDS (DVI_LINE_CMD_WORDS + MODE_H_ACTIVE_PIXELS / 2)
#define BLANK_RAW  (LINE_WORDS - 7)     /* explicit words in a blank line */
static uint32_t blank_line[LINE_WORDS], vsync_line[LINE_WORDS];

/* Address ring run by the control channel; the IRQ refills the used slots
 * CB_N lines ahead, so it may be several lines late. */
#define CB_N 16
static uint32_t cb[CB_N] __attribute__((aligned(CB_N * 4)));
static uint16_t cb_scan[CB_N];          /* scanline of each slot */
static uint dma_data, dma_ctrl;
static uint out_scan;                   /* scanline being sent */
static uint done_slot;                  /* last slot accounted for */
static uint32_t hstx_csr;               /* HSTX configuration, for restarts */
static uint32_t last_frame_us;

/* per-frame picture geometry */
static uint32_t pic_top, pic_scale, pic_height;
static bool pic_active;

static void __not_in_flash_func(latch_frame_geometry)(void) {
    uint32_t h = vdp->height(vdp->vdp);
    if (h > DVI_HEIGHT) h = DVI_HEIGHT;
    pic_scale = (h <= DVI_HEIGHT / 2) ? 2 : 1;
    pic_height = h;
    pic_top = (DVI_HEIGHT - h * pic_scale) / 2;
    dvi_lines.src_height = h;
    dvi_lines.scale = pic_scale;
}

static void __not_in_flash_func(end_picture)(void) {
    if (!pic_active) return;
    pic_active = false;
    dvi_lines.src_line = -1;
    dvi_lines.frame++;
    vdp->vblank_start(vdp->vdp);
}

/* Just-in-time mode: events for line n + 1 as line n starts on screen. */
static void __not_in_flash_func(jit_events)(int32_t rel) {
    int32_t sub = rel & (int32_t)(pic_scale - 1);
    int32_t vline = (rel - sub) / (int32_t)pic_scale;
    if (vdp->hblank) vdp->hblank(vdp->vdp, pic_scale == 2 ? sub == 0 : (rel & 1) == 0);
    if (sub != 0) return;
    int32_t next = vline + 1;
    if (next == 0) {
        pic_active = true;
        vdp->vblank_end(vdp->vdp);
    }
    if (next >= 0 && next < (int32_t)pic_height) vdp->line(vdp->vdp, (unsigned)next);
}

/* ---- filling side: what to send for a scanline (CB_N lines ahead) ---- */

/* geometry used for the frame being filled; adopted by the event side when
 * that frame starts on screen */
static uint32_t fill_top, fill_scale, fill_height;

static void __not_in_flash_func(fill_block)(uint slot, uint scan) {
    const void *addr;
    if (scan >= MODE_V_FRONT_PORCH && scan < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        addr = vsync_line;
    } else if (scan < MODE_V_BLANK_LINES) {
        addr = blank_line;
    } else {
        uint32_t y = scan - MODE_V_BLANK_LINES;
        if (y == 0) {
            uint32_t h = vdp->height(vdp->vdp);
            if (h > DVI_HEIGHT) h = DVI_HEIGHT;
            fill_scale = (h <= DVI_HEIGHT / 2) ? 2 : 1;
            fill_height = h;
            fill_top = (DVI_HEIGHT - h * fill_scale) / 2;
        }
        const dvi_line_t *l = &dvi_lines.border;
        if (y >= fill_top && y < fill_top + fill_height * fill_scale)
            l = &dvi_lines.line[((y - fill_top) / fill_scale) & (DVI_LINE_BUFFERS - 1)];
        addr = l;
    }
    cb[slot] = (uint32_t)(uintptr_t)addr;
    cb_scan[slot] = (uint16_t)scan;
}

/* ---- event side: VDP timing for the scanline now on screen ---- */

static void __not_in_flash_func(scan_events)(uint scan) {
    if (scan == 0) end_picture();
    /* output line relative to the picture top, negative in the vertical
     * blank too, so a render-ahead renderer starts a frame just before it
     * is shown (after the vblank interrupt handler set it up) */
    if (scan < MODE_V_BLANK_LINES)
        dvi_lines.pos = (int32_t)scan - MODE_V_BLANK_LINES - (int32_t)fill_top;
    /* Render-ahead VDPs: the new frame starts (line counters and scroll
     * latches reset) before the renderer may draw its first lines, which
     * is DVI_LINE_BUFFERS - 1 VDP lines ahead of the scan. */
    if (!vdp->just_in_time) {
        int32_t start = (int32_t)MODE_V_BLANK_LINES + (int32_t)fill_top -
                        (int32_t)(DVI_LINE_BUFFERS * fill_scale);
        if (start < 1) start = 1;
        if ((int32_t)scan == start) {
            pic_active = true;
            vdp->vblank_end(vdp->vdp);
        }
    }
    if (scan >= MODE_V_BLANK_LINES) {
        uint32_t y = scan - MODE_V_BLANK_LINES;
        if (y == 0) {
            pic_scale = fill_scale;
            pic_height = fill_height;
            pic_top = fill_top;
            dvi_lines.src_height = pic_height;
            dvi_lines.scale = pic_scale;
        }
        int32_t rel = (int32_t)y - (int32_t)pic_top;
        dvi_lines.pos = rel;
        if (vdp->just_in_time && rel >= -2 * (int32_t)pic_scale)
            jit_events(rel);
        uint32_t end = pic_top + pic_height * pic_scale;
        if (y == end) end_picture();
        if (y >= pic_top && y < end && (uint32_t)rel % pic_scale == 0) {
            uint32_t src = (uint32_t)rel / pic_scale;
            if (!vdp->just_in_time) vdp->line(vdp->vdp, src);
            dvi_lines.src_line = (int32_t)src;
        }
    }
    /* Render-ahead VDPs: horizontal retrace on every other output line
     * (one VDP line = two 31kHz lines), in the vertical blank too.
     * Just-in-time VDPs get it from jit_events(). */
    if (!vdp->just_in_time && vdp->hblank)
        vdp->hblank(vdp->vdp, (scan & 1u) == 0);
}

static void dvi_restart(void);

/* The data channel finished a block. Find the block now running from the
 * control channel's read pointer (several may have finished since the last
 * IRQ), fire the events of the lines passed and refill the used slots. */
static void __not_in_flash_func(dma_irq_handler)(void) {
    dma_hw->ints0 = 1u << dma_data;
    uint32_t next = (uint32_t)((dma_hw->ch[dma_ctrl].read_addr - (uintptr_t)cb) / 4u) % CB_N;
    uint cur = (next + CB_N - 1u) % CB_N;   /* slot being sent */
    uint passed = (cur + CB_N - done_slot) % CB_N;
    if (passed == 0) return;
    if (passed > 1) dvi_lines.dma_late++;   /* the IRQ was late */

    uint scan_now = cb_scan[cur];
    uint scan = out_scan;
    for (uint i = 0; i < passed && scan != scan_now; i++) {
        scan = (scan + 1u) % MODE_V_TOTAL_LINES;
        if (scan == 0) {
            /* a frame is 16.68ms; far off means HSTX lost its place in
             * the command stream: start the output over */
            uint32_t now = timer_hw->timerawl, dt = now - last_frame_us;
            last_frame_us = now;
            if (dt < 15000u || dt > 18500u) {
                if (dvi_lines.frame > 2) { dvi_restart(); return; }
            }
        }
        scan_events(scan);
    }
    out_scan = scan_now;

    /* refill every slot except the running one and the one loaded next:
     * slot cur + k carries scanline scan_now + k */
    for (uint k = 2; k < CB_N; k++) {
        uint slot = (cur + k) % CB_N;
        uint want = (scan_now + k) % MODE_V_TOTAL_LINES;
        if (cb_scan[slot] != want) fill_block(slot, want);
    }
    done_slot = cur;
}

/* (Re)start the output at scanline 0 with a fresh ring. */
static void __not_in_flash_func(dvi_begin)(void) {
    for (uint i = 0; i < CB_N; i++) fill_block(i, i);
    out_scan = 0;
    done_slot = 0;
    last_frame_us = timer_hw->timerawl;
    dma_hw->ch[dma_ctrl].read_addr = (uintptr_t)cb;
    dma_hw->ch[dma_ctrl].transfer_count = 1;
    dma_channel_start(dma_ctrl);
}

static void __not_in_flash_func(dvi_restart)(void) {
    dvi_lines.dma_restarts++;
    for (uint i = 0; i < CB_N; i++) cb[i] = 0;  /* null trigger: the chain stops */
    while (dma_channel_is_busy(dma_data) || dma_channel_is_busy(dma_ctrl)) tight_loop_contents();
    dma_hw->ints0 = 1u << dma_data;
    hstx_ctrl_hw->csr = 0;                      /* reset the command expander */
    for (volatile int i = 0; i < 100; i++) {}
    hstx_ctrl_hw->csr = hstx_csr;
    dvi_begin();
}

static void assign_pair(uint pin, uint32_t sel) {
    uint bit = pin - 12;
    hstx_ctrl_hw->bit[bit]     = sel;
    hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
}

void dvi_out_start(const dvi_vdp_t *v) {
    static_assert(count_of(vactive_line) == DVI_LINE_CMD_WORDS, "line command list size");
    for (uint i = 0; i < DVI_LINE_BUFFERS; i++)
        memcpy(dvi_lines.line[i].cmd, vactive_line, sizeof vactive_line);
    memcpy(dvi_lines.border.cmd, vactive_line, sizeof vactive_line);
    vdp = v;
    dvi_lines.src_line = -1;
    latch_frame_geometry();

    /* RGB565: red 15:11 -> lane 2, green 10:5 -> lane 1, blue 4:0 -> lane 0 */
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        2  << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1  << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0  << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    /* 10 bits per pixel, 2 bits per HSTX clock: pixel clock = clk_hstx / 5 */
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    assign_pair(PIN_DVI_CLK, HSTX_CTRL_BIT0_CLK_BITS);
    static const uint lane_pin[3] = { PIN_DVI_D0, PIN_DVI_D1, PIN_DVI_D2 };
    for (uint lane = 0; lane < 3; lane++) {
        assign_pair(lane_pin[lane],
                    (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
                    (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB);
    }
    for (uint pin = 12; pin <= 19; pin++)
        gpio_set_function(pin, GPIO_FUNC_HSTX);

    /* blank lines: front porch, sync, then BLANK_RAW explicit words and a
     * repeat for the rest: LINE_WORDS words, 800 clocks */
    for (uint v = 0; v < 2; v++) {
        uint32_t *l = v ? vsync_line : blank_line;
        uint32_t h1 = v ? SYNC_V0_H1 : SYNC_V1_H1, h0 = v ? SYNC_V0_H0 : SYNC_V1_H0;
        uint n = 0;
        l[n++] = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH; l[n++] = h1;
        l[n++] = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;  l[n++] = h0;
        l[n++] = HSTX_CMD_RAW | BLANK_RAW;
        for (uint i = 0; i < BLANK_RAW; i++) l[n++] = h1;
        l[n++] = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS - BLANK_RAW);
        l[n++] = h1;
        (void)n;
    }

    dma_data = (uint)dma_claim_unused_channel(true);
    dma_ctrl = (uint)dma_claim_unused_channel(true);

    /* data: one line to HSTX, then hand over to the control channel */
    dma_channel_config c = dma_channel_get_default_config(dma_data);
    channel_config_set_chain_to(&c, dma_ctrl);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(dma_data, &c, &hstx_fifo_hw->fifo, NULL, LINE_WORDS, false);

    /* control: next line address into READ_ADDR_TRIG, which starts it */
    c = dma_channel_get_default_config(dma_ctrl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, __builtin_ctz(CB_N * 4));  /* read ring: the table */
    dma_channel_configure(dma_ctrl, &c, &dma_hw->ch[dma_data].al3_read_addr_trig,
                          cb, 1, false);

    dma_hw->ints0 = 1u << dma_data;
    dma_hw->inte0 = 1u << dma_data;
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    hstx_csr = hstx_ctrl_hw->csr;
    dvi_begin();
}

/* USB serial "stat": the output state, to tell a clock change from
 * overwritten command lists when the line timing breaks. */
void dvi_print_debug(void) {
    printf("dvi: clk_sys %lu kHz, clk_hstx %lu kHz (measured), hstx csr %08lx\n",
           (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS),
           (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX),
           (unsigned long)hstx_ctrl_hw->csr);
    bool ok = memcmp(dvi_lines.border.cmd, vactive_line, sizeof vactive_line) == 0;
    for (uint i = 0; i < DVI_LINE_BUFFERS; i++)
        ok = ok && memcmp(dvi_lines.line[i].cmd, vactive_line, sizeof vactive_line) == 0;
    printf("dvi: line command lists %s; vsync_off %03lx %08lx %03lx %08lx %03lx %08lx\n",
           ok ? "intact" : "OVERWRITTEN",
           (unsigned long)vblank_line_vsync_off[0], (unsigned long)vblank_line_vsync_off[1],
           (unsigned long)vblank_line_vsync_off[2], (unsigned long)vblank_line_vsync_off[3],
           (unsigned long)vblank_line_vsync_off[4], (unsigned long)vblank_line_vsync_off[5]);
    for (uint k = 0; k < 2; k++) {
        uint c = k ? dma_ctrl : dma_data;
        printf("dvi: dma %u ctrl %08lx count %lu read %08lx\n", c,
               (unsigned long)dma_hw->ch[c].ctrl_trig, (unsigned long)dma_hw->ch[c].transfer_count,
               (unsigned long)dma_hw->ch[c].read_addr);
    }
    if (!ok) {
        printf("dvi: line0 cmd:");
        for (uint i = 0; i < DVI_LINE_CMD_WORDS; i++) printf(" %08lx", (unsigned long)dvi_lines.line[0].cmd[i]);
        printf("\n");
    }
}
