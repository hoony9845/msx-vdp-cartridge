/*
 * APS6404L (8MB QSPI PSRAM) on QMI chip-select 1.
 * Sequence follows the CircuitPython / MicroPython RP2350 PSRAM setup.
 * Call after the final system clock has been set: the QMI timing is
 * derived from clk_sys.
 */
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "board_pins.h"
#include "psram.h"

#define PSRAM_MAX_FREQ_HZ   109000000   /* conservative (datasheet max 133MHz) */
#define PSRAM_TCEM_NS       8000        /* max CS low time */
#define PSRAM_TCPH_NS       18          /* min CS high time */

static void __not_in_flash_func(qmi_wait_idle)(void) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {}
}

static size_t __not_in_flash_func(psram_detect_and_init)(void) {
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

    if (kgd == 0x5D) {
        size = 1024 * 1024;                 /* EID bits 7:5 encode the size */
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) size *= 8;
        else if (size_id == 0) size *= 2;
        else if (size_id == 1) size *= 4;

        /* Reset enable, reset, enter quad mode */
        qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
        qmi_wait_idle();
        static const uint8_t cmds[3] = { 0x66, 0x99, 0x35 };
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
