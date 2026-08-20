#include "config_ign_block_sync.h"

#include "config_sync.hpp"
#include "app.hpp"
#include "device_config.h"
#include "tick_time.h"

extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;

#define IGN_BLOCK_START_DELAY_MS 5000u

static uint8_t g_boot_sync_done;
static uint8_t g_boot_sync_started;
static uint8_t g_pending_request;
static uint8_t g_igniter_mcu_seen;
static uint32_t g_igniter_mcu_seen_ms;

static uint8_t IgnBlockSync_ActiveMcuHasIgniter(void)
{
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (ad->online == 0u) {
			continue;
		}
		if ((ad->dev.zone & 0x7Fu) == 0u) {
			continue;
		}
		if (ad->dev.d_type != DEVICE_MCU_IGN_TYPE &&
		    ad->dev.d_type != DEVICE_MCU_TC_TYPE &&
		    ad->dev.d_type != DEVICE_MCU_K1 &&
		    ad->dev.d_type != DEVICE_MCU_K2 &&
		    ad->dev.d_type != DEVICE_MCU_K3 &&
		    ad->dev.d_type != DEVICE_MCU_KR) {
			continue;
		}
		for (uint8_t vi = 0u; vi < ad->vdev_count; vi++) {
			if (ad->vdevs[vi].online != 0u &&
			    ad->vdevs[vi].v_d_type == DEVICE_IGNITER_TYPE) {
				return 1u;
			}
		}
	}
	return 0u;
}

static void IgnBlockSync_TryStart(void)
{
	if (ConfigSync_IsBusy()) {
		g_pending_request = 1u;
		return;
	}
	g_pending_request = 0u;
	ConfigSync_StartIgnBlockSync();
}

void ConfigIgnBlockSync_Init(void)
{
	g_boot_sync_done = 0u;
	g_boot_sync_started = 0u;
	g_pending_request = 0u;
	g_igniter_mcu_seen = 0u;
	g_igniter_mcu_seen_ms = 0u;
}

void ConfigIgnBlockSync_Request(void)
{
	g_pending_request = 1u;
	IgnBlockSync_TryStart();
}

uint8_t ConfigIgnBlockSync_ShouldDeferCrc(void)
{
	/* Нет спичек - boot-синк не нужен, CRC можно сразу. */
	if (g_igniter_mcu_seen == 0u) {
		return 0u;
	}
	/* Ждём старта или завершения первой синхронизации блоков. */
	if (g_boot_sync_done == 0u) {
		return 1u;
	}
	return 0u;
}

void ConfigIgnBlockSync_Process1ms(uint32_t now_ms)
{
	if (IgnBlockSync_ActiveMcuHasIgniter() != 0u) {
		if (g_igniter_mcu_seen == 0u) {
			g_igniter_mcu_seen = 1u;
			g_igniter_mcu_seen_ms = now_ms;
		}
	}

	if (g_pending_request != 0u && !ConfigSync_IsBusy()) {
		IgnBlockSync_TryStart();
	}

	if (g_boot_sync_done != 0u) {
		return;
	}

	if (g_boot_sync_started != 0u) {
		/* Старт уже был: ждём освобождения ConfigSync. */
		if (!ConfigSync_IsBusy()) {
			g_boot_sync_done = 1u;
		}
		return;
	}

	if (g_igniter_mcu_seen == 0u) {
		return;
	}
	if (!TickAgeExpiredMs(now_ms, g_igniter_mcu_seen_ms, IGN_BLOCK_START_DELAY_MS)) {
		return;
	}
	if (ConfigSync_IsBusy()) {
		return;
	}

	g_boot_sync_started = 1u;
	ConfigSync_StartIgnBlockSync();
	if (!ConfigSync_IsBusy()) {
		/* Нет eligible-слотов / мгновенный finish - boot-синк не нужен. */
		g_boot_sync_done = 1u;
	}
}
