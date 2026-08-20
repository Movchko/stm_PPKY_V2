#include "config_zone_block.h"
#include "device_config.h"
#include "gost_mode.h"
#include "fire.h"

extern PPKYCfg PPKYConfig;

uint8_t PPKY_ZoneFireModeGet(uint8_t zone_idx)
{
	if (zone_idx >= ZONE_NUMBER) {
		return 0u;
	}
	uint8_t m = PPKYConfig.zone_fire_mode[zone_idx];
	if (m > 3u) {
		m = 0u;
	}
	return m;
}

void PPKY_ZoneFireModeSet(uint8_t zone_idx, uint8_t mode)
{
	if (zone_idx >= ZONE_NUMBER) {
		return;
	}
	if (mode > 3u) {
		mode = 0u;
	}
	/* В меню ГОСТ автономный (1) не выставляем; защита на API. */
	if (mode == 1u) {
		mode = 0u;
	}
	if (PPKYConfig.zone_fire_mode[zone_idx] == mode) {
		return;
	}
	PPKYConfig.zone_fire_mode[zone_idx] = mode;
	Fire_NotifyZoneModeChanged();
}

uint8_t PPKY_ZoneEffectiveMode(uint8_t zone_can)
{
#if GOST_MODE
	if (zone_can == 0u) {
		return PPKYConfig.fire_mode;
	}
	return PPKY_ZoneFireModeGet((uint8_t)(zone_can - 1u));
#else
	(void)zone_can;
	return PPKYConfig.fire_mode;
#endif
}

uint8_t PPKY_ZoneLaunchBlockedByCanZone(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return 0u;
	}
	return (PPKY_ZoneEffectiveMode(zone_can) == 3u) ? 1u : 0u;
}

uint8_t PPKY_ZoneIsManualByCanZone(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return (PPKYConfig.fire_mode == 2u) ? 1u : 0u;
	}
	return (PPKY_ZoneEffectiveMode(zone_can) == 2u) ? 1u : 0u;
}

uint8_t PPKY_AnyZoneManualOrBlocked(void)
{
#if GOST_MODE
	/* Только именованные зоны (как в меню "РЕЖИМ ЗОН"). */
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (PPKYConfig.zone_name[zi][0] == 0) {
			continue;
		}
		uint8_t m = PPKY_ZoneFireModeGet(zi);
		if (m == 2u || m == 3u) {
			return 1u;
		}
	}
	return 0u;
#else
	return (PPKYConfig.fire_mode == 2u) ? 1u : 0u;
#endif
}

void PPKY_ZoneFireModeInitFromGlobal(void)
{
	uint8_t fm = PPKYConfig.fire_mode;
	if (fm > 3u) {
		fm = 0u;
	}
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		PPKYConfig.zone_fire_mode[zi] = fm;
	}
	Fire_NotifyZoneModeChanged();
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
