#include "menu_ui.h"

#include "main.h"
#include "backend.h"
#include "can_bus.h"
#include "event_log.h"
#include "esp_manager.h"
#include "beeper.h"

#define MENU_CFG_SUCCESS_HOLD_MS 5000u

/* Объявлено в config_sync.hpp; без include C++-заголовка в .c */
extern uint8_t ConfigSync_GetApplyPercent(void);

static uint8_t g_config_session_active = 0u;
static uint8_t g_main_screen_active = 0u;
static uint8_t g_esp32_enabled = 0u;
static int16_t g_menu_index = 0;

static MenuCfgState g_cfg_state = MENU_CFG_STATE_IDLE;
static uint16_t g_cfg_words_max = 0u;
static uint16_t g_cfg_total_words = 0u;
static uint32_t g_cfg_success_from_ms = 0u;
static uint8_t g_cfg_success_beep_done = 0u;

static uint8_t g_mcu_detail_slot = 0u;

/* Remote-controlled menu selection.
 * On master side it is used as part of UI state machine; panel reads it
 * (via MENU_LIST frames) to highlight. */
static uint16_t g_menu_selected = 0u;
static uint8_t g_menu_n_items = 7u;

void MenuUi_SetConfigSession(uint8_t active)
{
	g_config_session_active = active ? 1u : 0u;
	if (!g_config_session_active) {
		g_cfg_state = MENU_CFG_STATE_IDLE;
		g_cfg_words_max = 0u;
		g_cfg_success_from_ms = 0u;
		g_cfg_success_beep_done = 0u;
	}
}

uint8_t MenuUi_IsConfigSessionActive(void)
{
	return g_config_session_active;
}

uint8_t MenuUi_IsConfigOverlayActive(void)
{
	return (g_config_session_active != 0u &&
		g_cfg_state != MENU_CFG_STATE_IDLE) ? 1u : 0u;
}

void MenuUi_SetMainScreenActive(uint8_t active)
{
	g_main_screen_active = active ? 1u : 0u;
}

uint8_t MenuUi_IsMainScreenActive(void)
{
	return g_main_screen_active;
}

void MenuUi_SetMenuIndex(int16_t index)
{
	if (index < 0) {
		index = 0;
	}
	g_menu_index = index;
}

int16_t MenuUi_GetMenuIndex(void)
{
	return g_menu_index;
}

void MenuUi_ResetMenuIndex(void)
{
	g_menu_index = 0;
}

void Esp32_SetEnabled(uint8_t enabled)
{
	if (enabled != 0u) {
		if (g_esp32_enabled != 0u) {
			return;
		}
		HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port, ESP32_BOOT_Pin, GPIO_PIN_SET);
		HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);
		HAL_Delay(100);
		g_esp32_enabled = 1u;
		EspManager_OnEspPoweredOn();
	} else {
		EspManager_OnEspPoweredOff();
		UartBridge_Stop();
		HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);
		HAL_GPIO_WritePin(ESP32_BOOT_GPIO_Port, ESP32_BOOT_Pin, GPIO_PIN_RESET);
		g_esp32_enabled = 0u;
		EventLog_HostLinkSessionReset(0u); /* WiFi: разрешить новый HOST_LINK */
	}
}

uint8_t Esp32_IsEnabled(void)
{
	return g_esp32_enabled;
}

void MenuConfig_Reset(void)
{
	g_cfg_state = MENU_CFG_STATE_RECEIVING;
	g_cfg_words_max = 0u;
	g_cfg_success_from_ms = 0u;
	g_cfg_success_beep_done = 0u;
	g_cfg_total_words = (uint16_t)(GetConfigSize() / 4u);
	if (g_cfg_total_words == 0u) {
		g_cfg_total_words = 1u;
	}
}

void MenuConfig_OnWordReceived(uint16_t word_num)
{
	if (!g_config_session_active) {
		return;
	}
	uint16_t next = (uint16_t)(word_num + 1u);
	if (next > g_cfg_words_max) {
		g_cfg_words_max = next;
	}
	if (g_cfg_state == MENU_CFG_STATE_IDLE) {
		g_cfg_state = MENU_CFG_STATE_RECEIVING;
	}
}

void MenuConfig_OnSaveCompleted(void)
{
	(void)0;
}

void MenuConfig_OnApplyStarted(void)
{
	if (!g_config_session_active) {
		return;
	}
	g_cfg_state = MENU_CFG_STATE_APPLYING;
}

void MenuConfig_OnApplySuccess(void)
{
	if (!g_config_session_active) {
		return;
	}
	g_cfg_state = MENU_CFG_STATE_SUCCESS;
	g_cfg_success_from_ms = HAL_GetTick();
	g_cfg_success_beep_done = 0u;
}

void MenuConfig_Process1ms(uint32_t now_ms)
{
	if (!g_config_session_active || g_cfg_state != MENU_CFG_STATE_SUCCESS) {
		return;
	}
	if (g_cfg_success_beep_done == 0u) {
		Beeper_PlayConfigSuccess();
		g_cfg_success_beep_done = 1u;
		/* После блокирующего писка брать актуальный tick. */
		now_ms = HAL_GetTick();
	}
	if (g_cfg_success_from_ms != 0u &&
	    (int32_t)(now_ms - g_cfg_success_from_ms) >= (int32_t)MENU_CFG_SUCCESS_HOLD_MS) {
		MenuUi_SetConfigSession(0u);
	}
}

MenuCfgState MenuConfig_GetState(void)
{
	return g_cfg_state;
}

uint8_t MenuConfig_GetPercent(void)
{
	if (g_cfg_state == MENU_CFG_STATE_APPLYING) {
		return ConfigSync_GetApplyPercent();
	}
	if (g_cfg_total_words == 0u) {
		return 0u;
	}
	uint32_t pct = ((uint32_t)g_cfg_words_max * 100u) / (uint32_t)g_cfg_total_words;
	return (pct > 100u) ? 100u : (uint8_t)pct;
}

void MenuUi_SetMcuDetailSlot(uint8_t cfg_slot)
{
	g_mcu_detail_slot = cfg_slot;
}

uint8_t MenuUi_GetMcuDetailSlot(void)
{
	return g_mcu_detail_slot;
}

void MenuUi_SetMenuSelected(uint16_t selected)
{
	g_menu_selected = selected;
}

uint16_t MenuUi_GetMenuSelected(void)
{
	return g_menu_selected;
}

void MenuUi_SetMenuNItems(uint8_t n_items)
{
	g_menu_n_items = n_items;
}

uint8_t MenuUi_GetMenuNItems(void)
{
	return g_menu_n_items;
}
