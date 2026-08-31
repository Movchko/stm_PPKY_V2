#include "esp_manager.h"

#include "backend.h"
#include "can_bus.h"
#include "device_config.h"
#include "esp_protocol.h"
#include "menu_ui.h"
#include "main.h"
#include "tick_time.h"

#include <string.h>

/* Ждём activity от ESP, а не фиксированную задержку после EN:
 * ESP boot + uart_bridge обычно >1 с, раньше команда терялась. */
#define ESP_ONLINE_TIMEOUT_MS       3000u
#define ESP_WIFI_RETRY_PERIOD_MS    2000u
#define ESP_WIFI_RETRY_MAX          10u
#define ESP_WIFI_CONNECT_TIMEOUT_MS (5u * 60u * 1000u)
#define ESP_WIFI_ICON_BLINK_MS      500u /* 1 Гц: 500 мс вкл / 500 мс выкл */

extern struct PPKYCfg PPKYConfig;

static uint32_t s_last_activity_ms = 0u;
static uint8_t  s_online = 0u;
static uint8_t  s_wifi_enabled = 0u;
static uint8_t  s_tcp_connected = 0u;
static uint8_t  s_want_wifi = 0u;       /* ППКУ запросило WiFi (меню / конфиг) */
static uint8_t  s_user_wifi_on = 0u;    /* Ручной Вкл/Выкл из меню «Связь» */
static uint8_t  s_config_sent = 0u;
static uint8_t  s_wifi_cmd_sent = 0u;
static uint8_t  s_wifi_retry_cnt = 0u;
static uint32_t s_next_wifi_retry_ms = 0u;
static uint32_t s_wifi_attempt_start_ms = 0u;
static uint8_t  s_prev_tcp_connected = 0u;
static uint16_t s_cmd_seq = 0u;

void EspManager_Init(void)
{
	s_last_activity_ms = 0u;
	s_online = 0u;
	s_wifi_enabled = 0u;
	s_tcp_connected = 0u;
	s_want_wifi = 0u;
	s_user_wifi_on = 0u;
	s_config_sent = 0u;
	s_wifi_cmd_sent = 0u;
	s_wifi_retry_cnt = 0u;
	s_next_wifi_retry_ms = 0u;
	s_wifi_attempt_start_ms = 0u;
	s_prev_tcp_connected = 0u;
}

static void send_esp_cmd(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
	uint8_t body[1u + ESP_CONFIG_PAYLOAD_SIZE];
	uint16_t body_len = 1u;

	if (!Esp32_IsEnabled()) {
		return;
	}

	body[0] = cmd;
	if (payload != NULL && payload_len > 0u) {
		if (payload_len > (uint16_t)(sizeof(body) - 1u)) {
			payload_len = (uint16_t)(sizeof(body) - 1u);
		}
		memcpy(&body[1], payload, payload_len);
		body_len = (uint16_t)(1u + payload_len);
	}

	(void)UartBridge_SendBsuPacket(BSU_PKT_TYPE_ESP_CMD, s_cmd_seq++, body, body_len);
}

static void send_config(void)
{
	EspConfigPayload cfg;

	cfg.ex_can_on = PPKYConfig.ex_can_on;
	cfg.ex_can_protocol = PPKYConfig.ex_can_protocol;
	cfg.wifi_block = PPKYConfig.wifi_block;
	cfg.reserved = 0u;
	cfg.ex_can_baudrate = PPKYConfig.ex_can_baudrate;
	cfg.ex_rs485_baudrate = PPKYConfig.ex_rs485_baudrate;

	send_esp_cmd(ESP_CMD_SET_CONFIG, (const uint8_t *)&cfg, ESP_CONFIG_PAYLOAD_SIZE);
	s_config_sent = 1u;
}

static void send_wifi_enable(void)
{
	send_esp_cmd(ESP_CMD_WIFI_ENABLE, NULL, 0u);
	s_wifi_cmd_sent = 1u;
	s_next_wifi_retry_ms = HAL_GetTick() + ESP_WIFI_RETRY_PERIOD_MS;
}

static void send_wifi_disable(void)
{
	send_esp_cmd(ESP_CMD_WIFI_DISABLE, NULL, 0u);
	s_wifi_cmd_sent = 0u;
	s_wifi_retry_cnt = 0u;
	s_next_wifi_retry_ms = 0u;
}

void EspManager_OnEspPoweredOn(void)
{
	s_config_sent = 0u;
	s_wifi_cmd_sent = 0u;
	s_wifi_retry_cnt = 0u;
	s_next_wifi_retry_ms = 0u;
	s_wifi_attempt_start_ms = 0u;
	s_prev_tcp_connected = 0u;
	s_want_wifi = 0u;
	s_user_wifi_on = 0u;
	/* Конфиг уйдёт после activity; WiFi — только по запросу из меню. */
}

void EspManager_OnEspPoweredOff(void)
{
	if (s_online != 0u) {
		send_wifi_disable();
	}
	s_want_wifi = 0u;
	s_user_wifi_on = 0u;
	s_config_sent = 0u;
	s_wifi_cmd_sent = 0u;
	s_wifi_retry_cnt = 0u;
	s_wifi_attempt_start_ms = 0u;
	s_prev_tcp_connected = 0u;
	s_online = 0u;
	s_wifi_enabled = 0u;
	s_tcp_connected = 0u;
	s_last_activity_ms = 0u;
}

void EspManager_RequestWifiEnable(void)
{
	if (PPKYConfig.wifi_block != 0u) {
		s_want_wifi = 0u;
		s_user_wifi_on = 0u;
		return;
	}
	s_user_wifi_on = 1u;
	s_want_wifi = 1u;
	s_wifi_retry_cnt = 0u;
	s_wifi_attempt_start_ms = HAL_GetTick();
	if (Esp32_IsEnabled() && s_online != 0u) {
		if (s_config_sent == 0u) {
			send_config();
		}
		if (s_wifi_enabled == 0u) {
			send_wifi_enable();
		}
	}
}

void EspManager_RequestWifiDisable(void)
{
	s_user_wifi_on = 0u;
	s_want_wifi = 0u;
	s_wifi_attempt_start_ms = 0u;
	if (Esp32_IsEnabled() && s_online != 0u) {
		send_wifi_disable();
	}
}

void EspManager_Process(uint32_t now_ms)
{
	if (!Esp32_IsEnabled()) {
		return;
	}

	if (s_online != 0u) {
		if (s_config_sent == 0u) {
			send_config();
		}
		if (s_want_wifi != 0u && s_wifi_enabled == 0u) {
			if (s_wifi_cmd_sent == 0u) {
				send_wifi_enable();
			} else if (s_wifi_retry_cnt < ESP_WIFI_RETRY_MAX &&
			           (int32_t)(now_ms - s_next_wifi_retry_ms) >= 0) {
				send_wifi_enable();
				s_wifi_retry_cnt++;
			}
		}
	}

	if (s_want_wifi != 0u && s_tcp_connected == 0u && s_wifi_attempt_start_ms != 0u &&
	    (int32_t)(now_ms - s_wifi_attempt_start_ms) >= (int32_t)ESP_WIFI_CONNECT_TIMEOUT_MS) {
		s_want_wifi = 0u;
		s_user_wifi_on = 0u;
		s_wifi_attempt_start_ms = 0u;
		if (s_online != 0u) {
			send_wifi_disable();
		}
	}

	if (s_last_activity_ms != 0u &&
	    TickAgeExpiredMs(now_ms, s_last_activity_ms, ESP_ONLINE_TIMEOUT_MS) != 0u) {
		s_online = 0u;
		s_wifi_enabled = 0u;
		s_tcp_connected = 0u;
		/* После пропадания связи — снова ждать activity и повторить команды. */
		s_config_sent = 0u;
		s_wifi_cmd_sent = 0u;
		s_wifi_retry_cnt = 0u;
		s_prev_tcp_connected = 0u;
	}
}

void EspManager_OnActivity(const uint8_t *payload, uint16_t len)
{
	EspActivityPayload act;

	if (payload == NULL || len < ESP_ACTIVITY_PAYLOAD_SIZE) {
		return;
	}

	memcpy(&act, payload, ESP_ACTIVITY_PAYLOAD_SIZE);
	s_last_activity_ms = HAL_GetTick();
	s_online = 1u;
	s_wifi_enabled = (act.wifi_enabled != 0u) ? 1u : 0u;
	s_tcp_connected = (act.tcp_connected != 0u) ? 1u : 0u;

	if (s_wifi_enabled != 0u) {
		s_wifi_retry_cnt = ESP_WIFI_RETRY_MAX; /* успех — больше не долбить */
	}

	if (s_tcp_connected != 0u) {
		s_wifi_attempt_start_ms = 0u;
	} else if (s_prev_tcp_connected != 0u && s_want_wifi != 0u) {
		s_wifi_attempt_start_ms = s_last_activity_ms;
	}
	s_prev_tcp_connected = s_tcp_connected;
}

uint8_t EspManager_IsOnline(void)
{
	return s_online;
}

uint8_t EspManager_IsWifiEnabled(void)
{
	return s_wifi_enabled;
}

uint8_t EspManager_IsHostConnected(void)
{
	return s_tcp_connected;
}

uint8_t EspManager_IsLinkActive(void)
{
	return (uint8_t)(Esp32_IsEnabled() && s_online != 0u && s_tcp_connected != 0u);
}

uint8_t EspManager_IsUserWifiOn(void)
{
	return s_user_wifi_on;
}

uint8_t EspManager_IsWifiSessionActive(void)
{
	return (uint8_t)(s_user_wifi_on != 0u || s_tcp_connected != 0u);
}

uint8_t EspManager_IsWifiIconVisible(uint32_t now_ms)
{
	if (!EspManager_IsWifiSessionActive()) {
		return 0u;
	}
	if (EspManager_IsLinkActive() != 0u) {
		return 1u;
	}
	return (uint8_t)(((now_ms / ESP_WIFI_ICON_BLINK_MS) & 1u) == 0u);
}
