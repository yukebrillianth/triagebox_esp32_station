#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "station_net.h"
#include "station_poll.h"

/*
 * TriageBox station: polls the STM32 sensor nodes over LoRa (SX1278) and
 * republishes each reply as the canonical MQTT vital over wired Ethernet
 * (W5500). See tb_station_pins.h before touching any GPIO.
 *
 * DUAL CORE, BY ROLE NOT BY LOAD:
 *   core 0  W5500 driver, lwIP, esp-mqtt, the event loop -- everything that can
 *           block for an unbounded time on a network.
 *   core 1  the LoRa poll task, whose 250ms reply window is a hard deadline.
 * Both cores are idle almost all of the time; the split exists so a TCP
 * retransmit cannot eat the front of a node's reply. See station_poll.h.
 */

static const char *TAG = "station";

void app_main(void)
{
	/* esp-mqtt needs NVS for its own state, and DHCP hostname bookkeeping
	 * uses it too. A fresh or resized flash gives NO_FREE_PAGES once; erase
	 * and retry rather than aborting a station that would otherwise run. */
	esp_err_t err = nvs_flash_init();

	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	ESP_LOGI(TAG, "station %s -> broker %s", CONFIG_TB_STATION_ID,
		 CONFIG_TB_MQTT_URI);

	ESP_ERROR_CHECK(station_net_start());

	/* Deliberately NOT ESP_ERROR_CHECK: a radio that fails RegVersion is a
	 * wiring fault on one of two independent subsystems, and aborting here
	 * would take the working one (Ethernet, MQTT, the station's own status
	 * topic) down with it. A station that reports itself ONLINE with a dead
	 * radio is diagnosable from the dashboard; one stuck in a boot loop is
	 * only diagnosable with a serial cable. */
	esp_err_t radio = station_poll_start();

	if (radio != ESP_OK) {
		ESP_LOGE(TAG, "lora not started: %s -- network only",
			 esp_err_to_name(radio));
	}

	/* Bring-up heartbeat, ON TRANSITION ONLY. The poll task logs node
	 * transitions itself, so this only reports the thing that has no other
	 * symptom: whether the broker is reachable.
	 *
	 * It used to print every 5s unconditionally, which on a bench with no
	 * broker is 12 identical lines a minute -- enough to bury the `poll:`
	 * lines that are the only evidence the radio works. The state is still
	 * sampled every 5s; only the printing is edge-triggered. */
	bool was_up = false;
	bool first = true;

	while (1) {
		bool up = station_net_mqtt_connected();

		if (first || (up != was_up)) {
			ESP_LOGI(TAG, "mqtt %s", up ? "connected" : "down");
			was_up = up;
			first = false;
		}
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}
