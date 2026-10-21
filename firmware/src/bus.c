/*
 * MSX bus side of the emulator (runs on core1).
 *
 *  writes : PIO monitor -> DMA -> ring buffer -> core1 -> v9990_write_port()
 *  reads  : PIO read SM -> DMA (address) -> DMA (lookup) -> PIO drives D0-D7
 *           core1 keeps read_table[] up to date with v9990_peek_port() and
 *           applies the read side effects afterwards via the monitor events.
 *
 * Known limitation: two back-to-back reads of the same port faster than
 * core1 can refresh read_table[] return a stale value. If this shows up
 * (R800 INIR on P#0), assert /WAIT from the read SM until core1 is done.
 */
#include <assert.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "board_pins.h"
#include "bus.h"
#include "bus.pio.h"

static_assert(PIN_CS == v9990_bus_monitor_PIN_CS, "bus.pio PIN_CS mismatch");
static_assert(PIN_CS == v9990_bus_read_PIN_CS, "bus.pio PIN_CS mismatch");
static_assert(PIN_D0 == PIN_A0 + 4, "data bus must follow A0-A3");

#define RING_BITS   12                          /* 4KB = 1024 events */
#define RING_WORDS  ((1u << RING_BITS) / 4u)

static uint32_t ring[RING_WORDS] __attribute__((aligned(1u << RING_BITS)));
static volatile uint32_t read_table[16] __attribute__((aligned(64)));

static v9990_t *vdp;
static PIO bus_pio;
static uint sm_monitor, sm_read;
static int dma_ring, dma_read_addr, dma_read_data;

static void init_gpio(void) {
    /* A0-A3, D0-D7: PIO controlled, inputs by default */
    for (uint pin = PIN_A0; pin < PIN_D0 + 8; pin++) {
        pio_gpio_init(bus_pio, pin);
        gpio_set_pulls(pin, false, false);
    }
    pio_sm_set_consecutive_pindirs(bus_pio, sm_read, PIN_A0, 12, false);

    gpio_init(PIN_CS);    gpio_set_dir(PIN_CS, GPIO_IN);    gpio_pull_up(PIN_CS);
    gpio_init(PIN_RD);    gpio_set_dir(PIN_RD, GPIO_IN);    gpio_pull_up(PIN_RD);
    gpio_init(PIN_WR);    gpio_set_dir(PIN_WR, GPIO_IN);    gpio_pull_up(PIN_WR);
    gpio_init(PIN_RESET); gpio_set_dir(PIN_RESET, GPIO_IN); gpio_pull_up(PIN_RESET);

    /* 74LVC07 open-drain buffers: GPIO high = released */
    gpio_init(PIN_INT);  gpio_put(PIN_INT, 1);  gpio_set_dir(PIN_INT, GPIO_OUT);
    gpio_init(PIN_WAIT); gpio_put(PIN_WAIT, 1); gpio_set_dir(PIN_WAIT, GPIO_OUT);
}

static void init_monitor_sm(void) {
    uint offset = pio_add_program(bus_pio, &v9990_bus_monitor_program);
    pio_sm_config c = v9990_bus_monitor_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_in_shift(&c, false, false, 32);   /* shift left, no autopush */
    sm_config_set_jmp_pin(&c, PIN_RD);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(bus_pio, sm_monitor, offset, &c);

    /* DMA: monitor RX FIFO -> ring buffer, forever */
    dma_channel_config dc = dma_channel_get_default_config(dma_ring);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, RING_BITS);
    channel_config_set_dreq(&dc, pio_get_dreq(bus_pio, sm_monitor, false));
    dma_channel_configure(dma_ring, &dc, ring, &bus_pio->rxf[sm_monitor],
                          (DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS
                           << DMA_CH0_TRANS_COUNT_MODE_LSB) | 1u,
                          true);
}

static void init_read_sm(void) {
    uint offset = pio_add_program(bus_pio, &v9990_bus_read_program);
    pio_sm_config c = v9990_bus_read_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_in_shift(&c, false, false, 32);   /* shift left */
    sm_config_set_out_shift(&c, true, false, 32);   /* shift right */
    sm_config_set_jmp_pin(&c, PIN_RD);
    pio_sm_init(bus_pio, sm_read, offset, &c);

    /* X = table address >> 6 */
    pio_sm_put_blocking(bus_pio, sm_read, (uint32_t)(uintptr_t)read_table >> 6);
    pio_sm_exec(bus_pio, sm_read, pio_encode_pull(false, true));
    pio_sm_exec(bus_pio, sm_read, pio_encode_mov(pio_x, pio_osr));

    /* DMA 1: RX FIFO (response address) -> read_addr trigger of DMA 2 */
    dma_channel_config ca = dma_channel_get_default_config(dma_read_addr);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_32);
    channel_config_set_read_increment(&ca, false);
    channel_config_set_write_increment(&ca, false);
    channel_config_set_dreq(&ca, pio_get_dreq(bus_pio, sm_read, false));
    channel_config_set_high_priority(&ca, true);

    /* DMA 2: one word from the table -> TX FIFO */
    dma_channel_config cd = dma_channel_get_default_config(dma_read_data);
    channel_config_set_transfer_data_size(&cd, DMA_SIZE_32);
    channel_config_set_read_increment(&cd, false);
    channel_config_set_write_increment(&cd, false);
    channel_config_set_high_priority(&cd, true);
    dma_channel_configure(dma_read_data, &cd, &bus_pio->txf[sm_read],
                          read_table, 1, false);

    dma_channel_configure(dma_read_addr, &ca,
                          &dma_hw->ch[dma_read_data].al3_read_addr_trig,
                          &bus_pio->rxf[sm_read],
                          (DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS
                           << DMA_CH0_TRANS_COUNT_MODE_LSB) | 1u,
                          true);
}

static void __not_in_flash_func(refresh_read_table)(void) {
    for (uint port = 0; port < 8; port++)
        read_table[port] = v9990_peek_port(vdp, (uint8_t)port);
}

void bus_init(v9990_t *v) {
    vdp = v;
    bus_pio = pio0;
    sm_monitor = (uint)pio_claim_unused_sm(bus_pio, true);
    sm_read = (uint)pio_claim_unused_sm(bus_pio, true);
    dma_ring = dma_claim_unused_channel(true);
    dma_read_addr = dma_claim_unused_channel(true);
    dma_read_data = dma_claim_unused_channel(true);

    for (uint port = 0; port < 16; port++) read_table[port] = 0xFF;
    refresh_read_table();

    init_gpio();
    init_monitor_sm();
    init_read_sm();
    pio_set_sm_mask_enabled(bus_pio, (1u << sm_monitor) | (1u << sm_read), true);
}

void __not_in_flash_func(bus_core1_main)(void) {
    uint32_t tail = 0;
    bool in_reset = false;

    for (;;) {
        uint32_t head = ((dma_hw->ch[dma_ring].write_addr - (uintptr_t)ring) / 4u)
                        & (RING_WORDS - 1u);
        while (tail != head) {
            uint32_t ev = ring[tail];
            tail = (tail + 1u) & (RING_WORDS - 1u);
            uint8_t port = (ev >> 1) & 0x0F;
            if (ev & 1u)
                (void)v9990_read_port(vdp, port);
            else
                v9990_write_port(vdp, port, (uint8_t)(ev >> 5));
            refresh_read_table();
        }

        if (v9990_cmd_step(vdp, 16))
            refresh_read_table();
        read_table[V9990_PORT_STATUS] = v9990_peek_port(vdp, V9990_PORT_STATUS);
        read_table[V9990_PORT_INT_FLAG] = v9990_peek_port(vdp, V9990_PORT_INT_FLAG);

        gpio_put(PIN_INT, !v9990_irq_pending(vdp));

        bool reset = !gpio_get(PIN_RESET);
        if (reset && !in_reset) {
            v9990_reset(vdp);
            refresh_read_table();
        }
        in_reset = reset;
    }
}
