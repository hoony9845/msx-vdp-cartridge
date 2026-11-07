/*
 * APS6404L (8MB QSPI PSRAM) on QMI chip-select 1.
 * Sequence follows the CircuitPython / MicroPython RP2350 PSRAM setup.
 * Call after the final system clock has been set: the QMI timing is
 * derived from clk_sys.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/xip_cache.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "board_pins.h"
#include "psram.h"

#define PSRAM_MAX_FREQ_HZ   126000000   /* clk_sys / 2 (datasheet max 133MHz); RX delay
                                         * 2-5 read correctly on a Core2350B */
#define PSRAM_TCEM_NS       8000        /* max CS low time */
#define PSRAM_TCPH_NS       18          /* min CS high time */

static void __not_in_flash_func(qmi_wait_idle)(void) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
}

uint8_t psram_id_kgd, psram_id_eid;          /* last read ID, for diagnostics */

/* Must not be inlined into flash code: XIP stalls while direct mode is on. */
static size_t __no_inline_not_in_flash_func(psram_detect_and_init)(void) {
    size_t size = 0;
    uint32_t irq = save_and_disable_interrupts();

    /* Exit QPI mode in case the PSRAM was already initialised */
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    qmi_wait_idle();
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xF5;
    qmi_wait_idle();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    /* Read ID (0x9F): byte 5 = KGD (0x5D), byte 6 = EID */
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0) ? 0x9F : 0xFF;
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {}
        qmi_wait_idle();
        uint8_t rx = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = rx;
        if (i == 6) eid = rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
    psram_id_kgd = kgd;
    psram_id_eid = eid;

    if (kgd == 0x5D) {
        size = 1024 * 1024;                 /* EID bits 7:5 encode the size */
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) size *= 8;
        else if (size_id == 0) size *= 2;
        else if (size_id == 1) size *= 4;

        /* Reset enable, reset, enter quad mode */
        qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
        qmi_wait_idle();
        static const uint8_t __not_in_flash("psram") cmds[3] = { 0x66, 0x99, 0x35 };
        for (int i = 0; i < 3; i++) {
            qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
            qmi_hw->direct_tx = cmds[i];
            qmi_wait_idle();
            qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
            for (int j = 0; j < 20; j++) __asm volatile("nop");
            (void)qmi_hw->direct_rx;
        }
        qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

        /* Timing derived from clk_sys */
        const uint32_t sys_hz = clock_get_hz(clk_sys);
        uint32_t divisor = (sys_hz + PSRAM_MAX_FREQ_HZ - 1) / PSRAM_MAX_FREQ_HZ;
        if (divisor == 1 && sys_hz > 100000000) divisor = 2;
        uint32_t rxdelay = divisor;
        if (sys_hz / divisor > 100000000) rxdelay += 1;
        const uint64_t period_fs = 1000000000000000ull / sys_hz;
        const uint32_t max_select =
            (uint32_t)(((uint64_t)PSRAM_TCEM_NS * 1000000ull / 64u) / period_fs);
        const uint32_t min_deselect =
            (uint32_t)(((uint64_t)PSRAM_TCPH_NS * 1000000ull + period_fs - 1) / period_fs)
            - (divisor + 1) / 2;

        qmi_hw->m[1].timing =
            1u << QMI_M1_TIMING_COOLDOWN_LSB |
            QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
            max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
            min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
            rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
            divisor << QMI_M1_TIMING_CLKDIV_LSB;
        qmi_hw->m[1].rfmt =
            QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
            QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
            QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
            QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
            QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
            QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
            6u << QMI_M1_RFMT_DUMMY_LEN_LSB;            /* 24 dummy bits */
        qmi_hw->m[1].rcmd = 0xEBu << QMI_M1_RCMD_PREFIX_LSB;
        qmi_hw->m[1].wfmt =
            QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
            QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
            QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
            QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
            QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
            QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
        qmi_hw->m[1].wcmd = 0x38u << QMI_M1_WCMD_PREFIX_LSB;

        hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    }

    restore_interrupts(irq);
    return size;
}

size_t psram_init(void) {
    gpio_set_function(PIN_PSRAM_CS, GPIO_FUNC_XIP_CS1);
    return psram_detect_and_init();
}

static int stream_dma = -1;

/* ---- diagnostics (USB serial "psram") ---- */

/* Uncached, non-allocating alias of the PSRAM window: every access goes
 * to the chip, so read errors cannot hide behind the cache. */
#define PSRAM_NOCACHE ((volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + 0x01000000u))

static uint32_t pattern(uint32_t i, uint32_t pass) {
    uint32_t x = i * 0x9E3779B1u + pass * 0x85EBCA77u;
    return x ^ (x >> 15);
}

/* Writes and reads back `words` words; returns the number of bad words. */
static uint32_t test_words(uint32_t words, uint32_t pass, uint32_t *or_bits,
                           bool verbose) {
    volatile uint32_t *m = PSRAM_NOCACHE;
    uint32_t bad = 0, shown = 0;
    for (uint32_t i = 0; i < words; i++) m[i] = pattern(i, pass);
    for (uint32_t i = 0; i < words; i++) {
        uint32_t want = pattern(i, pass), got = m[i];
        if (got != want) {
            bad++;
            *or_bits |= got ^ want;
            if (verbose && shown < 6) {
                printf("  @%06lx want %08lx got %08lx\n", (unsigned long)i * 4u,
                       (unsigned long)want, (unsigned long)got);
                shown++;
            }
        }
    }
    return bad;
}

void psram_diag(bool sweep) {
    uint32_t t = qmi_hw->m[1].timing;
    uint32_t clkdiv = (t & QMI_M1_TIMING_CLKDIV_BITS) >> QMI_M1_TIMING_CLKDIV_LSB;
    uint32_t rxdelay = (t & QMI_M1_TIMING_RXDELAY_BITS) >> QMI_M1_TIMING_RXDELAY_LSB;
    printf("PSRAM ID KGD %02x EID %02x, QMI M1 timing %08lx: clkdiv %lu (%lu MHz), rxdelay %lu\n",
           psram_id_kgd, psram_id_eid, (unsigned long)t, (unsigned long)clkdiv,
           (unsigned long)(clock_get_hz(clk_sys) / 1000000u / (clkdiv ? clkdiv : 256u)),
           (unsigned long)rxdelay);
    printf("(this overwrites the VRAM)\n");

    /* first 512KB (the V9990 VRAM), two patterns */
    const uint32_t words = 512u * 1024u / 4u;
    uint32_t or_bits = 0, bad = 0;
    for (uint32_t pass = 0; pass < 2; pass++) bad += test_words(words, pass, &or_bits, true);
    printf("512KB x2: %lu bad words, bit errors %08lx\n", (unsigned long)bad,
           (unsigned long)or_bits);

    /* streaming read of data still in the (write-back) cache */
    if (stream_dma >= 0) {
        static uint32_t buf[512];
        volatile uint32_t *m = (volatile uint32_t *)(XIP_BASE + 0x01000000u + 0x40000u);
        for (uint32_t i = 0; i < 512; i++) m[i] = pattern(i, 99);
        uint32_t t0 = timer_hw->timerawl;
        psram_stream_read(buf, (const void *)m, sizeof buf);
        uint32_t dt = timer_hw->timerawl - t0;
        uint32_t sbad = 0;
        for (uint32_t i = 0; i < 512; i++) sbad += buf[i] != pattern(i, 99);
        t0 = timer_hw->timerawl;
        xip_cache_invalidate_range(0x01000000u + 0x48000u, 2048);
        volatile uint32_t sink = 0;
        for (uint32_t i = 0; i < 512; i++) sink += ((volatile uint32_t *)(XIP_BASE + 0x01048000u))[i];
        uint32_t dc = timer_hw->timerawl - t0;
        printf("stream 2KB: %lu bad words, %lu us (cached reads of 2KB: %lu us)\n",
               (unsigned long)sbad, (unsigned long)dt, (unsigned long)dc);
        /* clean data, row sized: what the renderer does */
        for (uint32_t n = 256; n <= 1024; n *= 2) {
            t0 = timer_hw->timerawl;
            for (int k = 0; k < 10; k++)
                psram_stream_read(buf, (const void *)(XIP_BASE + 0x01050000u + k * 4096u), n);
            uint32_t ts = timer_hw->timerawl - t0;
            xip_cache_invalidate_range(0x01060000u, 40960);
            t0 = timer_hw->timerawl;
            for (int k = 0; k < 10; k++) {
                const volatile uint32_t *q = (const volatile uint32_t *)(XIP_BASE + 0x01060000u + k * 4096u);
                for (uint32_t i = 0; i < n / 4; i++) sink += q[i];
            }
            uint32_t tc = timer_hw->timerawl - t0;
            printf("  %4lu bytes: stream %lu.%lu us, cached miss %lu.%lu us\n", (unsigned long)n,
                   (unsigned long)ts / 10, (unsigned long)ts % 10,
                   (unsigned long)tc / 10, (unsigned long)tc % 10);
        }
    }
    if (!sweep) return;

    /* rxdelay sweep at the current clkdiv, then a slower clock */
    for (uint32_t div = 2u; div <= clkdiv + 1u; div++) {
        for (uint32_t rx = 0; rx <= 7; rx++) {
            uint32_t nt = (t & ~(QMI_M1_TIMING_CLKDIV_BITS | QMI_M1_TIMING_RXDELAY_BITS)) |
                          div << QMI_M1_TIMING_CLKDIV_LSB | rx << QMI_M1_TIMING_RXDELAY_LSB;
            qmi_hw->m[1].timing = nt;
            or_bits = 0;
            bad = test_words(64u * 1024u / 4u, 7u + rx, &or_bits, false);
            printf("  clkdiv %lu rxdelay %lu: %lu bad of 16384, bits %08lx\n",
                   (unsigned long)div, (unsigned long)rx, (unsigned long)bad,
                   (unsigned long)or_bits);
        }
    }
    qmi_hw->m[1].timing = t;
    printf("timing restored\n");
}

/* ---- bulk reads (renderer row fetch) ---- */


void psram_stream_init(void) {
    stream_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)stream_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_XIP_STREAM);
    dma_channel_configure((uint)stream_dma, &c, NULL, (const void *)XIP_AUX_BASE, 0, false);
}

/* Copy `len` bytes at XIP address `src` (the cached PSRAM window) to `dst`
 * with one streaming burst. Pending writes in the cache are cleaned first:
 * the stream reads the chip, not the cache. */
void __not_in_flash_func(psram_stream_read)(void *dst, const void *src, uint32_t len) {
    uintptr_t a = (uintptr_t)src;
    if (stream_dma < 0 || ((a | (uintptr_t)dst | len) & 3u) || len == 0) {
        for (uint32_t i = 0; i < len; i++) ((uint8_t *)dst)[i] = ((const volatile uint8_t *)src)[i];
        return;
    }
    uintptr_t c0 = (a - XIP_BASE) & ~(uintptr_t)(XIP_CACHE_LINE_SIZE - 1u);
    uintptr_t c1 = (a - XIP_BASE + len + XIP_CACHE_LINE_SIZE - 1u) &
                   ~(uintptr_t)(XIP_CACHE_LINE_SIZE - 1u);
    xip_cache_clean_range(c0, c1 - c0);

    while (xip_ctrl_hw->stream_ctr) tight_loop_contents();
    dma_channel_set_write_addr((uint)stream_dma, dst, false);
    dma_channel_set_trans_count((uint)stream_dma, len / 4u, true);
    xip_ctrl_hw->stream_addr = a;
    xip_ctrl_hw->stream_ctr = len / 4u;
    dma_channel_wait_for_finish_blocking((uint)stream_dma);
}
