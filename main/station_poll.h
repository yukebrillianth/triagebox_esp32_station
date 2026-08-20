#ifndef STATION_POLL_H
#define STATION_POLL_H

#include "esp_err.h"

/*
 * The station's poll cycle: bring up the radio and round-robin nodes
 * 1..CONFIG_TB_NODE_COUNT forever, publishing whatever answers.
 *
 * WHY THIS IS ITS OWN TASK ON CORE 1. The poll loop's correctness is a timing
 * property -- the node has LORA_POLL_SLOT_MS to answer, and the station must be
 * listening for all of it. On core 0 it would share a core with the W5500 driver,
 * the lwIP thread and the esp-mqtt task, any of which can hold the CPU through a
 * TCP retransmit or a DHCP renewal. Missing the front of a reply loses the whole
 * packet (the SX1278 needs the preamble), and the symptom would be nodes that
 * intermittently look dead in a way that correlates with network activity --
 * about the worst bug in this system to diagnose. Pinning is for isolation from
 * the network stack, NOT for throughput: this task is idle over 99% of the time.
 *
 * The MQTT client stays on core 0 (CONFIG_MQTT_USE_CORE_0), so the publish call
 * from this task crosses cores. That is fine -- esp_mqtt_client_publish() copies
 * into the outbox under a mutex and returns.
 */

/** Start the radio and the poll task. Call after station_net_start(); a poll
 * cycle runs whether or not the broker is up, because a radio link that only
 * works when MQTT is connected is two failures wearing one coat. */
esp_err_t station_poll_start(void);

#endif /* STATION_POLL_H */
