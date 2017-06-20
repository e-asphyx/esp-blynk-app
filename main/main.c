#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "driver/ledc.h"
#include "driver/adc.h"
#include "esp_system.h"
#include "esp_task.h"
#include "esp_log.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "blynk.h"

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASSWORD
#define BLYNK_TOKEN CONFIG_BLYNK_TOKEN
#define BLYNK_SERVER CONFIG_BLYNK_SERVER

#define ADC_BITS 10
#define ADC_WIDTH(x) ((x) - 9)
#define ADC_CHANNEL 4 /* IO32 */
#define ADC_ATTEN ADC_ATTEN_0db

#define PWM_PIN 12
#define PWM_BITS 10
#define PWM_FREQ 1000
#define PWM_TIMER LEDC_TIMER_0
#define PWM_MODE LEDC_HIGH_SPEED_MODE
#define PWM_CHANNEL LEDC_CHANNEL_0

enum {
	VP_PWM = 0,
	VP_ADC,
	VP_UPTIME,
};

static const char *tag = "blynk-example";

static esp_err_t event_handler(void *arg, system_event_t *event) {
	switch (event->event_id) {
		case SYSTEM_EVENT_STA_START:
		{
			ESP_LOGI(tag, "WiFi started");
			/* Auto connect feature likely not implemented in WiFi lib, so do it manually */
			bool ac;
			ESP_ERROR_CHECK(esp_wifi_get_auto_connect(&ac));
			if (ac) {
				esp_wifi_connect();
			}
			break;
		}

		case SYSTEM_EVENT_STA_CONNECTED:
			/* enable ipv6 */
			tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
			break;

		case SYSTEM_EVENT_STA_GOT_IP:
			break;

		case SYSTEM_EVENT_STA_DISCONNECTED:
			/* This is a workaround as ESP32 WiFi libs don't currently
			   auto-reassociate. */
			esp_wifi_connect();
			break;

		default:
			break;
	}
	return ESP_OK;
}

static void wifi_conn_init() {
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

	// Get config from NVS
	wifi_config_t wifi_config;
	ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &wifi_config));

	if (!wifi_config.sta.ssid[0]) {
		memset(&wifi_config, 0, sizeof(wifi_config));

		strlcpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
		strlcpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

		if (!wifi_config.sta.ssid[0]) {
			return;
		}

		ESP_LOGI(tag, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	}

	ESP_ERROR_CHECK(esp_wifi_start());
}

/* Blynk client state handler */
static void state_handler(blynk_client_t *c, const blynk_state_evt_t *ev, void *data) {
	ESP_LOGI(tag, "state: %d\n", ev->state);
}

/* Virtual write handler */
static void vw_handler(blynk_client_t *c, uint16_t id, const char *cmd, int argc, char **argv, void *data) {
	if (argc > 1 && atoi(argv[0]) == VP_PWM) {
		uint32_t value = atoi(argv[1]);

		/* Update PWM channel */
		ledc_set_duty(PWM_MODE, PWM_CHANNEL, value);
		ledc_update_duty(PWM_MODE, PWM_CHANNEL);
	}
}

/* Virtual read handler */
static void vr_handler(blynk_client_t *c, uint16_t id, const char *cmd, int argc, char **argv, void *data) {
	if (!argc) {
		return;
	}

	int pin = atoi(argv[0]);

	switch (pin) {
		case VP_ADC:
		{
			/* Get ADC value */
			int value = adc1_get_voltage(ADC_CHANNEL);

			/* Respond with `virtual write' command */
			blynk_send(c, BLYNK_CMD_HARDWARE, 0, "sii", "vw", VP_ADC, value);
			break;
		}

		case VP_UPTIME:
		{
			unsigned long value = (unsigned long long)xTaskGetTickCount() * portTICK_RATE_MS / 1000;

			/* Respond with `virtual write' command */
			blynk_send(c, BLYNK_CMD_HARDWARE, 0, "siL", "vw", VP_UPTIME, value);
			break;
		}
	}
}

static void init_adc() {
	adc1_config_width(ADC_WIDTH(ADC_BITS));
	adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);
}

static void init_pwm() {
	ledc_timer_config_t timer = {
		.bit_num = PWM_BITS,
		.freq_hz = PWM_FREQ,
		.speed_mode = PWM_MODE,
		.timer_num = PWM_TIMER,
	};
	ledc_timer_config(&timer);

	ledc_channel_config_t channel = {
		.channel = PWM_CHANNEL,
		.duty = 0,
		.gpio_num = PWM_PIN,
		.speed_mode = PWM_MODE,
		.timer_sel = PWM_TIMER,
	};

	ledc_channel_config(&channel);
}

void app_main() {
	init_adc();
	init_pwm();

	ESP_ERROR_CHECK(nvs_flash_init());
	tcpip_adapter_init();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

	/* Init WiFi */
	wifi_conn_init();

	blynk_client_t *client = malloc(sizeof(blynk_client_t));
	blynk_init(client);

	blynk_options_t opt = {
		.token = BLYNK_TOKEN,
		.server = BLYNK_SERVER,
		/* Use default timeouts */
	};

	blynk_set_options(client, &opt);

	/* Subscribe to state changes and errors */
	blynk_set_state_handler(client, state_handler, NULL);

	/* blynk_set_handler sets hardware (BLYNK_CMD_HARDWARE) command handler */
	blynk_set_handler(client, "vw", vw_handler, NULL);
	blynk_set_handler(client, "vr", vr_handler, NULL);

	/* Start Blynk client task */
	blynk_start(client);
}
