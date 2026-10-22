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
	 * no priority, no battery, no esi/age/gender -- and critically no zeros
	 * standing in. The exact strcmp is what proves the omission. */
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0);
	assert(n > 0);
	assert(strcmp(buf, "{\"packet_counter\":42,\"device_status\":0}") == 0);

	/* A scored patient with real vitals and the operator's facts. */
	v.flags = TB_FLAG_HR_VALID | TB_FLAG_SPO2_VALID | TB_FLAG_RR_VALID;
	v.hr = 118;
	v.spo2 = 91;
	v.rr = 28;
	v.battery = 76;
	v.priority = LORA_VITAL_PRIORITY_RED;
	v.confidence = 87;
	v.esi = 3;
	v.age = 47;
	v.gender = 'M';
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 1755500000U);
	assert(n > 0);
	assert(strstr(buf, "\"hr\":118") != NULL);
	assert(strstr(buf, "\"hr_source\":\"ECG\"") != NULL);
	/* The PPG fallback is labelled as such; no hr, no source. */
	v.flags |= TB_FLAG_HR_FROM_PPG;
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 1755500000U);
	assert(strstr(buf, "\"hr_source\":\"PPG\"") != NULL);
	v.flags &= (uint8_t)~TB_FLAG_HR_FROM_PPG;
	{
		uint8_t saved = v.flags;
		v.flags = TB_FLAG_HR_FROM_PPG; /* the bit alone, hr not valid */
		n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0);
		assert(strstr(buf, "hr_source") == NULL && strstr(buf, "\"hr\"") == NULL);
		v.flags = saved;
	}
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 1755500000U);
	assert(strstr(buf, "\"priority\":\"RED\"") != NULL);
	assert(strstr(buf, "\"confidence\":0.87") != NULL);
	assert(strstr(buf, "\"battery\":76") != NULL);
	assert(strstr(buf, "\"ts\":1755500000") != NULL);
	assert(strstr(buf, "\"esi\":3") != NULL);
	assert(strstr(buf, "\"age\":47") != NULL);
	assert(strstr(buf, "\"gender\":\"M\"") != NULL);
	/* BP values ride in the packet but TB_FLAG_BP_VALID gates the keys. */
	v.bp_sys = 120;
	v.bp_dia = 80;
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

	/* The documented worst case: every key at once, full 20-character tag,
	 * widest values. tb_vital_json.h's capacity claim is this assert.
	 *
	 * battery is 254 and not 255 on purpose -- 255 IS LORA_VITAL_BATTERY_NONE,
	 * so the widest battery that still emits a key is a 3-digit non-sentinel. */
	v = base();
	v.flags = TB_FLAG_HR_VALID | TB_FLAG_SPO2_VALID | TB_FLAG_RR_VALID |
		  TB_FLAG_BP_VALID;
	v.packet_counter = 65535;
	v.device_status = 255;
	v.hr = 65535;
	v.spo2 = 255;
	v.rr = 255;
	v.bp_sys = 65535;
	v.bp_dia = 65535;
	v.battery = 254;
	v.priority = LORA_VITAL_PRIORITY_YELLOW;
	v.confidence = 100;
	v.esi = 5;
	v.age = 255;
	v.gender = 'F';
	memset(v.victim_rfid, 'F', LORA_VITAL_RFID_MAX);
	v.victim_rfid_len = LORA_VITAL_RFID_MAX;
	n = tb_vital_json(buf, sizeof(buf), &v,
			  LORA_VITAL_FIXED_LEN + LORA_VITAL_RFID_MAX,
			  0xFFFFFFFFU);
	assert(n > 0);
	assert(n < (int) TB_VITAL_JSON_MAX);
	assert(strstr(buf, "\"battery\":254") != NULL);
	printf("worst-case vital: %d of %u bytes\n", n, TB_VITAL_JSON_MAX);

	/* Rejections: bad version, a claimed tag longer than the frame, and a
	 * buffer too small. All must return -1 rather than emit half an object. */
	v = base();
	v.version = 0x01;
	assert(tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	v = base();
	v.victim_rfid_len = 20;
	assert(tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	v = base();
	assert(tb_vital_json(buf, 8, &v, LORA_VITAL_FIXED_LEN, 0) == -1);

	/* A gender byte that is not an answer is omitted, not published: the
	 * byte comes off a radio and valid() does not inspect it, so this is
	 * the JSON's own guard against a stray byte reaching the broker. */
	v = base();
	v.priority = LORA_VITAL_PRIORITY_GREEN;
	v.gender = '"';
	n = tb_vital_json(buf, sizeof(buf), &v, LORA_VITAL_FIXED_LEN, 0);
	assert(n > 0);
	assert(strstr(buf, "gender") == NULL);

	printf("tb_vital_json: all checks passed\n");
	return 0;
}
