/*
 * Board-specific pin assignment. See docs/hardware.md for the wiring.
 *
 * The bus pins are chosen so that they are all reachable on the Pico-style
 * header of the Pimoroni Pico Plus 2 (GP0-GP22, GP26-GP28). A generic
 * RP2350B core board exposes all 48 GPIOs, so the same map works there too.
 *
 * NOTE: the PIO programs in bus.pio hard-code PIN_CS; keep them in sync
 * (checked by a static_assert in bus.c).
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* ---- MSX bus (through 74LVC245 level shifters) ---- */
#define PIN_A0        0   /* A0-A3  : GPIO 0-3  (port low nibble) */
#define PIN_D0        4   /* D0-D7  : GPIO 4-11 */
#define PIN_CS       20   /* /CS : /IORQ=0, /M1=1, A7-A4=0110 (74HCT688) */
#define PIN_RD       21   /* /RD */
#define PIN_VDP_SEL  22   /* SW1 (PCB rev 0.2): high = V9968, low = V9990.
                             On rev 0.1 boards this pin carries /WR. */
#define PIN_RESET    26   /* /RESET from the slot (input) */
#define PIN_INT      27   /* -> 74LVC07 -> /INT  (low = assert) */
#define PIN_WAIT     28   /* -> 74LVC07 -> /WAIT (low = assert) */

/* ---- DVI over HSTX: GPIO 12-19 only ----
 * Default matches the Pico DVI Sock / common RP2350 DVI breakouts:
 *   GP12/13 D0  GP14/15 CLK  GP16/17 D2  GP18/19 D1   (+ on the even pin)
 */
#define PIN_DVI_D0   12
#define PIN_DVI_CLK  14
#define PIN_DVI_D2   16
#define PIN_DVI_D1   18

/* ---- PSRAM (VRAM) ---- */
#if defined(PIMORONI_PICO_PLUS2_RP2350)
#define PIN_PSRAM_CS PIMORONI_PICO_PLUS2_PSRAM_CS_PIN   /* GPIO 47 */
#elif defined(WAVESHARE_CORE2350B)
#define PIN_PSRAM_CS WAVESHARE_CORE2350B_PSRAM_CS_PIN   /* GPIO 47 (B1/B2 versions) */
#elif defined(V9990_PSRAM_CS_PIN)
#define PIN_PSRAM_CS V9990_PSRAM_CS_PIN                 /* -DV9990_PSRAM_CS_PIN=... */
#else
#error "No PSRAM CS pin: build for pimoroni_pico_plus2_rp2350 or set V9990_PSRAM_CS_PIN"
#endif

#endif
