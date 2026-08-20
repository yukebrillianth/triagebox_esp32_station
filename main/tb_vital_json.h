#ifndef TB_VITAL_JSON_H
#define TB_VITAL_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "lora_vital.h"

/*
 * lora_vital_t -> the canonical MQTT vital JSON. Deliberately free of ESP-IDF
 * headers so tools/test_vital_json.c can compile it with host gcc: this is the
 * only part of the LoRa path whose output a human reads, and the only part whose
 * bugs (a stray comma, an invented blood pressure, an unescaped RFID tag) are
 * invisible on the UART and fatal at the broker.
 *
 * ponytail: snprintf, not cJSON. The shape is fixed, nine of its keys are
 * conditional and none of them nest -- a JSON library would be a new dependency
 * plus a malloc per packet to emit a string this file emits in one pass. Move to
 * cJSON if the payload ever gains arrays or objects that vary in shape.
 */

/** Worst case: every key present with a 20-character tag is ~200 bytes. */
#define TB_VITAL_JSON_MAX 256U

/**
 * Render @p v (already accepted by lora_vital_valid()) into @p out.
 *
 * @param ts epoch seconds, or 0 to omit the key -- the backend stamps arrival
 *           time itself, so 0 is honest and a made-up ts is not.
 * @return bytes written, or -1 if the packet is invalid or @p cap is too small.
 *         -1 must not be published: a truncated JSON object is worse than a
 *         dropped packet, because the broker accepts it and the parser at the
 *         far end is what fails.
 */
int tb_vital_json(char *out, size_t cap, const lora_vital_t *v, uint8_t len,
		  uint32_t ts);

#endif /* TB_VITAL_JSON_H */
