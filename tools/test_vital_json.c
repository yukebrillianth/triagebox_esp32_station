/*
 * Host self-check for tb_vital_json(). Not part of the firmware build.
 *
 *   gcc -I../main -o /tmp/tvj test_vital_json.c ../main/tb_vital_json.c && /tmp/tvj
 *
 * Exists because this is the one function whose output nobody sees until the
 * backend rejects it, and its whole job is deciding which keys NOT to emit.
 * Every assert below is a key that must be absent, which is precisely the
 * property that cannot be eyeballed from a UART log.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tb_vital_json.h"

static lora_vital_t base(void)
{
	lora_vital_t v = { 0 };

	v.node_id = 7;
	v.version = LORA_VITAL_VERSION;
	v.packet_counter = 42;
	v.battery = LORA_VITAL_BATTERY_NONE;
	v.priority = LORA_VITAL_PRIORITY_NONE;
	return v;
}

int main(void)
{
	char buf[TB_VITAL_JSON_MAX];
	lora_vital_t v = base();
	int n;

	/* Nothing valid: an object with only the two unconditional keys. No hr,
	 * no priority, no battery -- and critically no zeros standing in. */
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0);
	assert(n > 0);
	assert(strcmp(buf, "{\"packet_counter\":42,\"device_status\":0}") == 0);

	/* A scored patient with real vitals. */
	v.flags = TB_FLAG_HR_VALID | TB_FLAG_SPO2_VALID | TB_FLAG_RR_VALID;
	v.hr = 118;
	v.spo2 = 91;
	v.rr = 28;
	v.battery = 76;
	v.priority = LORA_VITAL_PRIORITY_RED;
	v.confidence = 87;
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 1755500000U);
	assert(n > 0);
	assert(strstr(buf, "\"hr\":118") != NULL);
	assert(strstr(buf, "\"priority\":\"RED\"") != NULL);
	assert(strstr(buf, "\"confidence\":0.87") != NULL);
	assert(strstr(buf, "\"battery\":76") != NULL);
	assert(strstr(buf, "\"ts\":1755500000") != NULL);
	/* BP is never published on this hardware. */
	assert(strstr(buf, "bp_sys") == NULL);

	/* BLACK is priority 0 and must survive as a string, not vanish. */
	v.priority = LORA_VITAL_PRIORITY_BLACK;
	v.confidence = 100;
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0);
	assert(n > 0);
	assert(strstr(buf, "\"priority\":\"BLACK\"") != NULL);
	assert(strstr(buf, "\"confidence\":1.00") != NULL);

	/* A tag, including a byte that must be escaped. */
	v = base();
	memcpy(v.victim_rfid, "04A2\"9F", 7);
	v.victim_rfid_len = 7;
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN + 7, 0);
	assert(n > 0);
	assert(strstr(buf, "\"victim_rfid\":\"04A2\\\"9F\"") != NULL);

	/* Rejections: bad version, a claimed tag longer than the frame, and a
	 * buffer too small. All must return -1 rather than emit half an object. */
	v = base();
	v.version = 0x02;
	assert(tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	v = base();
	v.victim_rfid_len = 20;
	assert(tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	v = base();
	assert(tb_vital_json(buf, 8, &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	printf("tb_vital_json: all checks passed\n");
	return 0;
}
