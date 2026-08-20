#include "station_net.h"

#include <stdio.h> /* snprintf */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
/* IDF v6 moved the SPI Ethernet drivers out of esp_eth into the managed
 * component espressif/w5500, which is where these two headers now come from.
 * On v5 they were esp_eth_mac.h / esp_eth_phy.h inside esp_eth. */
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tb_station_pins.h"

static const char *TAG = "net";

static esp_mqtt_client_handle_t s_mqtt;
static volatile bool s_mqtt_up;
static char s_lwt_topic[64];

/*
 * The station status topic carries JSON, NOT the bare strings ONLINE/OFFLINE.
 * Confirmed against the backend's frozen api-contract.md 2026-08-18, which
 * specifies {"status":"ONLINE"} and a retained LWT of {"status":"OFFLINE"}.
 * This code published bare strings until then; a bare string parses as neither
 * an object nor an error on most JSON parsers' fast paths, so the symptom would
 * have been a station that connects happily and never appears online.
 */
#define TB_STATUS_ONLINE_JSON "{\"status\":\"ONLINE\"}"
#define TB_STATUS_OFFLINE_JSON "{\"status\":\"OFFLINE\"}"

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id,
			       void *data)
{
	(void) arg;
	(void) base;

	switch ((esp_mqtt_event_id_t) id) {
	case MQTT_EVENT_CONNECTED:
		s_mqtt_up = true;
		ESP_LOGI(TAG, "mqtt connected to %s", CONFIG_TB_MQTT_URI);
		/* Retained ONLINE overwrites the broker's retained LWT. Same
		 * topic, same retain flag, so a late dashboard subscriber sees
		 * the current state instead of a stale OFFLINE. */
		esp_mqtt_client_publish(s_mqtt, s_lwt_topic,
					TB_STATUS_ONLINE_JSON, 0, 1, 1);
		break;
	case MQTT_EVENT_DISCONNECTED:
		s_mqtt_up = false;
		ESP_LOGW(TAG, "mqtt disconnected");
		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGE(TAG, "mqtt error");
		break;
	default:
		break;
	}
}

static void mqtt_start(void)
{
	if (s_mqtt != NULL) {
		return; /* link bounced; the client reconnects on its own */
	}

	snprintf(s_lwt_topic, sizeof(s_lwt_topic), "triagebox/%s/status",
		 CONFIG_TB_STATION_ID);

	/* The LWT is the whole point of the retained status topic: if this
	 * station loses power, the broker publishes OFFLINE on its behalf.
	 * QoS 1 + retain so it survives and reaches late subscribers. */
	esp_mqtt_client_config_t cfg = {
		.broker.address.uri = CONFIG_TB_MQTT_URI,
		.credentials.client_id = CONFIG_TB_STATION_ID,
		.credentials.username = CONFIG_TB_MQTT_USERNAME,
		.credentials.authentication.password = CONFIG_TB_MQTT_PASSWORD,
		.session.last_will.topic = s_lwt_topic,
		.session.last_will.msg = TB_STATUS_OFFLINE_JSON,
		.session.last_will.qos = 1,
		.session.last_will.retain = 1,
	};

	s_mqtt = esp_mqtt_client_init(&cfg);
	if (s_mqtt == NULL) {
		ESP_LOGE(TAG, "mqtt client init failed");
		return;
	}
	ESP_ERROR_CHECK(esp_mqtt_client_register_event(
		s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
	ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));
}

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id,
			      void *data)
{
	(void) arg;
	(void) base;
	(void) data;

	switch (id) {
	case ETHERNET_EVENT_CONNECTED:
		ESP_LOGI(TAG, "eth link up");
#if CONFIG_TB_ETH_STATIC_IP
		/* Redundant, kept as a belt: esp_netif turns out to post
		 * IP_EVENT_ETH_GOT_IP for a static address too, so got_ip_handler
		 * has usually run by now. mqtt_start() returns early when the
		 * client already exists, which is what makes this safe -- do not
		 * remove that guard. */
		mqtt_start();
#endif
		break;
	case ETHERNET_EVENT_DISCONNECTED:
		ESP_LOGW(TAG, "eth link down");
		s_mqtt_up = false;
		break;
	default:
		break;
	}
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id,
			   void *data)
{
	(void) arg;
	(void) base;
	(void) id;

	const ip_event_got_ip_t *e = (const ip_event_got_ip_t *) data;

	ESP_LOGI(TAG, "eth up " IPSTR " gw " IPSTR, IP2STR(&e->ip_info.ip),
		 IP2STR(&e->ip_info.gw));
	mqtt_start();
}

/* The IDF W5500 driver has no reset pin field, so the hard reset is ours. The
 * datasheet wants PWDN/RSTn low for at least 500us and the chip needs ~50ms
 * before SPI is answered; the delays below are the coarse tick-rounded version
 * of that, which is free at boot. */
static void w5500_hard_reset(void)
{
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << TB_ETH_PIN_RST,
		.mode = GPIO_MODE_OUTPUT,
	};

	ESP_ERROR_CHECK(gpio_config(&io));
	ESP_ERROR_CHECK(gpio_set_level(TB_ETH_PIN_RST, 0));
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_ERROR_CHECK(gpio_set_level(TB_ETH_PIN_RST, 1));
	vTaskDelay(pdMS_TO_TICKS(60));
}

esp_err_t station_net_start(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	w5500_hard_reset();

	spi_bus_config_t bus = {
		.mosi_io_num = TB_ETH_PIN_MOSI,
		.miso_io_num = TB_ETH_PIN_MISO,
		.sclk_io_num = TB_ETH_PIN_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};

	ESP_ERROR_CHECK(spi_bus_initialize(TB_ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

	spi_device_interface_config_t dev = {
		.command_bits = 16, /* W5500 frame: 16b address ... */
		.address_bits = 8,  /* ... then the control byte */
		.mode = 0,
		.clock_speed_hz = TB_ETH_SPI_CLOCK_HZ,
		.spics_io_num = TB_ETH_PIN_CS,
		.queue_size = 20,
	};

	eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(TB_ETH_SPI_HOST, &dev);

	/*
	 * ponytail: polling, not the INT pin. INTn is wired to GPIO4 and the code
	 * for it is right below, but the interrupt path has never once been proven
	 * to work on this board, and its failure mode is invisible: link state is
	 * polled from PHYCFGR by esp_eth's own timer and comes up fine, while
	 * received frames are collected ONLY on INTn -- so DHCP OFFERs rot unread
	 * and the log reads like a network problem. Polling removes that variable
	 * entirely. Costs one SPI status read every 10ms; at 8MHz that is noise.
	 * Set TB_ETH_USE_INT to 1 to go back once the INT wire is trusted.
	 */
#define TB_ETH_USE_INT 0

#if TB_ETH_USE_INT
	w5500.int_gpio_num = TB_ETH_PIN_INT;

	/* NOT NEEDED, kept only as a note: emac_w5500_init() in the managed
	 * component already calls gpio_pullup_en() on int_gpio_num. INTn is
	 * open-drain and GPIO4's IOMUX reset default is a pull-DOWN, so a
	 * pull-up is genuinely required -- the driver just gets there first.
	 * Configuring it here was a no-op, which is why doing so changed
	 * nothing when the missing DHCP lease was blamed on it. */
#else
	w5500.int_gpio_num = -1;
	w5500.poll_period_ms = 10;
#endif

	/* Logged so a boot log proves which path is compiled in -- both modes
	 * are otherwise indistinguishable until an IP appears, which is exactly
	 * the thing being debugged. */
	ESP_LOGI(TAG, "w5500 rx: %s", TB_ETH_USE_INT ? "int gpio" : "poll 10ms");

	eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
	eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();

	/* The W5500 has no PHY reset strap and no MDIO address, and its PHY does
	 * NOT implement IEEE 802.3 auto-negotiation. autonego_timeout_ms must be
	 * 0 or the generic 802.3 PHY layer waits for a negotiation that can
	 * never complete and the link is never reported up -- which looks
	 * exactly like an unplugged cable, because no event is emitted at all. */
	phy_cfg.phy_addr = 1;
	phy_cfg.reset_gpio_num = -1;
	phy_cfg.autonego_timeout_ms = 0;

	esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
	esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
	if (mac == NULL || phy == NULL) {
		ESP_LOGE(TAG, "w5500 mac/phy alloc failed");
		return ESP_FAIL;
	}

	esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
	esp_eth_handle_t eth = NULL;

	/* MUST precede esp_eth_driver_install(). The W5500 driver registers its
	 * INTn handler with gpio_isr_handler_add(), which does NOT install the
	 * shared ISR service itself -- and its failure is logged, not returned,
	 * so the install call still succeeds. The symptom is a station that
	 * boots clean, prints its MAC, and then never reports link up or gets an
	 * IP, because with int_gpio_num set the driver relies on the interrupt
	 * instead of a link-poll timer. ESP_ERR_INVALID_STATE just means someone
	 * already installed it. */
	esp_err_t isr = gpio_install_isr_service(0);

	if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(isr));
		return isr;
	}

	ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));

	/* The W5500 ships with no MAC of its own -- unset, it would transmit
	 * from 00:00:00:00:00:00 and the switch would drop it. Derive one from
	 * the ESP32's factory base so it is stable across reflashes and unique
	 * per board. */
	uint8_t macaddr[6];

	ESP_ERROR_CHECK(esp_read_mac(macaddr, ESP_MAC_ETH));
	ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, macaddr));

	esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
	esp_netif_t *netif = esp_netif_new(&netif_cfg);

#if CONFIG_TB_ETH_STATIC_IP
	/* The DHCP client is on by default for an Ethernet netif and must be
	 * stopped BEFORE the address is set, or it overwrites it on link up.
	 * Before esp_netif_attach() it is still in its init state, which
	 * reports ALREADY_STOPPED -- that is success, not a failure. */
	esp_err_t dhcpc = esp_netif_dhcpc_stop(netif);

	if (dhcpc != ESP_OK && dhcpc != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
		ESP_LOGE(TAG, "dhcpc stop: %s", esp_err_to_name(dhcpc));
		return dhcpc;
	}

	esp_netif_ip_info_t ip = {
		.ip.addr = esp_ip4addr_aton(CONFIG_TB_ETH_IP),
		.netmask.addr = esp_ip4addr_aton(CONFIG_TB_ETH_NETMASK),
		.gw.addr = esp_ip4addr_aton(CONFIG_TB_ETH_GW),
	};

	ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));
	ESP_LOGW(TAG, "static ip %s gw %s (dhcp off)", CONFIG_TB_ETH_IP,
		 CONFIG_TB_ETH_GW);
#endif

	ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));

	ESP_ERROR_CHECK(esp_event_handler_register(
		ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(
		IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));

	return esp_eth_start(eth);
}

bool station_net_mqtt_connected(void)
{
	return s_mqtt_up;
}

esp_err_t station_net_publish(uint8_t node_id, const char *leaf,
			      const char *json, int qos, bool retain)
{
	if (leaf == NULL || json == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s_mqtt == NULL || !s_mqtt_up) {
		return ESP_ERR_INVALID_STATE;
	}

	char topic[80];

	/* "node-%02u", zero-padded to two digits -- see station_net.h. */
	snprintf(topic, sizeof(topic), "triagebox/%s/node-%02u/%s",
		 CONFIG_TB_STATION_ID, node_id, leaf);

	/* esp_mqtt_client_publish() copies the payload into the outbox before it
	 * returns, so the caller's buffer may be a stack local. A negative
	 * return is a full outbox, which at one message per node per 15s means
	 * the link is down in a way MQTT_EVENT_DISCONNECTED has not reported
	 * yet -- worth surfacing, not worth retrying here. */
	int id = esp_mqtt_client_publish(s_mqtt, topic, json, 0, qos,
					 retain ? 1 : 0);

	if (id < 0) {
		ESP_LOGW(TAG, "publish %s failed", topic);
		return ESP_FAIL;
	}
	return ESP_OK;
}
