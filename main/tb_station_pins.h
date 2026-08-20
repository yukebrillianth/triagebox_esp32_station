#ifndef TB_STATION_PINS_H
#define TB_STATION_PINS_H

#include "driver/spi_master.h"

/*
 * TriageBox station pin map -- ESP32-WROOM (classic), W5500 Ethernet, SX1278 433MHz.
 * ONE PLACE. Nothing else in this project may name a GPIO number.
 *
 * Two separate SPI buses on purpose.
 *
 * BUS PIN CHOICE: SPI3 (SX1278) uses its IOMUX defaults 18/19/23. SPI2 (W5500)
 * uses the IOMUX defaults for SCK/MOSI but MISO is DELIBERATELY OFF-IOMUX on
 * GPIO35 -- see the strapping note below. ESP-IDF picks the fast IOMUX path
 * only when EVERY bus pin matches, so SPI2 routes through the GPIO matrix,
 * which caps reliable full-duplex around 26MHz. That is above the 20MHz
 * configured here, so it costs nothing. DO NOT "fix" MISO back to GPIO12.
 * Do not relocate 13/14 or 18/19/23 for tidiness either. CS is per-device and
 * may live anywhere, which is why 16 is fine below.
 *
 * IDF renamed these hosts years ago: SPI2_HOST is the old HSPI, SPI3_HOST the
 * old VSPI. SPI1_HOST is the flash bus -- not available.
 *
 * BOOT-TIME STRAPPING (read before changing anything):
 *   GPIO12 = MTDI is sampled AT RESET to select the VDD_SDIO voltage. If
 *     anything drives it high while the chip resets, the module boots expecting
 *     1.8V flash and does not boot at all. W5500 MISO was originally here: the
 *     W5500 releases MISO only while its CS is high, and GPIO16 has no
 *     reset-default pull, so CS floats until this firmware runs and the W5500
 *     could drive MTDI during reset. FIXED IN HARDWARE 2026-08-17 by moving
 *     MISO to GPIO35 -- input-only, nothing can drive it, no resistor needed.
 *     GPIO12 IS NOW UNCONNECTED. Keep it that way.
 *     Internal pulls (gpio_set_pull_mode / pull_up_en) could never have fixed
 *     this: pin config reverts to the IOMUX default on every reset and the
 *     strap is latched before any code runs. Only reset-default pulls and
 *     external resistors exist at t=0.
 *   GPIO2 is deliberately UNUSED. Download boot needs it low/floating, and the
 *     W5500's open-drain INTn with a breakout pull-up fights esptool's
 *     auto-reset. INT was moved 2 -> 4 for exactly this reason; GPIO4 is not a
 *     strapping pin.
 *   GPIO5 (SX1278 CS) and GPIO15 (W5500 RST) have PULL-UP as their IOMUX reset
 *     default, which reads as "deselected" and "not held in reset". Benign, and
 *     no external resistor is needed on either.
 *
 * CONSEQUENCES OF THIS MAP:
 *   - 13/14/15 are MTCK/MTMS/MTDO, so there is no JTAG on this board. UART0
 *     (GPIO1/3) console logging only. Accepted -- the user has no debugger.
 *   - GPIO16 is the board's "RX2" label, so UART2 is gone. Nothing needs it;
 *     the link to the sensor nodes is a radio, not a wire.
 *   - GPIO16/17 carry PSRAM on WROVER modules. This map is WROOM-only. If the
 *     module is ever swapped for a WROVER, the W5500 CS must move.
 *   - GPIO35 is input-only and has no internal pulls at all (true of 34-39).
 *     Fine for MISO, useless for anything that must be driven. Check a new
 *     board does not wire it to a battery-sense divider (common on LilyGO/TTGO,
 *     absent on plain DevKitC) before reusing this map.
 */

/* SX1278 -- SPI3 / old VSPI, IOMUX defaults 18/19/23 */
#define TB_LORA_SPI_HOST SPI3_HOST
#define TB_LORA_PIN_SCK  18
#define TB_LORA_PIN_MISO 19
#define TB_LORA_PIN_MOSI 23
#define TB_LORA_PIN_CS   5  /* pull-up is the reset default; no resistor */
#define TB_LORA_PIN_DIO0 25 /* RX done / TX done */
#define TB_LORA_PIN_RST  33

/* W5500 -- SPI2 / old HSPI. SCK/MOSI on IOMUX defaults; MISO is not. */
#define TB_ETH_SPI_HOST SPI2_HOST
#define TB_ETH_PIN_SCK  14
#define TB_ETH_PIN_MISO 35 /* input-only, off-IOMUX ON PURPOSE; keeps GPIO12 free */
#define TB_ETH_PIN_MOSI 13
#define TB_ETH_PIN_CS   16 /* board "RX2" */
#define TB_ETH_PIN_INT  4  /* open-drain INTn */
#define TB_ETH_PIN_RST  15

/*
 * ponytail: 8MHz, well under the 33-36MHz the W5500 datasheet allows. Dropped
 * from 20MHz on 2026-08-17 while chasing a link that came up, flapped once,
 * and never completed DHCP -- the signature of reads that work for 2-byte PHY
 * registers and corrupt on frame-length bursts. Two things make 20MHz optimistic
 * here: jumper wires on a breadboard, and MISO on GPIO35 routing through the
 * GPIO matrix (see above), whose input delay caps reliable full-duplex nearer
 * 26MHz than 40. A marginal SPI clock fails as dropped frames and phantom link
 * transitions, never as a clean error. Raise it only on a PCB, one step at a
 * time, and watch for link flaps -- they are the first symptom.
 */
#define TB_ETH_SPI_CLOCK_HZ (8 * 1000 * 1000)

#endif /* TB_STATION_PINS_H */
