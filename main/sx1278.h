#ifndef SX1278_H
#define SX1278_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Minimal SX1278 LoRa driver for the TriageBox station.
 *
 * ponytail: half-duplex, one packet in flight, blocking transmit, no FSK, no
 * CAD, no frequency hopping, no implicit header, no continuous-receive-while-
 * transmitting. The station's whole job on this radio is "send a 5-byte poll,
 * wait up to 250ms for one reply, repeat" -- a state machine with two states.
 * A general-purpose SX127x driver is several hundred lines more and every one
 * of them is a line that can be wrong on hardware I cannot probe.
 *
 * ALL SETTINGS MUST MATCH THE NODE'S newLoRa() DEFAULTS EXACTLY. The node uses
 * the library's constructor defaults, NOT the #defines in its main.c, so the
 * agreed air config is: 433MHz, SF7, BW125kHz, CR4/5, preamble 8, explicit
 * header, CRC on, sync word 0x12 (the reset default -- neither end writes it).
 * A single mismatched parameter yields silence, not an error.
 */

/** Bring up SPI3 and the radio. Verifies RegVersion, so a wiring fault is
 * reported here rather than as an absence of packets later. */
esp_err_t sx1278_init(void);

/** Transmit @p len bytes and block until the SX1278 reports TxDone.
 * @p timeout_ms bounds the wait; at SF7 a 38-byte packet is ~82ms of air. */
esp_err_t sx1278_transmit(const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/**
 * Wait for one packet, up to @p timeout_ms.
 *
 * Returns ESP_ERR_TIMEOUT when nothing arrives -- the expected outcome for an
 * absent node, so callers must not treat it as an error worth logging loudly.
 * Returns ESP_ERR_INVALID_CRC when a frame arrived but was corrupt, which is
 * worth counting separately: timeouts mean nobody answered, CRC errors mean
 * somebody did and the link is marginal.
 *
 * @param buf     destination, at least @p cap bytes
 * @param cap     capacity; a longer packet is rejected, not truncated
 * @param len_out bytes actually received
 * @param rssi    dBm of the received packet, or NULL
 * @param snr     dB, or NULL
 */
esp_err_t sx1278_receive(uint8_t *buf, uint8_t cap, uint8_t *len_out,
			 uint32_t timeout_ms, int *rssi, float *snr);

#endif /* SX1278_H */
