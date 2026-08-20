#include "tb_vital_json.h"

#include <stdarg.h>
#include <stdio.h>

#include "tb_regs.h" /* TB_FLAG_* */

/*
 * Appends with a running offset and a hard bound. Every helper below returns the
 * new offset and refuses to write past cap, so a single overflow check at the end
 * covers the whole object -- snprintf's return value is the length it WANTED,
 * which is what makes the truncation detectable at all.
 */
static int append(char *out, size_t cap, int at, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));

static int append(char *out, size_t cap, int at, const char *fmt, ...)
{
	if (at < 0 || (size_t) at >= cap) {
		return -1;
	}

	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(out + at, cap - (size_t) at, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t) n >= cap - (size_t) at) {
		return -1; /* truncated; caller must not publish */
	}
	return at + n;
}

/*
 * The tag is ASCII hex from a PN532 UID, so in practice nothing needs escaping.
 * It is escaped anyway because "in practice" is doing a lot of work in that
 * sentence: the bytes come off a radio, and lora_vital_valid() checks the length
 * of this field, not its contents. A single stray quote would otherwise produce
 * malformed JSON that the broker forwards happily.
 */
static int append_rfid(char *out, size_t cap, int at, const char *tag, uint8_t n)
{
	at = append(out, cap, at, "\"victim_rfid\":\"");
	for (uint8_t i = 0; i < n && at >= 0; i++) {
		unsigned char c = (unsigned char) tag[i];

		if (c == '"' || c == '\\') {
			at = append(out, cap, at, "\\%c", c);
		} else if (c < 0x20 || c > 0x7E) {
			at = append(out, cap, at, "\\u%04x", c);
		} else {
			at = append(out, cap, at, "%c", c);
		}
	}
	return append(out, cap, at, "\",");
}

int tb_vital_json(char *out, size_t cap, const lora_vital_t *v, uint8_t len,
		  uint32_t ts)
{
	if (out == NULL || v == NULL || cap < 3) {
		return -1;
	}
	if (!lora_vital_valid(v, len)) {
		return -1;
	}

	int at = append(out, cap, 0, "{");

	/*
	 * KEYS ARE OMITTED, NEVER ZEROED. Every conditional below is a value
	 * whose zero means something clinical: 0 hr is a dead patient, 0 spo2 is
	 * asphyxia, 0 battery is a flat node and priority 0 is BLACK. The
	 * backend accepts missing keys and null; it cannot tell an invented zero
	 * from a measured one. This is the single most important property of
	 * this function.
	 */
	if (v->victim_rfid_len > 0) {
		at = append_rfid(out, cap, at, v->victim_rfid,
				 v->victim_rfid_len);
	}
	if (v->flags & TB_FLAG_HR_VALID) {
		at = append(out, cap, at, "\"hr\":%u,", v->hr);
	}
	if (v->flags & TB_FLAG_SPO2_VALID) {
		at = append(out, cap, at, "\"spo2\":%u,", v->spo2);
	}
	if (v->flags & TB_FLAG_RR_VALID) {
		at = append(out, cap, at, "\"rr\":%u,", v->rr);
	}
	/* Permanently clear on this hardware -- nothing measures pressure. Kept
	 * as a flag test rather than deleted so the day a cuff appears, this
	 * starts publishing without anyone remembering to come back here. */
	if (v->flags & TB_FLAG_BP_VALID) {
		at = append(out, cap, at, "\"bp_sys\":%u,\"bp_dia\":%u,",
			    v->bp_sys, v->bp_dia);
	}
	if (v->battery != LORA_VITAL_BATTERY_NONE) {
		at = append(out, cap, at, "\"battery\":%u,", v->battery);
	}

	const char *prio = lora_vital_priority_name(v->priority);

	/* confidence rides with priority: a confidence for a priority that was
	 * never assigned describes nothing. Emitted as 0..1 because that is what
	 * the LCD repo and the simulator already speak -- the backend also
	 * accepts 0..100, but one wire format beats two. */
	if (prio != NULL) {
		unsigned pct = v->confidence > 100U ? 100U : v->confidence;

		at = append(out, cap, at,
			    "\"priority\":\"%s\",\"confidence\":%u.%02u,", prio,
			    pct / 100U, pct % 100U);
	}

	/* Unconditional: diagnostics, and 0 is a meaningful value for both. */
	at = append(out, cap, at, "\"packet_counter\":%u,\"device_status\":%u",
		    v->packet_counter, v->device_status);

	if (ts != 0) {
		/* Epoch seconds, which the contract accepts alongside ms and
		 * ISO-8601. Cheapest of the three by a wide margin. */
		at = append(out, cap, at, ",\"ts\":%lu", (unsigned long) ts);
	}

	at = append(out, cap, at, "}");
	return at;
}
