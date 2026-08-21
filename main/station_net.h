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
 *   triagebox/{station_id}/announce           retained, published on connect
 */

/**
 * MQTT node id for a LoRa radio address, as this station publishes it.
 *
 * Radio addresses restart at 1 on every station, but node ids are unique across
 * the whole database -- the backend binds node-01 to exactly one station and
 * silently drops a vital that arrives under any other. So a second station
 * publishing its own radio address 1 as "node-01" would have every packet
 * discarded with a perfect radio link. The offset is what keeps them apart:
 *
 *   station 1 -> radio 1..20 -> node-01 .. node-20
 *   station 2 -> radio 1..20 -> node-21 .. node-40
 *
 * TB_STATION_NUM is therefore load-bearing for MQTT as well as for the radio.
 * It must match the nodeIdBase used when the station was adopted in the
 * dashboard ((num - 1) * 20); nothing can detect a mismatch, the packets just
 * stop arriving.
 */
#define TB_NODE_ID_OFFSET ((CONFIG_TB_STATION_NUM - 1) * 20)

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
 * the zero-padded string id the backend registered ("node-07"), shifted by
 * TB_NODE_ID_OFFSET so two stations cannot claim the same id. The backend drops
 * topics whose segments it does not recognise and explicitly never invents node
 * ids from binary fields, so this mapping is load-bearing: get the padding or the
 * offset wrong and every packet is silently discarded at the far end.
 *
 * Returns ESP_ERR_INVALID_STATE while the broker is disconnected. There is no
 * queue: a vital that cannot be delivered now is stale by the time the link
 * returns, and the next poll is 15s away.
 */
esp_err_t station_net_publish(uint8_t node_id, const char *leaf,
			      const char *json, int qos, bool retain);

#endif /* STATION_NET_H */
