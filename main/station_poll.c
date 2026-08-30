#include "station_poll.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lora_poll.h"
#include "lora_vital.h"
#include "station_net.h"
#include "sx1278.h"
#include "tb_vital_json.h"

#ifndef CONFIG_TB_POLL_PERIOD_MS
#define CONFIG_TB_POLL_PERIOD_MS LORA_POLL_PERIOD_MS
#endif

static const char *TAG = "poll";

/* Kconfig already bounds this 1..20; the assert is here because the bound that
 * matters is the array below, not the menu. */
_Static_assert(CONFIG_TB_NODE_COUNT <= LORA_POLL_NODE_MAX,
	       "CONFIG_TB_NODE_COUNT exceeds LORA_POLL_NODE_MAX");

/* Indexed by node id, so 1-based with slot 0 wasted. Four bytes of RAM buys
 * the absence of every off-by-one in this file. */
static uint8_t s_misses[LORA_POLL_NODE_MAX + 1];
static bool s_online[LORA_POLL_NODE_MAX + 1];

/*
 * The station has no RTC and no SNTP (the bench topology is a direct cable to a
 * PC: no gateway, no DNS, no reachable time server -- see Kconfig
 * TB_ETH_STATIC_IP). An unsynchronised ESP32 reports 1970, so this returns 0 and
 * tb_vital_json() omits "ts", which the backend accepts and stamps itself.
 *
 * ponytail: the threshold, not a bool flag. When SNTP is added -- one call in
 * station_net.c once the netif has a route -- this starts returning real epoch
 * seconds with no change here. Publishing a 1970 timestamp would be worse than
 * publishing none: the backend cannot tell it is wrong.
 */
static uint32_t epoch_now(void)
{
	time_t t = time(NULL);

	return (t > 1600000000) ? (uint32_t) t : 0U;
}

/*
 * Node status is published EVERY CYCLE, not only on transition.
 *
 * It used to be transition-only, which was cheaper and wrong in a way that only
 * appears once vitals can be legitimately withheld. `lastSeen` on the backend is
 * refreshed by a vital OR a node status, and a node is marked OFFLINE after
 * NODE_OFFLINE_SEC (45s) without either. An unscored node publishes no vital (see
 * poll_one), so with transition-only status the backend would mark it OFFLINE and
 * the station would have no transition left to correct it -- s_online[] is still
 * true, so nothing is ever published again. A live node answering every poll would
 * look dead forever. The failing case is not hypothetical: a node whose MAX30102
 * is dead never gets scored, so it never has a vital worth sending.
 *
 * One message per answering node per 15s: 20 nodes is 1.3 msg/s, which no broker
 * notices. The bonus is that rssi/snr in the dashboard become current values
 * rather than "whatever they were when the node came up", which is what makes the
 * link-quality column worth looking at at all.
 */
static void publish_status_online(uint8_t node_id, const lora_vital_t *v,
				  int rssi, float snr)
{
	char json[128];
	char battery[24] = "";

	/* Built separately rather than as a conditional argument pair: the
	 * obvious "%s%u" with ("" , 0) appends a 0 to the PREVIOUS number, so
	 * packet_count 1421 becomes 14210. Omitted keys and printf do not mix. */
	if (v->battery != LORA_VITAL_BATTERY_NONE) {
		snprintf(battery, sizeof(battery), ",\"battery\":%u",
			 v->battery);
	}

	/* %.1f: fine on esp32, where full newlib formatting is the default.
	 * CONFIG_NEWLIB_NANO_FORMAT=y would silently print nothing here. */
	snprintf(json, sizeof(json),
		 "{\"status\":\"ONLINE\",\"rssi\":%d,\"snr\":%.1f,"
		 "\"packet_count\":%u%s}",
		 rssi, snr, v->packet_counter, battery);

	/* Retained: node liveness is state, not an event. A dashboard that
	 * subscribes between cycles must not see a node as missing. */
	station_net_publish(node_id, "status", json, 1, true);
}

static void mark_miss(uint8_t node_id)
{
	if (s_misses[node_id] < 0xFF) {
		s_misses[node_id]++;
	}

	if (s_misses[node_id] == LORA_POLL_MISS_LIMIT && s_online[node_id]) {
		s_online[node_id] = false;
		ESP_LOGW(TAG, "node %u offline after %u misses", node_id,
			 LORA_POLL_MISS_LIMIT);
		/* No rssi/snr/battery: there is no fresh measurement to report
		 * and last-known values on an offline node read as current. */
		station_net_publish(node_id, "status", "{\"status\":\"OFFLINE\"}",
				    1, true);
	}
}

static void poll_one(uint8_t node_id)
{
	const lora_poll_t p = {
		.magic = LORA_POLL_MAGIC,
		.version = LORA_POLL_VERSION,
		.station_id = CONFIG_TB_STATION_NUM,
		.node_id = node_id,
		.command = LORA_POLL_CMD_REPORT,
	};
	uint8_t buf[LORA_VITAL_FIXED_LEN + LORA_VITAL_RFID_MAX];
	uint8_t len = 0;
	int rssi = 0;
	float snr = 0.0f;

	esp_err_t err = sx1278_transmit((const uint8_t *) &p, LORA_POLL_LEN,
					LORA_POLL_SLOT_MS);

	if (err != ESP_OK) {
		/* The radio failed to get 5 bytes out in 250ms, which is a
		 * station fault, not a node fault -- log it as one and do not
		 * blame the node's miss counter. */
		ESP_LOGE(TAG, "poll tx node %u: %s", node_id,
			 esp_err_to_name(err));
		return;
	}

	/* The node's reply deadline is strictly under LORA_POLL_SLOT_MS, so
	 * waiting a full slot from the END of our transmit is generous on
	 * purpose: a slot that is too tight fails as a timeout indistinguishable
	 * from a dead node. Cost is bounded -- 20 nodes x (30ms tx + 250ms wait)
	 * is under 6s inside the 15s period even if every node is absent. */
	err = sx1278_receive(buf, sizeof(buf), &len, LORA_POLL_SLOT_MS, &rssi,
			     &snr);

	if (err == ESP_ERR_TIMEOUT) {
		mark_miss(node_id); /* expected for an absent node; not logged */
		return;
	}
	if (err != ESP_OK) {
		/* ESP_ERR_INVALID_CRC means somebody answered and the link is
		 * marginal -- a different problem from silence, and worth seeing
		 * in the log because it is the one that antenna work fixes. */
		ESP_LOGW(TAG, "rx node %u: %s", node_id, esp_err_to_name(err));
		mark_miss(node_id);
		return;
	}

	const lora_vital_t *v = (const lora_vital_t *) buf;

	if (!lora_vital_valid(v, len)) {
		ESP_LOGW(TAG, "node %u: bad vital (len %u ver 0x%02x)", node_id,
			 len, v->version);
		mark_miss(node_id);
		return;
	}

	/*
	 * Every node hears every reply on this shared channel, so a frame that
	 * passed CRC is not necessarily the one we asked for -- a node answering
	 * late lands in the next node's slot. Trusting the reply and publishing
	 * it under the polled id would attribute one patient's vitals to
	 * another, which is the worst possible failure of this system.
	 */
	if (v->node_id != node_id) {
		ESP_LOGW(TAG, "polled %u, node %u answered -- dropped", node_id,
			 v->node_id);
		mark_miss(node_id);
		return;
	}

	s_misses[node_id] = 0;
	if (!s_online[node_id]) {
		s_online[node_id] = true;
		ESP_LOGI(TAG, "node %u online (rssi %d snr %.1f)", node_id, rssi,
			 snr);
	}
	/* Every cycle, not just on the transition -- see publish_status_online.
	 * This is also the only thing keeping an unscored node from being marked
	 * OFFLINE by the backend while it is answering every poll. */
	publish_status_online(node_id, v, rssi, snr);

	/*
	 * A vital with no priority has nothing to say: priority drives the triage
	 * board, the KPIs and the alerts, and it is the one field the backend still
	 * requires. lora_vital.h already says the station must omit priority and
	 * confidence when it sees 0xFF; the rule that follows from that is that the
	 * whole packet is not worth publishing.
	 *
	 * Only the vital is withheld. The status publish above already ran, so the
	 * node stays ONLINE with fresh rssi/snr -- it answered, it is alive, it just
	 * has no triage decision yet. Normal for the first cycles after boot, and
	 * permanent for a node whose PPG sensor is dead.
	 */
	if (lora_vital_priority_name(v->priority) == NULL) {
		ESP_LOGD(TAG, "node %u unscored, vital withheld", node_id);
		return;
	}

	char json[TB_VITAL_JSON_MAX];
	int n = tb_vital_json(json, sizeof(json), v, len, epoch_now());

	if (n < 0) {
		ESP_LOGE(TAG, "node %u: json build failed", node_id);
		return;
	}

	esp_err_t pub = station_net_publish(node_id, "vital", json, 1, false);

	if (pub == ESP_ERR_INVALID_STATE) {
		/* Radio works, broker does not. Deliberately not queued: by the
		 * time MQTT returns, this reading is stale and a newer poll has
		 * happened.
		 *
		 * Logged at INFO, and the JSON goes with it, because while the
		 * broker is down this is the ONLY evidence that the LoRa half
		 * works -- and it is strong evidence: reaching this line means the
		 * reply passed CRC, arrived inside the slot, carried the node_id we
		 * polled, satisfied lora_vital_valid() and encoded cleanly. One
		 * line per answering node per 15s cycle, so a bench station with a
		 * node or two is 4-8 lines a minute, not a flood. Turn it back down
		 * to ESP_LOGD once the broker is up and the vitals are visible on
		 * the MQTT side instead. */
		ESP_LOGI(TAG, "node %u vital dropped, mqtt down: %s", node_id,
			 json);
	}
}

static void poll_task(void *arg)
{
	(void) arg;

	while (1) {
		TickType_t cycle = xTaskGetTickCount();

		for (uint8_t id = 1; id <= CONFIG_TB_NODE_COUNT; id++) {
			poll_one(id);
		}

		/* Fixed cycle start, not a fixed gap: the period must not drift
		 * with how many nodes answered, or the MQTT cadence wanders and
		 * two stations that were interleaved slowly collide. If a cycle
		 * overran the period this returns immediately, which is the
		 * right degradation -- keep polling, just slower.
		 *
		 * Sourced from Kconfig CONFIG_TB_POLL_PERIOD_MS (default 15000) so a
		 * range test can raise the cadence without editing a shared header --
		 * lora_poll.h is copied verbatim into the node project, so changing
		 * LORA_POLL_PERIOD_MS there would force both firmwares to be reflashed
		 * together for a station-only experiment. */
		xTaskDelayUntil(&cycle, pdMS_TO_TICKS(CONFIG_TB_POLL_PERIOD_MS));
	}
}

esp_err_t station_poll_start(void)
{
	esp_err_t err = sx1278_init();

	if (err != ESP_OK) {
		return err;
	}

	/* 4KB: the deepest thing here is snprintf on a 256-byte stack buffer.
	 * Core 1 -- see station_poll.h for why that matters. Priority 5 is above
	 * the default task priority and below the W5500/lwIP internals, which
	 * live on core 0 anyway. */
	BaseType_t ok = xTaskCreatePinnedToCore(poll_task, "lora_poll", 4096,
						NULL, 5, NULL, 1);

	if (ok != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	/* Both id spaces logged together: they are easy to confuse and a mismatch
	 * between the MQTT range here and the nodeIdBase used at adoption is
	 * undetectable at runtime -- the packets just stop arriving. */
	ESP_LOGI(TAG,
		 "polling radio 1..%d every %ums as station %d -> mqtt node-%02u..node-%02u",
		 CONFIG_TB_NODE_COUNT, CONFIG_TB_POLL_PERIOD_MS,
		 CONFIG_TB_STATION_NUM,
		 (unsigned) (1 + TB_NODE_ID_OFFSET),
		 (unsigned) (CONFIG_TB_NODE_COUNT + TB_NODE_ID_OFFSET));
	return ESP_OK;
}
