#include "config_zone_block.h"
#include "device_config.h"
#include "gost_mode.h"
#include "fire.h"
#include "event_log.h"

extern PPKYCfg PPKYConfig;

#define ZONE_MODE_SRC_MENU  0u
#define ZONE_MODE_SRC_CAN   1u

uint8_t PPKY_ZoneFireModeGet(uint8_t zone_idx)
{
	if (zone_idx >= ZONE_NUMBER) {
		return 0u;
	}
	uint8_t m = PPKYConfig.zone_fire_mode[zone_idx];
	if (m > 4u) {
		m = 0u;
	}
	return m;
}

void PPKY_ZoneFireModeSet(uint8_t zone_idx, uint8_t mode)
{
	if (zone_idx >= ZONE_NUMBER) {
		return;
	}
	if (mode > 4u) {
		mode = 0u;
	}
	/* Меню/API: автономный (1) не выставляем. */
	if (mode == 1u) {
		mode = 0u;
	}
	if (PPKYConfig.zone_fire_mode[zone_idx] == mode) {
		return;
	}
	PPKYConfig.zone_fire_mode[zone_idx] = mode;
	EventLog_LogFireModeChange((uint8_t)(zone_idx + 1u), mode, ZONE_MODE_SRC_MENU);
	Fire_NotifyZoneModeChanged();
}

uint8_t PPKY_ZoneEffectiveMode(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return 0u;
	}
	return PPKY_ZoneFireModeGet((uint8_t)(zone_can - 1u));
}

uint8_t PPKY_ZoneLaunchBlockedByCanZone(uint8_t zone_can)
{
	uint8_t m;
	if (zone_can == 0u) {
		return 0u;
	}
	m = PPKY_ZoneEffectiveMode(zone_can);
	return (m == 3u || m == 4u) ? 1u : 0u;
}

uint8_t PPKY_ZoneFireBlockedByCanZone(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return 0u;
	}
	return (PPKY_ZoneEffectiveMode(zone_can) == 4u) ? 1u : 0u;
}

uint8_t PPKY_ZoneIsManualByCanZone(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return 0u;
	}
	return (PPKY_ZoneEffectiveMode(zone_can) == 2u) ? 1u : 0u;
}

uint8_t PPKY_AnyZoneManualOrBlocked(void)
{
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (PPKYConfig.zone_name[zi][0] == 0) {
			continue;
		}
		uint8_t m = PPKY_ZoneFireModeGet(zi);
		if (m == 2u || m == 3u || m == 4u) {
			return 1u;
		}
	}
	return 0u;
}

void PPKY_ZoneFireModeInitFromGlobal(void)
{
	uint8_t fm = PPKYConfig.fire_mode;
	if (fm > 4u) {
		fm = 0u;
	}
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		PPKYConfig.zone_fire_mode[zi] = fm;
	}
	Fire_NotifyZoneModeChanged();
}

void PPKY_ZoneFireModeSetAllNamed(uint8_t mode)
{
	/* cmd13 / концевик: только 0/1/2 (не блок пуска/пожара). */
	uint8_t m = mode;
	uint8_t any = 0u;

	if (m > 2u) {
		return;
	}
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (PPKYConfig.zone_name[zi][0] == 0) {
			continue;
		}
		if (PPKYConfig.zone_fire_mode[zi] != m) {
			PPKYConfig.zone_fire_mode[zi] = m;
			EventLog_LogFireModeChange((uint8_t)(zi + 1u), m, ZONE_MODE_SRC_CAN);
			any = 1u;
		}
	}
	if (any != 0u) {
		Fire_NotifyZoneModeChanged();
	}
}

static uint8_t g_zone_mode_ui_new = 0u;
static uint8_t g_zone_mode_ui_zi = 0u;

void PPKY_ZoneModeUiNotify(uint8_t zone_idx)
{
	g_zone_mode_ui_new = 1u;
	g_zone_mode_ui_zi = zone_idx;
}

uint8_t PPKY_ZoneModeUiConsumeNew(uint8_t *zone_idx_out)
{
	if (g_zone_mode_ui_new == 0u) {
		return 0u;
	}
	g_zone_mode_ui_new = 0u;
	if (zone_idx_out != NULL) {
		*zone_idx_out = g_zone_mode_ui_zi;
	}
	return 1u;
}
