#ifndef STATION_NET_H
#define STATION_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Wired network + MQTT for the station. One module, because the MQTT client is
 * only ever started from the Ethernet-got-IP event and stopped on link-down --
 * splitting them would mean exporting that glue between two files for no gain.
 *
 * Topics (station_id comes from CONFIG_TB_STATION_ID, node_id from the LoRa
 * packet -- IDs live in the topic, never in the JSON):
 *   triagebox/{station_id}/{node_id}/vital    QoS 1
 *   triagebox/{station_id}/{node_id}/status
 *   triagebox/{station_id}/status             retained, OFFLINE as the LWT
 */

/** Bring up W5500 + DHCP, and connect MQTT once an IP arrives. Non-blocking:
 * returns as soon as the drivers are started, link-up happens on the event
 * loop. Call once. */
esp_err_t station_net_start(void);

/** True once the broker has acknowledged CONNECT. */
bool station_net_mqtt_connected(void);

/**
 * Publish @p json under triagebox/{station_id}/node-NN/@p leaf.
 *
 * The uint8 @p node_id from the LoRa packet is formatted here, in one place, as
 * the zero-padded string id the backend registered ("node-07"). The backend
 * drops topics whose segments it does not recognise and explicitly never invents
 * node ids from binary fields, so this mapping is load-bearing: get the padding
 * wrong and every packet is silently discarded at the far end.
 *
 * Returns ESP_ERR_INVALID_STATE while the broker is disconnected. There is no
 * queue: a vital that cannot be delivered now is stale by the time the link
 * returns, and the next poll is 15s away.
 */
esp_err_t station_net_publish(uint8_t node_id, const char *leaf,
			      const char *json, int qos, bool retain);

#endif /* STATION_NET_H */
