/*
 * VDP selection setting (see vdp_select.h).
 *
 * Writing flash stalls XIP, which the bus core and the video IRQ cannot
 * afford while running. So a new choice only goes to a watchdog scratch
 * register; after the reboot, vdp_select_boot() writes it to flash while
 * nothing else runs yet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "board_pins.h"
#include "vdp_select.h"
#include "bus.h"
#include "psram.h"

#define SETTING_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTING_MAGIC   0x56445053u          /* "VDPS" */
#define PENDING_MAGIC   0x50454E44u          /* "PEND" in scratch[0] */

#ifndef MSX_VDP_DEFAULT_V9968
#define MSX_VDP_DEFAULT_V9968 0
#endif
#ifndef MSX_VDP_SWITCH
#define MSX_VDP_SWITCH 1
#endif

typedef struct {
    uint32_t magic;
    uint32_t chip;
    uint32_t check;                          /* ~chip */
} setting_t;

static vdp_chip_t default_chip(void) {
    return MSX_VDP_DEFAULT_V9968 ? VDP_CHIP_V9968 : VDP_CHIP_V9990;
}

static bool read_setting(vdp_chip_t *chip) {
    const setting_t *s = (const setting_t *)(XIP_BASE + SETTING_OFFSET);
    if (s->magic != SETTING_MAGIC || s->check != ~s->chip || s->chip > VDP_CHIP_V9968)
        return false;
    *chip = (vdp_chip_t)s->chip;
    return true;
}

static void write_setting(vdp_chip_t chip) {
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    setting_t s = { SETTING_MAGIC, (uint32_t)chip, ~(uint32_t)chip };
    memcpy(page, &s, sizeof s);
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(SETTING_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTING_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq);
}

/* SW1 is driven to 3.3V or GND (R10/R11 hold it while it moves). */
static vdp_chip_t read_switch(void) {
    gpio_init(PIN_VDP_SEL);
    gpio_set_dir(PIN_VDP_SEL, GPIO_IN);
    gpio_disable_pulls(PIN_VDP_SEL);
    sleep_us(100);
    return gpio_get(PIN_VDP_SEL) ? VDP_CHIP_V9968 : VDP_CHIP_V9990;
}

vdp_chip_t vdp_select_boot(void) {
    if (MSX_VDP_SWITCH) {
        watchdog_hw->scratch[0] = 0;
        return read_switch();
    }
    if (watchdog_hw->scratch[0] == PENDING_MAGIC) {
        vdp_chip_t want = (vdp_chip_t)(watchdog_hw->scratch[1] & 1);
        watchdog_hw->scratch[0] = 0;
        vdp_chip_t have;
        if (!read_setting(&have) || have != want) write_setting(want);
    }
    vdp_chip_t chip;
    return read_setting(&chip) ? chip : default_chip();
}

const char *vdp_chip_name(vdp_chip_t chip) {
    return chip == VDP_CHIP_V9968 ? "V9968 + geo3d v2.0" : "V9990";
}

static void switch_to(vdp_chip_t chip) {
    printf("switching to %s, rebooting\n", vdp_chip_name(chip));
    stdio_flush();
    watchdog_hw->scratch[0] = PENDING_MAGIC;
    watchdog_hw->scratch[1] = (uint32_t)chip;
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

/* "chip v9990" | "chip v9968" | "chip" (show) */
static void command(const char *line, vdp_chip_t current) {
    while (*line == ' ') line++;
    if (strncmp(line, "psram", 5) == 0) {
        psram_diag(strstr(line, "sweep") != NULL);
        return;
    }
    if (strncmp(line, "out ", 4) == 0) {
        char *end;
        unsigned long port = strtoul(line + 4, &end, 16);
        unsigned long value = strtoul(end, &end, 16);
        bus_debug_out((unsigned)port, (unsigned)value);
        return;
    }
    if (strncmp(line, "vram", 4) == 0) {
        bus_print_vram_crc();
        return;
    }
    if (strncmp(line, "stat", 4) == 0) {
        bus_print_stats(strstr(line, "reset") != NULL);
        return;
    }
    if (strncmp(line, "chip", 4) != 0) {
        if (*line) printf("commands: chip [v9990|v9968], stat [reset], psram [sweep], vram, out <port> <val>\n");
        return;
    }
    line += 4;
    while (*line == ' ') line++;
    if (MSX_VDP_SWITCH && *line) {
        printf("the chip is set by the switch on the cartridge (SW1); "
               "change it with the power off\n");
        line = "";
    }
    if (strcmp(line, "v9990") == 0 || strcmp(line, "V9990") == 0) {
        if (current != VDP_CHIP_V9990) switch_to(VDP_CHIP_V9990);
    } else if (strcmp(line, "v9968") == 0 || strcmp(line, "V9968") == 0) {
        if (current != VDP_CHIP_V9968) switch_to(VDP_CHIP_V9968);
    } else if (*line) {
        printf("unknown chip '%s' (v9990 or v9968)\n", line);
        return;
    }
    printf("chip: %s\n", vdp_chip_name(current));
}

void vdp_select_poll(vdp_chip_t current) {
    static char buf[32];
    static unsigned len;
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\r' || c == '\n') {
            buf[len] = 0;
            command(buf, current);
            len = 0;
        } else if (len < sizeof buf - 1) {
            buf[len++] = (char)c;
        }
    }
}
