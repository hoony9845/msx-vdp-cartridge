/*
 * Waveshare Core2350B (RP2350B, 16MB W25Q128 flash, PSRAM on GPIO47 for the
 * B1 / B2 versions). https://www.waveshare.com/wiki/Core2350B0
 *
 * Board header for this project (not part of the pico-sdk).
 */
#ifndef _BOARDS_WAVESHARE_CORE2350B_H
#define _BOARDS_WAVESHARE_CORE2350B_H

// pico_cmake_set PICO_PLATFORM=rp2350

#define WAVESHARE_CORE2350B

#define PICO_RP2350A 0                       /* RP2350B: 48 GPIOs */

#define WAVESHARE_CORE2350B_PSRAM_CS_PIN 47
#define WAVESHARE_CORE2350B_LED_PIN 39       /* red user LED */

/* UART0 / I2C / SPI defaults are left unset: GPIO 0-28 are the MSX bus and DVI. */

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (16 * 1024 * 1024)
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
