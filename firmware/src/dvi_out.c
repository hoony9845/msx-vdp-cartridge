/*
 * 640x480@60 DVI output over HSTX with scanline buffers.
 * Based on pico-examples/hstx/dvi_out_hstx_encoder, changed to RGB565 and
 * to feed V9990 lines (line doubled when the picture is <= 240 lines tall).
 *
 * The DMA IRQ also drives V9990 timing: vblank start/end and line events.
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
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

static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS), SYNC_V0_H1,
    HSTX_CMD_NOP
};

static uint32_t vactive_line[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1,
    HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS
};

dvi_lines_t dvi_lines;

static v9990_t *vdp;
static uint dma_ping, dma_pong;
static bool dma_pong_next;
static uint v_scanline = 2;
static bool vactive_cmdlist_posted;

/* per-frame picture geometry */
static uint32_t pic_top, pic_scale, pic_height;
static bool pic_active;

static void __not_in_flash_func(latch_frame_geometry)(void) {
    uint32_t h = v9990_display_height(vdp);
    if (h > DVI_HEIGHT) h = DVI_HEIGHT;
    pic_scale = (h <= DVI_HEIGHT / 2) ? 2 : 1;
    pic_height = h;
    pic_top = (DVI_HEIGHT - h * pic_scale) / 2;
    dvi_lines.src_height = h;
}

static void __not_in_flash_func(end_picture)(void) {
    if (!pic_active) return;
    pic_active = false;
    dvi_lines.src_line = -1;
    dvi_lines.frame++;
    v9990_video_vblank_start(vdp);
}

/* Returns the pixel data for output line y (0..479) and fires V9990 events. */
static const uint16_t *__not_in_flash_func(active_line)(uint32_t y) {
    if (y == 0) latch_frame_geometry();

    uint32_t end = pic_top + pic_height * pic_scale;
    if (y < pic_top || y >= end) {
        if (y == end) end_picture();
        return dvi_lines.border;
    }

    uint32_t rel = y - pic_top;
    uint32_t src = rel / pic_scale;
    if (rel % pic_scale == 0) {
        if (src == 0) {
            pic_active = true;
            v9990_video_vblank_end(vdp);
        }
        dvi_lines.src_line = (int32_t)src;
        v9990_video_line_start(vdp, src);
    }
    return dvi_lines.line[src & (DVI_LINE_BUFFERS - 1)];
}

static void __not_in_flash_func(dma_irq_handler)(void) {
    uint ch_num = dma_pong_next ? dma_pong : dma_ping;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1u << ch_num;
    dma_pong_next = !dma_pong_next;

    if (v_scanline >= MODE_V_FRONT_PORCH &&
        v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
    } else if (v_scanline < MODE_V_BLANK_LINES) {
        if (v_scanline == 0) end_picture();
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else if (!vactive_cmdlist_posted) {
        ch->read_addr = (uintptr_t)vactive_line;
        ch->transfer_count = count_of(vactive_line);
        vactive_cmdlist_posted = true;
    } else {
        ch->read_addr = (uintptr_t)active_line(v_scanline - MODE_V_BLANK_LINES);
        ch->transfer_count = MODE_H_ACTIVE_PIXELS / 2;   /* 2 px per word */
        vactive_cmdlist_posted = false;
    }

    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

static void assign_pair(uint pin, uint32_t sel) {
    uint bit = pin - 12;
    hstx_ctrl_hw->bit[bit]     = sel;
    hstx_ctrl_hw->bit[bit + 1] = sel | HSTX_CTRL_BIT0_INV_BITS;
}

void dvi_out_start(v9990_t *v) {
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

    dma_ping = (uint)dma_claim_unused_channel(true);
    dma_pong = (uint)dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(dma_ping);
    channel_config_set_chain_to(&c, dma_pong);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(dma_ping, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);
    c = dma_channel_get_default_config(dma_pong);
    channel_config_set_chain_to(&c, dma_ping);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(dma_pong, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1u << dma_ping) | (1u << dma_pong);
    dma_hw->inte0 = (1u << dma_ping) | (1u << dma_pong);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                            BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    dma_channel_start(dma_ping);
}
