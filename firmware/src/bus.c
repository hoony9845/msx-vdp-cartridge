/*
 * MSX bus side of the emulator (runs on core1).
 *
 *  writes : PIO monitor -> DMA -> ring buffer -> core1 -> <chip>_write_port()
 *  reads  : PIO read SM -> DMA (address) -> DMA (lookup) -> PIO drives D0-D7
 *           core1 keeps read_table[] up to date with <chip>_peek_port() and
 *           applies the read side effects afterwards via the monitor events.
 *
 * Port map (low nibble of the decoded port):
 *   V9990 : 0-7 = P#0-P#7, 8-F read FFh
 *   V9968 : 8-C = V9968 ports 0-4, D/F = geo3d index/data, 0-7 and E read FFh
 *           (the V9968 cartridge block: 88h-8Fh with SW1 at V9968, or
 *           68h-6Fh on a rev 0.1 PCB whose decoder is fixed at 6xh)
 *
 * Known limitation: two back-to-back reads of the same port faster than
 * core1 can refresh read_table[] return a stale value. If this shows up
 * (R800 INIR on P#0), assert /WAIT from the read SM until core1 is done.
 */
#include <assert.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "board_pins.h"
#include "bus.h"
#include "bus.pio.h"
#include "dvi_out.h"

static_assert(PIN_CS == v9990_bus_monitor_PIN_CS, "bus.pio PIN_CS mismatch");
static_assert(PIN_CS == v9990_bus_read_PIN_CS, "bus.pio PIN_CS mismatch");
static_assert(PIN_D0 == PIN_A0 + 4, "data bus must follow A0-A3");

#define RING_BITS   12                          /* 4KB = 1024 events */
#define RING_WORDS  ((1u << RING_BITS) / 4u)

#define V9968_BLOCK 8u                          /* V9968 ports at nibble 8-F */
#define V9968_CLK_PER_US 86u                    /* command engine: 85.9MHz */
#define V9968_SLICE 512u                        /* max clocks per loop pass */
#define V9990_CMD_SLICE 4u                      /* command engine units per loop pass */

static uint32_t ring[RING_WORDS] __attribute__((aligned(1u << RING_BITS)));
static volatile uint32_t read_table[16] __attribute__((aligned(64)));

/* Diagnostics, written by core1 and read by bus_print_stats() on core0. */
static volatile struct {
    uint32_t loops;            /* core1 heartbeat */
    uint32_t events;
    uint32_t max_backlog;      /* ring words waiting at once (overrun at RING_WORDS) */
    uint32_t max_loop_us;      /* longest core1 loop pass */
    uint32_t writes[16], reads[16];
    uint32_t last_ev[8];       /* last events, newest at last_pos - 1 */
    uint32_t last_pos;
    uint32_t resets;
    uint32_t fault_pc, fault_lr, fault_core;
    uint32_t cmd_us, cmd_slices;   /* time in the command engine while busy */
    uint32_t read_timeouts;    /* ordered reads the read SM did not take */
} stats;

static vdp_chip_t chip;
static v9990_t *v9990;
static v9968_t *v9968;
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
    gpio_init(PIN_RESET); gpio_set_dir(PIN_RESET, GPIO_IN); gpio_pull_up(PIN_RESET);

    /* 74LVC07 open-drain buffers: GPIO high = released */
    gpio_init(PIN_INT);  gpio_put(PIN_INT, 1);  gpio_set_dir(PIN_INT, GPIO_OUT);
    /* /WAIT: driven by the read SM (ordered reads), released to start */
    pio_sm_set_pins_with_mask(bus_pio, sm_read, 1u << PIN_WAIT, 1u << PIN_WAIT);
    pio_sm_set_pindirs_with_mask(bus_pio, sm_read, 1u << PIN_WAIT, 1u << PIN_WAIT);
    pio_gpio_init(bus_pio, PIN_WAIT);
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
    sm_config_set_set_pins(&c, PIN_WAIT, 1);
    pio_sm_init(bus_pio, sm_read, offset, &c);
    pio_sm_set_pins_with_mask(bus_pio, sm_read, 1u << PIN_WAIT, 1u << PIN_WAIT);

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

static void __not_in_flash_func(refresh_v9990)(void) {
    for (uint port = 0; port < 8; port++)
        read_table[port] = v9990_peek_port(v9990, (uint8_t)port);
}

static void __not_in_flash_func(refresh_v9968)(void) {
    for (uint port = 0; port < 8; port++)
        read_table[V9968_BLOCK + port] = v9968_peek_port(v9968, (uint8_t)port);
}

void bus_init(vdp_chip_t c, void *vdp) {
    chip = c;
    if (chip == VDP_CHIP_V9968) v9968 = vdp;
    else                        v9990 = vdp;
    bus_pio = pio0;
    sm_monitor = (uint)pio_claim_unused_sm(bus_pio, true);
    sm_read = (uint)pio_claim_unused_sm(bus_pio, true);
    dma_ring = dma_claim_unused_channel(true);
    dma_read_addr = dma_claim_unused_channel(true);
    dma_read_data = dma_claim_unused_channel(true);

    for (uint port = 0; port < 16; port++) read_table[port] = 0xFF;
    if (chip == VDP_CHIP_V9968) refresh_v9968();
    else                        refresh_v9990();
    v9968_cmd_watch = &dma_hw->ch[dma_ring].write_addr;

    init_gpio();
    init_monitor_sm();
    init_read_sm();
    pio_set_sm_mask_enabled(bus_pio, (1u << sm_monitor) | (1u << sm_read), true);
}

void __not_in_flash_func(bus_refresh_status)(void) {
    if (chip == VDP_CHIP_V9990 && v9990)
        read_table[V9990_PORT_STATUS] = v9990_peek_port(v9990, V9990_PORT_STATUS);
}

static inline uint32_t __not_in_flash_func(ring_head)(void) {
    return ((dma_hw->ch[dma_ring].write_addr - (uintptr_t)ring) / 4u) & (RING_WORDS - 1u);
}

/* Ordered reads (bus.pio): the read SM holds the CPU on /WAIT at this read
 * until here, where every earlier access has been applied and the table
 * refreshed. Let it answer, and return once it took the value, so that the
 * read's own side effects (VRAM pointer, flags) are applied after. */
#define READ_IRQ_GO   4u
#define READ_IRQ_DONE 5u

static inline void __not_in_flash_func(ordered_read)(void) {
    bus_pio->irq_force = 1u << READ_IRQ_GO;
    uint32_t t0 = timer_hw->timerawl;
    while (!(bus_pio->irq & (1u << READ_IRQ_DONE))) {
        if (timer_hw->timerawl - t0 > 20u) {
            /* the read SM did not see this read (took it for a write) */
            bus_pio->irq = 1u << READ_IRQ_GO;
            stats.read_timeouts++;
            return;
        }
    }
    bus_pio->irq = 1u << READ_IRQ_DONE;
}

static inline void __not_in_flash_func(stat_event)(uint32_t ev) {
    uint8_t port = (ev >> 1) & 0x0F;
    stats.events++;
    if (ev & 1u) stats.reads[port]++;
    else         stats.writes[port]++;
    stats.last_ev[stats.last_pos & 7u] = ev;
    stats.last_pos++;
}

static inline void __not_in_flash_func(stat_loop)(uint32_t head, uint32_t tail,
                                                  uint32_t *t0) {
    uint32_t backlog = (head - tail) & (RING_WORDS - 1u);
    if (backlog > stats.max_backlog) stats.max_backlog = backlog;
    uint32_t now = timer_hw->timerawl;
    if (now - *t0 > stats.max_loop_us) stats.max_loop_us = now - *t0;
    *t0 = now;
    stats.loops++;
}

static void __not_in_flash_func(loop_v9990)(void) {
    uint32_t tail = 0;
    bool in_reset = false;
    uint32_t t0 = timer_hw->timerawl;

    for (;;) {
        uint32_t head = ring_head();
        stat_loop(head, tail, &t0);
        while (tail != head) {
            uint32_t ev = ring[tail];
            tail = (tail + 1u) & (RING_WORDS - 1u);
            stat_event(ev);
            uint8_t port = (ev >> 1) & 0x0F;
            if (ev & 1u) ordered_read();
            if (port < 8) {
                if (ev & 1u)
                    (void)v9990_read_port(v9990, port);
                else
                    v9990_write_port(v9990, port, (uint8_t)(ev >> 5));
                refresh_v9990();
                /* An interrupt handler returns a few us after it clears
                 * P#6: release /INT now, or the Z80 takes it again. */
                gpio_put(PIN_INT, !v9990_irq_pending(v9990));
            }
        }

        /* small slices: bus events wait for the current slice to end */
        if (v9990->cmd.op) {
            uint32_t c0 = timer_hw->timerawl;
            if (v9990_cmd_step(v9990, V9990_CMD_SLICE))
                refresh_v9990();
            stats.cmd_us += timer_hw->timerawl - c0;
            stats.cmd_slices++;
        }
        read_table[V9990_PORT_STATUS] = v9990_peek_port(v9990, V9990_PORT_STATUS);
        read_table[V9990_PORT_INT_FLAG] = v9990_peek_port(v9990, V9990_PORT_INT_FLAG);

        gpio_put(PIN_INT, !v9990_irq_pending(v9990));

        bool reset = !gpio_get(PIN_RESET);
        if (reset && !in_reset) {
            stats.resets++;
            v9990_reset(v9990);
            refresh_v9990();
        }
        in_reset = reset;
    }
}

/* "dump": hold the V9968 command engine (bus events still served) */
volatile bool bus_cmd_hold;

static void __not_in_flash_func(loop_v9968)(void) {
    uint32_t tail = 0;
    bool in_reset = false;
    uint32_t last_us = timer_hw->timerawl;
    int32_t credit = 0;                 /* command engine clocks owed */
    uint32_t t0 = last_us;

    for (;;) {
        uint32_t head = ring_head();
        stat_loop(head, tail, &t0);
        while (tail != head) {
            uint32_t ev = ring[tail];
            tail = (tail + 1u) & (RING_WORDS - 1u);
            stat_event(ev);
            uint8_t port = (ev >> 1) & 0x0F;
            /* the value the read SM sends: read_table is written on this core only */
            uint8_t seen = (uint8_t)read_table[port];
            if (ev & 1u) ordered_read();
            if (port >= V9968_BLOCK) {
                if (ev & 1u)
                    (void)v9968_read_port_seen(v9968, port - V9968_BLOCK, seen);
                else {
                    /* A geo3d RUN transforms and classifies the whole model
                     * before it reports busy, and the CPU reads the status
                     * ~3 us after RUN: show busy first (refreshed below), or
                     * it takes geo3d as done and loads the next model over
                     * the one being drawn (GEO3D BASICS' dice). */
                    if (port - V9968_BLOCK == V9968_PORT_GEO_DATA)
                        read_table[V9968_BLOCK + V9968_PORT_GEO_INDEX] |= 0x05;
                    v9968_write_port(v9968, port - V9968_BLOCK, (uint8_t)(ev >> 5));
                }
                refresh_v9968();
            }
        }

        /* the command engine runs at the cartridge's speed, not faster */
        uint32_t now = timer_hw->timerawl;
        credit += (int32_t)((now - last_us) * V9968_CLK_PER_US);
        last_us = now;
        if (credit > (int32_t)V9968_SLICE) credit = V9968_SLICE;
        if (credit > 0 && !bus_cmd_hold && (v9968_cmd_busy(v9968) || v9968->geo.running)) {
            /* Back to the bus as soon as an event waits: the engine watches
             * the ring (v9968_cmd_watch) and stops within ~32 clocks
             * once one came in, so a read held on /WAIT is not kept
             * for a whole slice (SUPER CAT reads the status ~6 us after
             * "R#15 = 2"). The read table is refreshed once at the end: a
             * read waits on /WAIT until the ring is served, after this. */
            do {
                unsigned b = (unsigned)credit;
                unsigned used = v9968_service(v9968, b);
                credit -= (int32_t)used;
                if (used < b) break;              /* done, or waiting for the CPU */
            } while (credit > 0 && ring_head() == tail);
            refresh_v9968();
        } else {
            read_table[V9968_BLOCK + V9968_PORT_CTRL] = v9968_peek_port(v9968, V9968_PORT_CTRL);
            read_table[V9968_BLOCK + V9968_PORT_INT] = v9968_peek_port(v9968, V9968_PORT_INT);
        }
        if (!v9968_cmd_busy(v9968) && !v9968->geo.running && credit > 0) credit = 0;

        gpio_put(PIN_INT, !v9968_irq_pending(v9968));

        bool reset = !gpio_get(PIN_RESET);
        if (reset && !in_reset) {
            stats.resets++;
            v9968_reset(v9968);
            refresh_v9968();
        }
        in_reset = reset;
    }
}

void __not_in_flash_func(bus_core1_main)(void) {
    if (chip == VDP_CHIP_V9968) loop_v9968();
    else                        loop_v9990();
}

/* Hard fault: record where, then stop this core (the other one keeps
 * serving USB so "stat" can report it). */
void __attribute__((used)) bus_fault_record(uint32_t *frame) {
    stats.fault_pc = frame[6];
    stats.fault_lr = frame[5];
    stats.fault_core = get_core_num() + 1u;
    gpio_put(PIN_INT, 1);
    gpio_init(PIN_WAIT);                    /* from the read SM back to SIO */
    gpio_put(PIN_WAIT, 1);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    for (;;) __asm volatile("wfi");
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b bus_fault_record\n");
}

void bus_print_stats(bool reset) {
    static uint32_t prev_loops, prev_us;
    uint32_t now = timer_hw->timerawl;
    uint32_t loops = stats.loops;
    printf("core1: %lu loops (%lu/ms since last stat), max pass %lu us, "
           "max backlog %lu/%u, MSX resets %lu, ordered-read timeouts %lu\n",
           (unsigned long)loops,
           (unsigned long)((loops - prev_loops) / ((now - prev_us) / 1000u + 1u)),
           (unsigned long)stats.max_loop_us, (unsigned long)stats.max_backlog,
           (unsigned)RING_WORDS, (unsigned long)stats.resets,
           (unsigned long)stats.read_timeouts);
    prev_loops = loops;
    prev_us = now;
    if (stats.fault_core)
        printf("HARD FAULT on core%lu: pc %08lx lr %08lx\n",
               (unsigned long)stats.fault_core - 1u,
               (unsigned long)stats.fault_pc, (unsigned long)stats.fault_lr);
    printf("events %lu; port w/r:", (unsigned long)stats.events);
    for (uint p = 0; p < 16; p++)
        if (stats.writes[p] || stats.reads[p])
            printf(" %X:%lu/%lu", p, (unsigned long)stats.writes[p],
                   (unsigned long)stats.reads[p]);
    printf("\nlast events (old->new):");
    uint32_t pos = stats.last_pos;
    for (uint i = 0; i < 8; i++) {
        uint32_t ev = stats.last_ev[(pos + i) & 7u];
        printf(" %c%X=%02X", (ev & 1u) ? 'R' : 'W', (unsigned)((ev >> 1) & 0x0F),
               (unsigned)((ev >> 5) & 0xFF));
    }
    printf("\n/INT pin %s, /WAIT pin %s, /RESET %s\n",
           gpio_get_out_level(PIN_INT) ? "released" : "ASSERTED",
           gpio_get_out_level(PIN_WAIT) ? "released" : "ASSERTED",
           gpio_get(PIN_RESET) ? "high" : "LOW");
    if (chip == VDP_CHIP_V9990 && v9990) {
        const v9990_t *v = v9990;
        printf("V9990: status %02X int_flags %02X int_enable(R9) %02X irq %d, "
               "cmd op %X remaining %u x %u, P#7 %02X\n",
               v->status, v->int_flags, v->regs[V9990_R_INT_ENABLE],
               v9990_irq_pending(v), v->cmd.op, v->cmd.remaining_x, v->cmd.remaining_y,
               v->sys_ctrl);
        printf("regs:");
        for (uint r = 0; r < 64; r++) printf("%s%02X", (r & 15) ? " " : "\n ", v->regs[r]);
        printf("\nread_table:");
        for (uint p = 0; p < 8; p++) printf(" %02lX", (unsigned long)read_table[p] & 0xFF);
        printf("\n");
    }
    static uint32_t prev_frame, prev_frame_us;
    uint32_t fr = dvi_lines.frame;
    printf("frames %lu in %lu ms; command engine busy %lu us (%lu slices of %u)\n",
           (unsigned long)(fr - prev_frame), (unsigned long)((now - prev_frame_us) / 1000u),
           (unsigned long)stats.cmd_us, (unsigned long)stats.cmd_slices,
           (unsigned)V9990_CMD_SLICE);
    printf("dvi: late IRQs %lu, output restarts %lu\n",
           (unsigned long)dvi_lines.dma_late, (unsigned long)dvi_lines.dma_restarts);
    dvi_print_debug();
    prev_frame = fr;
    prev_frame_us = now;
    uint32_t n = dvi_lines.render_lines;
    printf("render: %lu lines, avg %lu us, max %lu us, late %lu; picture %lu lines x%lu "
           "(budget %lu us per line)\n",
           (unsigned long)n, (unsigned long)(n ? dvi_lines.render_us / n : 0),
           (unsigned long)dvi_lines.render_max_us, (unsigned long)dvi_lines.render_late,
           (unsigned long)dvi_lines.src_height, (unsigned long)dvi_lines.scale,
           (unsigned long)(dvi_lines.scale * 3178u / 100u));
#ifdef V9968_PROF
    if (chip == VDP_CHIP_V9968) {
        extern volatile uint32_t v9968_prof_us[6];
        printf("v9968 render stages (us total): background %lu, colours %lu, sprites %lu (select %lu, draw %lu), output %lu\n",
               (unsigned long)v9968_prof_us[0], (unsigned long)v9968_prof_us[1],
               (unsigned long)v9968_prof_us[2], (unsigned long)v9968_prof_us[4],
               (unsigned long)v9968_prof_us[5], (unsigned long)v9968_prof_us[3]);
        if (reset) for (int i = 0; i < 6; i++) v9968_prof_us[i] = 0;
    }
#endif
    if (reset) {
        stats.cmd_us = 0;
        stats.cmd_slices = 0;
        stats.read_timeouts = 0;
        dvi_lines.render_lines = 0;
        dvi_lines.render_us = 0;
        dvi_lines.render_max_us = 0;
        dvi_lines.render_late = 0;
        stats.max_loop_us = 0;
        stats.max_backlog = 0;
    }
}

/* USB serial "vram": CRC32 of each 64KB VRAM block, to compare with a
 * replay of the same software on the PC (zero block = d7978eeb). */
static uint32_t crc32_block(const volatile uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

void bus_print_vram_crc(void) {
    if (chip != VDP_CHIP_V9990 || !v9990) { printf("vram: V9990 only\n"); return; }
    printf("vram crc:");
    for (uint i = 0; i < 8; i++)
        printf(" %08lx", (unsigned long)crc32_block(
                   (i < 4 ? v9990->vram : v9990->vram1) + (i & 3u) * 65536u, 65536u));
    printf("\n");
}

/* USB serial "out <port> <value>" (hex): a V9990 port write from core0,
 * for testing without an MSX (not synchronised with core1). */
void bus_debug_out(unsigned port, unsigned value) {
    if (chip == VDP_CHIP_V9990 && v9990) {
        v9990_write_port(v9990, (uint8_t)(port & 15u), (uint8_t)value);
        printf("P#%X <- %02X\n", port & 15u, value & 0xFFu);
    }
}
