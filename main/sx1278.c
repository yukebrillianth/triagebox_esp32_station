#include "sx1278.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "tb_station_pins.h"

static const char *TAG = "sx1278";

/* Registers. Named as in the SX1276-8 datasheet and as in the node's LoRa.h so
 * the two can be diffed by eye. Only the ones this driver touches. */
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FR_MSB 0x06
#define REG_FR_MID 0x07
#define REG_FR_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_OCP 0x0B
#define REG_LNA 0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE 0x0E
#define REG_FIFO_RX_BASE 0x0F
#define REG_FIFO_RX_CURRENT 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_MODEM_CONFIG1 0x1D
#define REG_MODEM_CONFIG2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MODEM_CONFIG3 0x26
#define REG_SYNC_WORD 0x39
#define REG_DIO_MAPPING1 0x40
#define REG_VERSION 0x42

/* RegOpMode. LONG_RANGE_MODE is bit 7 and can only be changed in sleep, which
 * is why init drops to sleep before setting it. */
#define MODE_LONG_RANGE 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05

/* RegIrqFlags */
#define IRQ_RX_TIMEOUT 0x80
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08

/* SX1278 fixed-point frequency step: 32MHz / 2^19. 433MHz / FSTEP does not
 * divide evenly, so this is computed in 64-bit and truncated exactly the way
 * every other driver does it -- the residual error is far below the modem's
 * tolerance at BW125. */
#define SX1278_FREQ_HZ 433000000ULL
#define SX1278_FRF ((uint32_t) ((SX1278_FREQ_HZ << 19) / 32000000ULL))

/* ponytail: 8MHz, matching the W5500 for the same reason -- breadboard jumpers.
 * The SX1278 tolerates 10MHz+, and this bus IS on the SPI3 IOMUX defaults so it
 * takes the fast path, but a marginal SPI clock on a radio presents as
 * "occasionally the wrong register value", which is indistinguishable from a
 * protocol bug. Raise on a PCB, not before. */
#define SX1278_SPI_HZ (8 * 1000 * 1000)

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_dio0; /* given by the DIO0 ISR, taken by tx/rx */

static uint8_t reg_read(uint8_t addr)
{
	uint8_t tx[2] = { addr & 0x7F, 0x00 };
	uint8_t rx[2] = { 0, 0 };
	spi_transaction_t t = {
		.length = 16,
		.tx_buffer = tx,
		.rx_buffer = rx,
	};

	ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
	return rx[1];
}

static void reg_write(uint8_t addr, uint8_t val)
{
	uint8_t tx[2] = { addr | 0x80, val };
	spi_transaction_t t = {
		.length = 16,
		.tx_buffer = tx,
	};

	ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/* FIFO burst. One transaction with CS held down for the whole payload, which is
 * how the SX1278 auto-increments its FIFO pointer -- a byte-at-a-time loop with
 * CS toggling between bytes would rewrite address 0x00 every time. */
static void fifo_write(const uint8_t *data, uint8_t len)
{
	uint8_t addr = REG_FIFO | 0x80;
	spi_transaction_t t = {
		.length = 8 * (1 + len),
		.tx_buffer = NULL,
	};
	uint8_t buf[1 + 255];

	buf[0] = addr;
	memcpy(&buf[1], data, len);
	t.tx_buffer = buf;
	ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void fifo_read(uint8_t *data, uint8_t len)
{
	uint8_t txbuf[1 + 255] = { REG_FIFO & 0x7F };
	uint8_t rxbuf[1 + 255];
	spi_transaction_t t = {
		.length = 8 * (1 + len),
		.tx_buffer = txbuf,
		.rx_buffer = rxbuf,
	};

	ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
	memcpy(data, &rxbuf[1], len);
}

static void IRAM_ATTR dio0_isr(void *arg)
{
	(void) arg;
	xSemaphoreGiveFromISR(s_dio0, NULL);
}

static void radio_reset(void)
{
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << TB_LORA_PIN_RST,
		.mode = GPIO_MODE_OUTPUT,
	};

	ESP_ERROR_CHECK(gpio_config(&io));
	/* Datasheet 7.2.2: NRESET low for >100us, then 5ms before the chip is
	 * addressable. The node's library uses 1ms/100ms; matching its
	 * generosity costs nothing at boot and removes a variable. */
	ESP_ERROR_CHECK(gpio_set_level(TB_LORA_PIN_RST, 0));
	vTaskDelay(pdMS_TO_TICKS(2));
	ESP_ERROR_CHECK(gpio_set_level(TB_LORA_PIN_RST, 1));
	vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t sx1278_init(void)
{
	spi_bus_config_t bus = {
		.mosi_io_num = TB_LORA_PIN_MOSI,
		.miso_io_num = TB_LORA_PIN_MISO,
		.sclk_io_num = TB_LORA_PIN_SCK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
	};

	ESP_ERROR_CHECK(spi_bus_initialize(TB_LORA_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

	spi_device_interface_config_t dev = {
		.mode = 0, /* CPOL=0 CPHA=0, the SX1278's only SPI mode */
		.clock_speed_hz = SX1278_SPI_HZ,
		.spics_io_num = TB_LORA_PIN_CS,
		.queue_size = 4,
	};

	ESP_ERROR_CHECK(spi_bus_add_device(TB_LORA_SPI_HOST, &dev, &s_spi));

	radio_reset();

	/* RegVersion reads 0x12 on every SX1276/77/78. Checking it here is the
	 * difference between "the radio is not wired up" and three hours of
	 * wondering why no packets arrive: every other symptom of a broken SPI
	 * link on this chip is silence, which is also what a wrong sync word,
	 * wrong SF and wrong frequency all look like. */
	uint8_t version = reg_read(REG_VERSION);

	if (version != 0x12) {
		ESP_LOGE(TAG, "RegVersion 0x%02x, expected 0x12 -- check SPI3 "
			      "wiring (SCK%d MISO%d MOSI%d CS%d RST%d)",
			 version, TB_LORA_PIN_SCK, TB_LORA_PIN_MISO,
			 TB_LORA_PIN_MOSI, TB_LORA_PIN_CS, TB_LORA_PIN_RST);
		return ESP_ERR_NOT_FOUND;
	}

	/* LoRa mode is only settable from sleep (datasheet 4.1.1). */
	reg_write(REG_OP_MODE, MODE_SLEEP);
	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_SLEEP);
	vTaskDelay(pdMS_TO_TICKS(10));
	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);

	reg_write(REG_FR_MSB, (uint8_t) (SX1278_FRF >> 16));
	reg_write(REG_FR_MID, (uint8_t) (SX1278_FRF >> 8));
	reg_write(REG_FR_LSB, (uint8_t) SX1278_FRF);

	/* ModemConfig1 = BW125 (0x7<<4) | CR4/5 (0x1<<1) | explicit header (0).
	 * ModemConfig2 = SF7 (0x7<<4) | CRC on (0x04). Both must equal what the
	 * node's library writes; these are the two registers to dump first if
	 * the link is silent. */
	reg_write(REG_MODEM_CONFIG1, (0x7 << 4) | (0x1 << 1));
	reg_write(REG_MODEM_CONFIG2, (0x7 << 4) | 0x04);

	/* LowDataRateOptimize off: it is required only when symbol time exceeds
	 * 16ms, and SF7/BW125 is ~1ms. The node's setAutoLDO() computes the same
	 * answer for these settings. AGC on. */
	reg_write(REG_MODEM_CONFIG3, 0x04);

	reg_write(REG_PREAMBLE_MSB, 0x00);
	reg_write(REG_PREAMBLE_LSB, 0x08);

	/* Sync word: NOT written. 0x12 is the reset default and the node never
	 * writes it either, so both ends agree by doing nothing. Writing 0x34
	 * here (the LoRaWAN value) is the classic way to make two radios that
	 * look correctly configured never hear each other. */

	/* PA_BOOST at 20dBm and OCP 100mA, matching the node's POWER_20db /
	 * overCurrentProtection = 100. 20dBm on PA_BOOST officially requires
	 * RegPaDac high-power mode and a duty limit; the node has run this way
	 * since the start, so the station matches it rather than introducing an
	 * asymmetric link budget. */
	reg_write(REG_PA_CONFIG, 0xFF);
	reg_write(REG_OCP, 0x20 | 11); /* 100mA: (I-45)/5 = 11 */

	reg_write(REG_LNA, 0x23); /* max gain, LNA boost on */

	reg_write(REG_FIFO_TX_BASE, 0x00);
	reg_write(REG_FIFO_RX_BASE, 0x00);

	s_dio0 = xSemaphoreCreateBinary();
	if (s_dio0 == NULL) {
		return ESP_ERR_NO_MEM;
	}

	gpio_config_t dio = {
		.pin_bit_mask = 1ULL << TB_LORA_PIN_DIO0,
		.mode = GPIO_MODE_INPUT,
		.intr_type = GPIO_INTR_POSEDGE, /* DIO0 is active HIGH */
	};

	ESP_ERROR_CHECK(gpio_config(&dio));

	/* Same trap the W5500 driver fell into: gpio_isr_handler_add() does not
	 * install the shared service and its absence is only logged, so the
	 * handler silently never fires. ESP_ERR_INVALID_STATE means station_net
	 * already installed it, which is the normal case. */
	esp_err_t isr = gpio_install_isr_service(0);

	if (isr != ESP_OK && isr != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "gpio isr service: %s", esp_err_to_name(isr));
		return isr;
	}
	ESP_ERROR_CHECK(gpio_isr_handler_add(TB_LORA_PIN_DIO0, dio0_isr, NULL));

	ESP_LOGI(TAG, "sx1278 up: 433MHz SF7 BW125 CR4/5 pre8 crc sync0x12");
	return ESP_OK;
}

esp_err_t sx1278_transmit(const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
	if (data == NULL || len == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
	reg_write(REG_DIO_MAPPING1, 0x40); /* DIO0 = TxDone */
	reg_write(REG_IRQ_FLAGS, 0xFF); /* write-1-to-clear, all of them */
	reg_write(REG_FIFO_ADDR_PTR, 0x00);
	fifo_write(data, len);
	reg_write(REG_PAYLOAD_LENGTH, len);

	/* Drain a stale give: a DIO0 edge from the previous receive could
	 * otherwise satisfy this wait instantly and report TxDone before the
	 * packet has left. */
	xSemaphoreTake(s_dio0, 0);

	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_TX);

	if (xSemaphoreTake(s_dio0, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
		/* The radio is now stuck in TX. Force standby or the next
		 * receive silently never starts. */
		reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
		ESP_LOGW(TAG, "tx timeout (%u bytes)", len);
		return ESP_ERR_TIMEOUT;
	}

	reg_write(REG_IRQ_FLAGS, IRQ_TX_DONE);
	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
	return ESP_OK;
}

esp_err_t sx1278_receive(uint8_t *buf, uint8_t cap, uint8_t *len_out,
			 uint32_t timeout_ms, int *rssi, float *snr)
{
	if (buf == NULL || len_out == NULL || cap == 0) {
		return ESP_ERR_INVALID_ARG;
	}
	*len_out = 0;

	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
	reg_write(REG_DIO_MAPPING1, 0x00); /* DIO0 = RxDone */
	reg_write(REG_IRQ_FLAGS, 0xFF);
	reg_write(REG_FIFO_ADDR_PTR, 0x00);
	xSemaphoreTake(s_dio0, 0);

	/* RX_CONTINUOUS, not RX_SINGLE, even though exactly one reply is
	 * expected: RX_SINGLE times out on its own schedule via RegSymbTimeout
	 * and then drops to standby, so a reply that starts late is missed
	 * entirely. Continuous mode lets the caller own the deadline. */
	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_RX_CONTINUOUS);

	if (xSemaphoreTake(s_dio0, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
		reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
		return ESP_ERR_TIMEOUT; /* nobody answered; not an error */
	}

	uint8_t flags = reg_read(REG_IRQ_FLAGS);

	/* CRC first: on a corrupt frame RegRxNbBytes and the FIFO contents are
	 * meaningless, so there is nothing to salvage by reading them. */
	if (flags & IRQ_CRC_ERROR) {
		reg_write(REG_IRQ_FLAGS, 0xFF);
		reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
		return ESP_ERR_INVALID_CRC;
	}

	uint8_t len = reg_read(REG_RX_NB_BYTES);

	if (len > cap) {
		/* Rejected, not truncated: a half-read packet would fail
		 * lora_vital_valid() anyway, and truncating hides the fact that
		 * the two ends disagree about the wire format. */
		ESP_LOGW(TAG, "rx %u bytes > cap %u, dropped", len, cap);
		reg_write(REG_IRQ_FLAGS, 0xFF);
		reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
		return ESP_ERR_INVALID_SIZE;
	}

	/* In continuous mode the radio picks its own FIFO offset per packet, so
	 * the read must start at RegFifoRxCurrentAddr -- assuming 0x00 works
	 * for the first packet only and then quietly returns garbage. */
	reg_write(REG_FIFO_ADDR_PTR, reg_read(REG_FIFO_RX_CURRENT));
	fifo_read(buf, len);
	*len_out = len;

	if (rssi != NULL) {
		/* -164 dBm offset for the LF port (below 525MHz); the HF port
		 * uses -157. Wrong constant = every RSSI off by 7dB, which is
		 * plausible-looking and therefore hard to notice. */
		*rssi = (int) reg_read(REG_PKT_RSSI_VALUE) - 164;
	}
	if (snr != NULL) {
		*snr = (float) ((int8_t) reg_read(REG_PKT_SNR_VALUE)) * 0.25f;
	}

	reg_write(REG_IRQ_FLAGS, 0xFF);
	reg_write(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
	return ESP_OK;
}
