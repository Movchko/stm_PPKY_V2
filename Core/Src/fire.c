#include "fire.h"

#include "main.h"
#include "button.h"
#include "beeper.h"
#include "backend.h"
#include "device_config.h"
#include <string.h>
#include <stdio.h>
#include "app.hpp"
#include "menu_ui.h"
#include "event_log.h"
#include "warning.h"
#include "gost_mode.h"
#include "config_zone_block.h"
#include "tick_time.h"
#include "rs_panel_proto.h"
#include "rs_panel_v3_master.h"

extern PPKYCfg PPKYConfig;
extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;

#define FIRE_MAX_SLOTS       16u
#define FIRE_UI_MAX_ZONES    16u
/* Малая панель: порог «короткого» ПУСК СП vs переход к удержанию общего пуска. */
#define FIRE_SP_TO_ALL_HOLD_MS               1000u
#define FIRE_START_ALL_HOLD_MS               3000u
/* ack_flags у IGNITER: предполагаем бит 1 = end_ack */
#define FIRE_IGNITER_END_ACK_MASK            0x02u
#define FIRE_CMD_RETRY_TIMEOUT_MS            200u
#define FIRE_CMD_RETRY_TIMEOUT_STOP_RETRY_S	10
#define FIRE_CMD_RETRY_MAX_ATTEMPTS          (FIRE_CMD_RETRY_TIMEOUT_STOP_RETRY_S * 1000 / FIRE_CMD_RETRY_TIMEOUT_MS)//    10u
#define FIRE_CMD_RETRY_MAX_ITEMS             64u
#define FIRE_EXT_RETRY_MAX_ATTEMPTS          5u
#define FIRE_EXT_RETRY_INTERVAL_MS           2000u
/* Один цикл прожига (device_igniter: ramp 200 + hold 800 + wait 50) + запас на статус по шине.
 * Нужен, чтобы дотушивание не слало повторный Start до появления end_ack. */
#define FIRE_IGNITER_BURN_ONE_SHOT_MS        1500u
/* Спецрежим сдачи по ГОСТ: см. gost_mode.h (зональный СТОП/ПУСК, сброс hold 5с, unmute). */
#define FIRE_MAX_SOURCES_PER_ZONE            8u
#if GOST_MODE
#define FIRE_GOST_STOP_RESET_HOLD_MS         5000u
/* После квитирования hold ОСТАНОВ: игнор повторных SetStatusFire тех же источников
 * (МКУ ретраит, пока датчик в Fire / was_fire). Новый source_key снова поднимает пожар. */
#define FIRE_RESET_SUPPRESS_MAX              (FIRE_MAX_SLOTS * FIRE_MAX_SOURCES_PER_ZONE)
#endif
/* Коды payload catalog events 9–13 (doc/event_log_catalog.json). */
#define FIRE_LOG_DET_DPT_R                   0u
#define FIRE_LOG_DET_DPT_TC                  1u
#define FIRE_LOG_DET_BUTTON                  2u
#define FIRE_LOG_DET_EXTERNAL                3u
#define FIRE_LOG_DET_MANUAL                  4u

#define FIRE_LOG_START_MANUAL                0u
#define FIRE_LOG_START_AUTO                  1u
#define FIRE_LOG_START_AUTONOMOUS            2u

#define FIRE_LOG_STOP_OPERATOR               0u
#define FIRE_LOG_STOP_LIMIT                  1u
#define FIRE_LOG_STOP_FAULT                  2u
#define FIRE_LOG_STOP_MODE_CHANGE            3u
#define FIRE_LOG_STOP_OTHER                  4u
#define FIRE_LOG_STOP_NO_EVENT               0xFFu

#define FIRE_LOG_ABORT_NO_RESPONSE           0u
#define FIRE_LOG_ABORT_IGNITER_FAULT         1u
#define FIRE_LOG_ABORT_TIMER                 2u
#define FIRE_LOG_ABORT_OPERATOR              3u
#define FIRE_LOG_ABORT_CONFIG                4u
#define FIRE_LOG_ABORT_OTHER                 5u

#define FIRE_LOG_COMPLETE_FULL               0u
#define FIRE_LOG_COMPLETE_SKIPPED            1u
#define FIRE_LOG_COMPLETE_PARTIAL            2u

#define FIRE_LOG_BTN_START_ALL               0u
#define FIRE_LOG_BTN_START_SP                1u
#define FIRE_LOG_BTN_STOP                    2u

#define FIRE_LOG_PAUSE_SRC_PANEL             0u
#define FIRE_LOG_PAUSE_SRC_CAN               1u

typedef enum {
	FIRE_STATE_IDLE = 0,
	FIRE_STATE_WAIT_AUTO,
	FIRE_STATE_WAIT_MANUAL,
	FIRE_STATE_EXTINGUISHING
} FireState;

typedef enum {
	FIRE_EVENT_NONE = 0,
	FIRE_EVENT_STATUS_FIRE,
	FIRE_EVENT_REPLY_FIRE,
	FIRE_EVENT_STOP_EXT,
	FIRE_EVENT_BTN_START_SP,
	FIRE_EVENT_BTN_START_ALL,
	FIRE_EVENT_BTN_STOP,
	FIRE_EVENT_BUS_START_EXT,
	FIRE_EVENT_STATUS_FIRE_REDISPLAY,
	FIRE_EVENT_TICK_1MS
} FireEvent;

typedef struct {
	uint8_t  zone;
	uint8_t  active;
	uint8_t  phase2_sent;
	uint8_t  fire1_waiting;
	uint8_t  ext_retry_attempts;
	uint8_t  ext_retry_failed;
	uint8_t  extinguish_locked;
	uint8_t  fire_redisplay;
	uint8_t  launch_stopped; /* ОСТАНОВ по этой зоне (GOST): авто-таймер зоны не идёт */
	uint8_t  countdown_paused; /* GOST: пауза таймера ДО ПУСКА по зоне */
	uint8_t  extinguish_aborted; /* GOST: операторский стоп тушения («ТУШЕНИЕ ОСТ.») */
	uint8_t  ext_seen_no_ack;
	uint8_t  source_count;
	uint8_t  log_incomplete_posted;
	uint32_t phase2_deadline_ms;
	uint32_t ext_retry_next_ms;
	uint32_t paused_remaining_ms;
	uint32_t appeared_ms; /* момент появления слота (новое сверху в UI) */
	uint32_t source_keys[FIRE_MAX_SOURCES_PER_ZONE];
} FireZoneSlot;

typedef struct {
	uint8_t d_type;
	uint8_t h_adr;
	uint8_t l_adr;
	uint8_t zone;
} FireIgniterAddr;

typedef enum {
	FIRE_RETRY_START = 0u,
	FIRE_RETRY_STOP  = 1u,
	FIRE_RETRY_PAUSE = 2u,
	FIRE_RETRY_RESUME = 3u
} FireRetryKind;

typedef struct {
	uint8_t used;
	uint8_t kind;
	uint8_t attempts_sent;
	uint32_t deadline_ms;
	FireIgniterAddr addr;
	uint8_t zone_delay_sec;
	uint8_t module_delay_sec;
} FireRetryItem;

/* Центральный runtime-контекст сценария пожара (FSM + индикация + UI/звук). */
typedef struct {
	FireState state;
	uint8_t   reply_received;
	uint8_t   all_hold_active;
	uint16_t  all_hold_ms;
	/* Малая панель: удержание ПУСК СП (<1с → ПУСК СП, ≥1с UI общего, 3с → ОБЩИЙ). */
	uint8_t   sp_hold_active;
	uint16_t  sp_hold_ms;
	uint32_t  state_start_ms;
	uint8_t   stop_launch_pressed_latched;
	uint8_t   start_launch_pressed_latched;
	uint8_t   btn_start_all_hold_latched;
	uint8_t   btn_start_sp_latched;
	uint8_t   btn_stop_latched;
	/* Дедупликация критических команд ПАНЕЛИ (время принятого действия, мс). */
	uint32_t last_btn_start_all_action_ms;
	uint32_t last_btn_start_sp_action_ms;
#if GOST_MODE
	uint8_t   gost_stop_hold_active;
	uint8_t   gost_stop_reset_latched;
	uint16_t  gost_stop_hold_ms;
#endif
	/* После «ОСТАНОВ ПУСКА»: авто-отсчёт зон и его отображение отключены, слоты пожара сохраняются */
	uint8_t   zone_countdown_stopped;
	/* После команды PauseExtinguishmentTimer: таймеры зон "заморожены". */
	uint8_t   zone_countdown_paused;
	uint8_t   log_complete_posted;
	FireZoneSlot slots[FIRE_MAX_SLOTS];
} FireContext;

static FireContext g_fire;
static FireRetryItem g_fire_retry_items[FIRE_CMD_RETRY_MAX_ITEMS];
static uint8_t g_fire_ui_manual_select_enabled = 0u;
static uint8_t g_fire_ui_selected_index = 0u;
static uint8_t g_fire_bus_cmd_zone = 0u;
static uint8_t g_fire_bus_cmd_launch_type = 0u;
/* 1 = Transition вызван с физической панели (не с CAN-кнопки). */
static uint8_t g_fire_panel_btn_source = 0u;
#if GOST_MODE
typedef struct {
	uint8_t used;
	uint8_t zone;
	uint32_t source_key;
} FireResetSuppressEntry;
static FireResetSuppressEntry g_fire_reset_suppress[FIRE_RESET_SUPPRESS_MAX];
#endif

/* Отправляет широкую команду STOP всем МКУ пожарного контура. */
static void Fire_SendStopAllMcus(void);
/* Отправляет фазу 1 запуска по конкретной зоне. */
static void Fire_SendPhase1Zone(uint8_t zone);
/* Отправляет фазу 2 запуска по конкретной зоне. */
static uint8_t Fire_SendPhase2Zone(uint8_t zone);
/* Отправляет фазу 2 только тем спичкам зоны, у которых нет end_ack. */
static uint8_t Fire_SendPhase2ZonePending(uint8_t zone);
/* Запускает фазу 2 по всем слотам, где она ещё не отправлялась. */
static uint8_t Fire_Phase2AllPending(void);
/* Отметить отправку фазы 2 в слоте и инициализировать контур дотушивания. */
static void Fire_MarkSlotPhase2Sent(FireZoneSlot *slot, uint32_t now_ms, uint8_t start_type);
/* Полностью очищает слоты пожара и флаги остановки таймеров. */
static void Fire_ClearAllSlots(void);
#if GOST_MODE
/* GOST: удержание ОСТАНОВ ≥5 с — сброс активного пожара (слоты, таймеры, UI). */
static void Fire_GostResetFire(uint32_t now_ms);
static void Fire_ResetSuppressClear(void);
static void Fire_ResetSuppressAdd(uint8_t zone, uint32_t source_key);
static void Fire_ResetSuppressAddFromSlot(const FireZoneSlot *slot);
static uint8_t Fire_ResetSuppressHas(uint8_t zone, uint32_t source_key);
static void Fire_ClearSlotIndex(uint8_t idx);
#endif
static void Fire_Transition(FireEvent ev, uint32_t now_ms);
/* Синхронизирует состояние FSM исходя из состояния слотов/фаз. */
static void Fire_SyncStateFromSlots(void);
/* Переводит номер зоны CAN (1..N) в индекс массива имён (0..N-1). */
static uint8_t Fire_ZoneCanToIdx(uint8_t zone_can);
static uint8_t Fire_UiZoneKey(uint8_t zone);
static uint8_t Fire_ZoneLaunchBlocked(uint8_t zone_can);
/* Возвращает задержку зоны (сек) перед фазой 2. */
static uint8_t Fire_ZoneDelaySec(uint8_t zone);
static uint8_t Fire_AnyActiveSlot(void);
/* Возвращает задержку конкретного igniter-модуля из конфига ППКУ. */
static uint8_t Fire_ModuleDelaySecForIgniter(const FireIgniterAddr *addr);
/* Макс. module_delay (сек) среди спичек зоны без end_ack. */
static uint8_t Fire_ZonePendingMaxModuleDelaySec(uint8_t zone);
/* Пауза перед проверкой/повтором дотушивания с учётом задержки спички. */
static uint32_t Fire_ExtinguishGraceMs(uint8_t zone);
/* Поиск настроенного МКУ в PPKYConfig.CfgDevices. */
static const MKUCfg* Fire_FindConfiguredMcuByZoneHAdr(uint8_t zone, uint8_t h_adr, uint8_t mcu_d_type);
/* Проверка, что тип — один из типов МКУ. */
static uint8_t Fire_IsMcuDType(uint8_t d_type);
/* Ограничение uint32 в диапазон uint8. */
static uint8_t Fire_ClampU8FromU32(uint32_t v);
/* Ищет слот по UI-ключу зоны (0≡1), для панели/паузы/STOP. */
static int8_t Fire_FindSlotZone(uint8_t zone);
/* Ищет слот по точной CAN-зоне — для FSM пожара / extinguish_locked. */
static int8_t Fire_FindSlotZoneExact(uint8_t zone);
/* Выделяет свободный слот пожара, возвращает индекс или -1. */
static int8_t Fire_AllocSlot(void);
/** @return 1 если зона добавлена впервые в цикле, 0 если по этой зоне пожар уже учтён */
static uint8_t Fire_TryAddNewFireZone(uint8_t zone, uint32_t source_key, uint8_t and_effective,
				      uint32_t now_ms, uint32_t msg_id, const uint8_t *msg_data);
/* Операции с "И"-логикой пожара и источниками зоны. */
static uint8_t Fire_IsAndEnabledForZone(uint8_t zone);
static uint8_t Fire_CountOnlineDptForZone(uint8_t zone);
static uint8_t Fire_IsAndEffectiveForZone(uint8_t zone);
static uint32_t Fire_SourceKeyFromFireMsg(uint32_t msg_id, const uint8_t *msg_data);
static uint8_t Fire_AddSourceToSlot(FireZoneSlot *slot, uint32_t source_key);
static uint8_t Fire_HasFire1Waiting(void);
static uint8_t Fire_PromoteFire1WhenAndUnavailable(uint32_t now_ms);
/* Возвращает 1, если по зоне тушение уже запускалось и повторный пуск запрещён. */
static uint8_t Fire_ZoneIsExtinguishLocked(uint8_t zone);
/* Отметить зону как тушащуюся по команде с шины (без повторной отправки CAN). */
static uint8_t Fire_MarkExternalExtinguishZone(uint8_t zone, uint32_t now_ms);
/* Обновляет флаг «видели спички без end_ack» для корректного UI-списка зон. */
static void Fire_UpdateExtinguishAckTracking(void);
/* Считает зоны, где фаза 2 ещё не отправлена. */
static uint8_t Fire_CountPendingPhase2(void);
/* Минимальный оставшийся таймер (сек) среди pending-зон. */
/* Оставшийся таймер (сек) для конкретной зоны из UI-выбора. */
static uint8_t Fire_RemainingSecForZone(uint8_t zone, uint32_t now_ms);
/* Автообработка дедлайнов фазы 2 в автоматическом режиме. */
static uint8_t Fire_ProcessAutoDeadlines(uint32_t now_ms);
/* ПУСК ОБЩИЙ: старт по всем найденным igniter-зонам + отметка слотов. */
static uint8_t Fire_StartAllExistingZonesAndMarkSlots(void);
/* Возвращает 1, если по всем IGNITER в зоне есть end_ack. */
static uint8_t Fire_ZoneAllIgnitersEndAck(uint8_t zone);
/* Возвращает 1, если все активные пожарные зоны завершили тушение (end_ack). */
static uint8_t Fire_AllActiveZonesEndAck(void);
/* Возвращает 1, если все активные пожарные зоны в терминальном состоянии (успех/ошибка). */
/* Возвращает 1, если есть хотя бы одна зона с исчерпанными попытками дотушивания. */
static uint8_t Fire_HasExtinguishFailure(void);
/* Периодический дотушивающий перезапуск только незавершённых спичек. */
static void Fire_ProcessExtinguishRetries(uint32_t now_ms);
static uint8_t Fire_BuildUiZoneList(uint8_t *zones, uint8_t max_out);
/* Собирает отсортированный список igniter-адресов в заданной зоне. */
static uint8_t Fire_CollectSortedIgniterTargetsByZone(uint8_t zone, FireIgniterAddr *out, uint8_t max_out);
/* Собирает отсортированный список igniter-адресов по всем активным зонам. */
static uint8_t Fire_CollectSortedIgniterTargetsAll(FireIgniterAddr *out, uint8_t max_out);
/* Есть ли у МКУ хотя бы один online IGNITER-vdev. */
static uint8_t Fire_DeviceHasIgniterVdev(const ActiveDeviceInfo *ad);
/* Отправка команд fire-сервиса адресной спичке. */
static void Fire_SendStartToIgniterAddr(const FireIgniterAddr *addr, uint8_t zd_sec, uint8_t md_sec);
static void Fire_SendStopToIgniterAddr(const FireIgniterAddr *addr);
static void Fire_SendPauseToIgniterAddr(const FireIgniterAddr *addr);
static void Fire_SendResumeToIgniterAddr(const FireIgniterAddr *addr);
/* Ретрай-пул команд старта/остановки спичек. */
static void Fire_RetryQueueStart(const FireIgniterAddr *addr, uint8_t zd_sec, uint8_t md_sec, uint32_t now_ms);
static void Fire_RetryQueueStop(const FireIgniterAddr *addr, uint32_t now_ms);
static void Fire_RetryQueuePause(const FireIgniterAddr *addr, uint32_t now_ms);
static void Fire_RetryQueueResume(const FireIgniterAddr *addr, uint32_t now_ms);
static void Fire_RetryProcess(uint32_t now_ms);
static void Fire_RetryAckByMsgId(uint8_t kind, uint32_t msg_id);
static void Fire_RetryCancelAll(void);
static void Fire_RetryCancelKind(uint8_t kind);
/* Заглушка отчёта о спичке, которая не подтвердила запуск. */
static void SetErrIgnNotStart(const FireIgniterAddr *addr);
static uint8_t Fire_Phase2SelectedPending(uint8_t zone);
static void Fire_SendStopZone(uint8_t zone);
static void Fire_AbortExtinguishRetriesForZone(uint8_t zone);
static uint8_t Fire_GetSelectedZoneFromUi(uint8_t *zone);
static void Fire_ApplyPanelZoneSelect(uint8_t zone);
#if !GOST_MODE
static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text, uint8_t stop_reason);
#endif
static void Fire_ApplyFireModePolicy(uint32_t now_ms);
static void Fire_PauseCountdownAndDispatch(uint32_t now_ms);
static void Fire_ResumeCountdownAndDispatch(uint32_t now_ms);

static uint8_t Fire_ZoneCanToIdx(uint8_t zone_can)
{
	/* В CAN зоне обычно приходят как 1..N; в UI/массивах имён используем 0..N-1. */
	uint8_t z = (uint8_t)(zone_can & 0x7Fu);
	if (z == 0u) {
		return 0xFFu;
	}
	return (uint8_t)(z - 1u);
}

static uint8_t Fire_UiZoneKey(uint8_t zone)
{
	uint8_t z = (uint8_t)(zone & 0x7Fu);
	return (z == 0u) ? 1u : z;
}

static uint8_t Fire_ZoneLaunchBlocked(uint8_t zone_can)
{
	return PPKY_ZoneLaunchBlockedByCanZone(zone_can);
}

static uint8_t Fire_ZoneIsManual(uint8_t zone_can)
{
	return PPKY_ZoneIsManualByCanZone(zone_can);
}

static uint8_t Fire_ZoneDelaySec(uint8_t zone)
{
	uint8_t best = 0xFFu;
	uint8_t found = 0u;

	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online || ad->dev.zone != (zone & 0x7Fu) || !Fire_DeviceHasIgniterVdev(ad) ||
		    !Fire_IsMcuDType(ad->dev.d_type)) {
			continue;
		}
		const MKUCfg *cfg = Fire_FindConfiguredMcuByZoneHAdr(ad->dev.zone, ad->dev.h_adr, ad->dev.d_type);
		if (cfg == NULL) {
			continue;
		}
		uint8_t zd = Fire_ClampU8FromU32(cfg->zone_delay);
		if (!found || zd < best) {
			best = zd;
			found = 1u;
		}
	}

	if (found) {
		return best;
	}

	/* Fallback: если МКУ ещё не в активных, берём минимум по конфигу этой зоны. */
	for (uint8_t i = 0u; i < 32u; i++) {
		const MKUCfg *cfg = &PPKYConfig.CfgDevices[i];
		const Device *dv = &cfg->UId.devId;
		if (dv->d_type == 0u || !Fire_IsMcuDType(dv->d_type) || (dv->zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		uint8_t has_ign = 0u;
		for (uint8_t vi = 0u; vi < NUM_DEV_IN_MCU; vi++) {
			if ((uint8_t)cfg->VDtype[vi] == DEVICE_IGNITER_TYPE) {
				has_ign = 1u;
				break;
			}
		}
		if (!has_ign) {
			continue;
		}
		uint8_t zd = Fire_ClampU8FromU32(cfg->zone_delay);
		if (!found || zd < best) {
			best = zd;
			found = 1u;
		}
	}

	return found ? best : 0u;
}

static uint8_t Fire_ModuleDelaySecForIgniter(const FireIgniterAddr *addr)
{
	if (addr == NULL) {
		return 0u;
	}

	uint8_t mcu_d_type = 0u;
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online || !Fire_IsMcuDType(ad->dev.d_type)) {
			continue;
		}
		if ((ad->dev.zone & 0x7Fu) == (addr->zone & 0x7Fu) && ad->dev.h_adr == addr->h_adr &&
		    Fire_DeviceHasIgniterVdev(ad)) {
			mcu_d_type = ad->dev.d_type;
			break;
		}
	}

	const MKUCfg *cfg = Fire_FindConfiguredMcuByZoneHAdr(addr->zone & 0x7Fu, addr->h_adr, mcu_d_type);
	if (cfg == NULL) {
		return 0u;
	}

	uint8_t idx = (addr->l_adr > 0u) ? (uint8_t)(addr->l_adr - 1u) : 0u;
	if (idx >= NUM_DEV_IN_MCU) {
		return 0u;
	}
	return Fire_ClampU8FromU32(cfg->module_delay[idx]);
}

static uint8_t Fire_ZonePendingMaxModuleDelaySec(uint8_t zone)
{
	uint8_t max_delay = 0u;

	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || m->dev.zone != zone) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			if (!m->vdevs[vi].online || m->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			if ((m->vdevs[vi].ack_flags & FIRE_IGNITER_END_ACK_MASK) != 0u) {
				continue;
			}
			FireIgniterAddr addr;
			addr.d_type = DEVICE_IGNITER_TYPE;
			addr.h_adr = m->dev.h_adr;
			addr.l_adr = m->vdevs[vi].v_l_adr & 0x3Fu;
			addr.zone = m->dev.zone & 0x7Fu;
			uint8_t md = Fire_ModuleDelaySecForIgniter(&addr);
			if (md > max_delay) {
				max_delay = md;
			}
		}
	}
	return max_delay;
}

static uint32_t Fire_ExtinguishGraceMs(uint8_t zone)
{
	/* Окно до первого дотушивания: module_delay до старта ШИМ + длительность прожига + интервал. */
	return (uint32_t)Fire_ZonePendingMaxModuleDelaySec(zone) * 1000u
		+ FIRE_IGNITER_BURN_ONE_SHOT_MS
		+ FIRE_EXT_RETRY_INTERVAL_MS;
}

static uint8_t Fire_IgniterHasEndAck(const FireIgniterAddr *addr)
{
	if (addr == NULL) {
		return 0u;
	}
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo *m = &g_active_devices[mi];
		if (!m->online ||
		    (m->dev.h_adr != addr->h_adr) ||
		    ((m->dev.zone & 0x7Fu) != (addr->zone & 0x7Fu))) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			if (!m->vdevs[vi].online || m->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			if ((m->vdevs[vi].v_l_adr & 0x3Fu) != (addr->l_adr & 0x3Fu)) {
				continue;
			}
			return ((m->vdevs[vi].ack_flags & FIRE_IGNITER_END_ACK_MASK) != 0u) ? 1u : 0u;
		}
	}
	return 0u;
}

static uint8_t Fire_IsMcuDType(uint8_t d_type)
{
	return (d_type == DEVICE_MCU_IGN_TYPE ||
		d_type == DEVICE_MCU_TC_TYPE ||
		d_type == DEVICE_MCU_K1 ||
		d_type == DEVICE_MCU_K2 ||
		d_type == DEVICE_MCU_K3 ||
		d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

static uint8_t Fire_ClampU8FromU32(uint32_t v)
{
	return (v > 255u) ? 255u : (uint8_t)v;
}

static const MKUCfg* Fire_FindConfiguredMcuByZoneHAdr(uint8_t zone, uint8_t h_adr, uint8_t mcu_d_type)
{
	for (uint8_t i = 0u; i < 32u; i++) {
		const MKUCfg *cfg = &PPKYConfig.CfgDevices[i];
		const Device *dv = &cfg->UId.devId;
		if (dv->d_type == 0u) {
			continue;
		}
		if ((dv->zone & 0x7Fu) != (zone & 0x7Fu) || dv->h_adr != h_adr) {
			continue;
		}
		if (mcu_d_type != 0u && dv->d_type != mcu_d_type) {
			continue;
		}
		return cfg;
	}
	return NULL;
}

static uint8_t Fire_CollectSortedIgniterTargetsByZone(uint8_t zone, FireIgniterAddr *out, uint8_t max_out)
{
	uint8_t n = 0u;
	uint8_t z = zone & 0x7Fu;
	for (uint8_t i = 0u; i < g_active_devices_count && n < max_out; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online || ((ad->dev.zone & 0x7Fu) != z)) {
			continue;
		}
		for (uint8_t vi = 0u; vi < ad->vdev_count && n < max_out; vi++) {
			if (!ad->vdevs[vi].online || ad->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			out[n].d_type = DEVICE_IGNITER_TYPE;
			out[n].h_adr = ad->dev.h_adr;
			out[n].l_adr = ad->vdevs[vi].v_l_adr & 0x3Fu;
			out[n].zone = ad->dev.zone & 0x7Fu;
			n++;
		}
	}
	for (uint8_t a = 1u; a < n; a++) {
		FireIgniterAddr key = out[a];
		uint8_t b = a;
		while (b > 0u) {
			const FireIgniterAddr *prev = &out[b - 1u];
			uint8_t prev_gt = (prev->h_adr > key.h_adr) ||
					  ((prev->h_adr == key.h_adr) && (prev->l_adr > key.l_adr));
			if (!prev_gt) {
				break;
			}
			out[b] = out[b - 1u];
			b--;
		}
		out[b] = key;
	}
	return n;
}

static uint8_t Fire_CollectSortedIgniterTargetsAll(FireIgniterAddr *out, uint8_t max_out)
{
	uint8_t n = 0u;
	for (uint8_t i = 0u; i < g_active_devices_count && n < max_out; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online) {
			continue;
		}
		for (uint8_t vi = 0u; vi < ad->vdev_count && n < max_out; vi++) {
			if (!ad->vdevs[vi].online || ad->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			out[n].d_type = DEVICE_IGNITER_TYPE;
			out[n].h_adr = ad->dev.h_adr;
			out[n].l_adr = ad->vdevs[vi].v_l_adr & 0x3Fu;
			out[n].zone = ad->dev.zone & 0x7Fu;
			n++;
		}
	}
	for (uint8_t a = 1u; a < n; a++) {
		FireIgniterAddr key = out[a];
		uint8_t b = a;
		while (b > 0u) {
			const FireIgniterAddr *prev = &out[b - 1u];
			uint8_t prev_gt = (prev->zone > key.zone) ||
					  ((prev->zone == key.zone) && (prev->h_adr > key.h_adr)) ||
					  ((prev->zone == key.zone) && (prev->h_adr == key.h_adr) &&
					   (prev->l_adr > key.l_adr));
			if (!prev_gt) {
				break;
			}
			out[b] = out[b - 1u];
			b--;
		}
		out[b] = key;
	}
	return n;
}

static uint8_t Fire_DeviceHasIgniterVdev(const ActiveDeviceInfo *ad)
{
	if (ad == NULL || !ad->online) {
		return 0u;
	}
	for (uint8_t vi = 0u; vi < ad->vdev_count; vi++) {
		if (ad->vdevs[vi].online && ad->vdevs[vi].v_d_type == DEVICE_IGNITER_TYPE) {
			return 1u;
		}
	}
	return 0u;
}

static void Fire_SendStartToIgniterAddr(const FireIgniterAddr *addr, uint8_t zd_sec, uint8_t md_sec)
{
	if (addr == NULL) {
		return;
	}
	can_ext_id_t can_id;
	uint8_t data[8] = { 0 };

	can_id.ID = 0;
	can_id.field.dir = 0;
	can_id.field.d_type = addr->d_type & 0x7Fu;
	can_id.field.h_adr = addr->h_adr;
	can_id.field.l_adr = addr->l_adr & 0x3Fu;
	can_id.field.zone = addr->zone & 0x7Fu;

	data[0] = (uint8_t)ServiceCmd_Fire_StartExtinguishment;
	data[1] = addr->zone & 0x7Fu;
	data[2] = zd_sec;
	data[3] = md_sec;
	SendMessageFull(can_id, data, 0, BUS_CAN12);
	RelayAuto_NotifyStartExtinguish(addr->zone & 0x7Fu);
}

static void Fire_SendStopToIgniterAddr(const FireIgniterAddr *addr)
{
	if (addr == NULL) {
		return;
	}
	can_ext_id_t can_id;
	uint8_t data[8] = { 0 };

	can_id.ID = 0;
	can_id.field.dir = 0;
	can_id.field.d_type = addr->d_type & 0x7Fu;
	can_id.field.h_adr = addr->h_adr;
	can_id.field.l_adr = addr->l_adr & 0x3Fu;
	can_id.field.zone = addr->zone & 0x7Fu;

	data[0] = (uint8_t)ServiceCmd_Fire_StopExtinguishment;
	SendMessageFull(can_id, data, 0, BUS_CAN12);
}

static void Fire_SendPauseToIgniterAddr(const FireIgniterAddr *addr)
{
	if (addr == NULL) {
		return;
	}
	can_ext_id_t can_id;
	uint8_t data[8] = { 0 };

	can_id.ID = 0;
	can_id.field.dir = 0;
	can_id.field.d_type = addr->d_type & 0x7Fu;
	can_id.field.h_adr = addr->h_adr;
	can_id.field.l_adr = addr->l_adr & 0x3Fu;
	can_id.field.zone = addr->zone & 0x7Fu;

	data[0] = (uint8_t)ServiceCmd_Fire_PauseExtinguishmentTimer;
	SendMessageFull(can_id, data, 0, BUS_CAN12);
}

static void Fire_SendResumeToIgniterAddr(const FireIgniterAddr *addr)
{
	if (addr == NULL) {
		return;
	}
	can_ext_id_t can_id;
	uint8_t data[8] = { 0 };

	can_id.ID = 0;
	can_id.field.dir = 0;
	can_id.field.d_type = addr->d_type & 0x7Fu;
	can_id.field.h_adr = addr->h_adr;
	can_id.field.l_adr = addr->l_adr & 0x3Fu;
	can_id.field.zone = addr->zone & 0x7Fu;

	data[0] = (uint8_t)ServiceCmd_Fire_ResumeExtinguishmentTimer;
	SendMessageFull(can_id, data, 0, BUS_CAN12);
}

static int16_t Fire_RetryFind(uint8_t kind, const FireIgniterAddr *addr)
{
	for (uint8_t i = 0u; i < FIRE_CMD_RETRY_MAX_ITEMS; i++) {
		const FireRetryItem *it = &g_fire_retry_items[i];
		if (!it->used || it->kind != kind) {
			continue;
		}
		if (it->addr.d_type == addr->d_type && it->addr.h_adr == addr->h_adr &&
		    it->addr.l_adr == addr->l_adr && it->addr.zone == addr->zone) {
			return (int16_t)i;
		}
	}
	return -1;
}

static int16_t Fire_RetryAlloc(void)
{
	for (uint8_t i = 0u; i < FIRE_CMD_RETRY_MAX_ITEMS; i++) {
		if (!g_fire_retry_items[i].used) {
			return (int16_t)i;
		}
	}
	return -1;
}

static void Fire_RetryQueueStart(const FireIgniterAddr *addr, uint8_t zd_sec, uint8_t md_sec, uint32_t now_ms)
{
	int16_t pos;
	if (addr == NULL) {
		return;
	}
	Fire_SendStartToIgniterAddr(addr, zd_sec, md_sec);
	pos = Fire_RetryFind(FIRE_RETRY_START, addr);
	if (pos < 0) {
		pos = Fire_RetryAlloc();
	}
	if (pos < 0) {
		return;
	}

	g_fire_retry_items[(uint8_t)pos].used = 1u;
	g_fire_retry_items[(uint8_t)pos].kind = FIRE_RETRY_START;
	g_fire_retry_items[(uint8_t)pos].attempts_sent = 1u;
	g_fire_retry_items[(uint8_t)pos].deadline_ms = now_ms + FIRE_CMD_RETRY_TIMEOUT_MS;
	g_fire_retry_items[(uint8_t)pos].addr = *addr;
	g_fire_retry_items[(uint8_t)pos].zone_delay_sec = zd_sec;
	g_fire_retry_items[(uint8_t)pos].module_delay_sec = md_sec;
}

static void Fire_RetryQueueStop(const FireIgniterAddr *addr, uint32_t now_ms)
{
	int16_t pos;
	if (addr == NULL) {
		return;
	}
	Fire_SendStopToIgniterAddr(addr);
	pos = Fire_RetryFind(FIRE_RETRY_STOP, addr);
	if (pos < 0) {
		pos = Fire_RetryAlloc();
	}
	if (pos < 0) {
		return;
	}

	g_fire_retry_items[(uint8_t)pos].used = 1u;
	g_fire_retry_items[(uint8_t)pos].kind = FIRE_RETRY_STOP;
	g_fire_retry_items[(uint8_t)pos].attempts_sent = 1u;
	g_fire_retry_items[(uint8_t)pos].deadline_ms = now_ms + FIRE_CMD_RETRY_TIMEOUT_MS;
	g_fire_retry_items[(uint8_t)pos].addr = *addr;
	g_fire_retry_items[(uint8_t)pos].zone_delay_sec = 0u;
	g_fire_retry_items[(uint8_t)pos].module_delay_sec = 0u;
}

static void Fire_RetryQueuePause(const FireIgniterAddr *addr, uint32_t now_ms)
{
	int16_t pos;
	if (addr == NULL) {
		return;
	}
	Fire_SendPauseToIgniterAddr(addr);
	pos = Fire_RetryFind(FIRE_RETRY_PAUSE, addr);
	if (pos < 0) {
		pos = Fire_RetryAlloc();
	}
	if (pos < 0) {
		return;
	}
	g_fire_retry_items[(uint8_t)pos].used = 1u;
	g_fire_retry_items[(uint8_t)pos].kind = FIRE_RETRY_PAUSE;
	g_fire_retry_items[(uint8_t)pos].attempts_sent = 1u;
	g_fire_retry_items[(uint8_t)pos].deadline_ms = now_ms + FIRE_CMD_RETRY_TIMEOUT_MS;
	g_fire_retry_items[(uint8_t)pos].addr = *addr;
	g_fire_retry_items[(uint8_t)pos].zone_delay_sec = 0u;
	g_fire_retry_items[(uint8_t)pos].module_delay_sec = 0u;
}

static void Fire_RetryQueueResume(const FireIgniterAddr *addr, uint32_t now_ms)
{
	int16_t pos;
	if (addr == NULL) {
		return;
	}
	Fire_SendResumeToIgniterAddr(addr);
	pos = Fire_RetryFind(FIRE_RETRY_RESUME, addr);
	if (pos < 0) {
		pos = Fire_RetryAlloc();
	}
	if (pos < 0) {
		return;
	}
	g_fire_retry_items[(uint8_t)pos].used = 1u;
	g_fire_retry_items[(uint8_t)pos].kind = FIRE_RETRY_RESUME;
	g_fire_retry_items[(uint8_t)pos].attempts_sent = 1u;
	g_fire_retry_items[(uint8_t)pos].deadline_ms = now_ms + FIRE_CMD_RETRY_TIMEOUT_MS;
	g_fire_retry_items[(uint8_t)pos].addr = *addr;
	g_fire_retry_items[(uint8_t)pos].zone_delay_sec = 0u;
	g_fire_retry_items[(uint8_t)pos].module_delay_sec = 0u;
}

static void Fire_RetryProcess(uint32_t now_ms)
{
	for (uint8_t i = 0u; i < FIRE_CMD_RETRY_MAX_ITEMS; i++) {
		FireRetryItem *it = &g_fire_retry_items[i];
		if (!it->used) {
			continue;
		}
		if ((int32_t)(now_ms - it->deadline_ms) < 0) {
			continue;
		}
		if (it->attempts_sent >= FIRE_CMD_RETRY_MAX_ATTEMPTS) {
			if (it->kind == FIRE_RETRY_START) {
				SetErrIgnNotStart(&it->addr);
			}
			it->used = 0u;
			continue;
		}

		if (it->kind == FIRE_RETRY_START) {
			Fire_SendStartToIgniterAddr(&it->addr, it->zone_delay_sec, it->module_delay_sec);
		} else if (it->kind == FIRE_RETRY_STOP) {
			Fire_SendStopToIgniterAddr(&it->addr);
		} else if (it->kind == FIRE_RETRY_PAUSE) {
			Fire_SendPauseToIgniterAddr(&it->addr);
		} else {
			Fire_SendResumeToIgniterAddr(&it->addr);
		}
		it->attempts_sent++;
		it->deadline_ms = now_ms + FIRE_CMD_RETRY_TIMEOUT_MS;
	}
}

static void Fire_RetryAckByMsgId(uint8_t kind, uint32_t msg_id)
{
	can_ext_id_t id;
	id.ID = msg_id & 0x0FFFFFFFu;
	for (uint8_t i = 0u; i < FIRE_CMD_RETRY_MAX_ITEMS; i++) {
		FireRetryItem *it = &g_fire_retry_items[i];
		if (!it->used || it->kind != kind) {
			continue;
		}
		if (it->addr.d_type != (id.field.d_type & 0x7Fu)) {
			continue;
		}
		if (it->addr.h_adr != id.field.h_adr ||
		    (it->addr.l_adr & 0x3Fu) != (id.field.l_adr & 0x3Fu) ||
		    (it->addr.zone & 0x7Fu) != (id.field.zone & 0x7Fu)) {
			continue;
		}
		it->used = 0u;
	}
}

static void Fire_RetryCancelAll(void)
{
	memset(g_fire_retry_items, 0, sizeof(g_fire_retry_items));
}

static void Fire_RetryCancelKind(uint8_t kind)
{
	for (uint8_t i = 0u; i < FIRE_CMD_RETRY_MAX_ITEMS; i++) {
		if (g_fire_retry_items[i].used && g_fire_retry_items[i].kind == kind) {
			g_fire_retry_items[i].used = 0u;
		}
	}
}

static void SetErrIgnNotStart(const FireIgniterAddr *addr)
{
	/* TODO: заполнить обработку ошибки "спичка не подтвердила запуск". */
	(void)addr;
}

static void Fire_SendPhase1Zone(uint8_t zone)
{
	/* Фаза 1 больше не шлёт StartExtinguishment на МКУ.
	 * Отсчёт zone_delay живёт только в слоте ППКУ (phase2_deadline_ms);
	 * единственный пуск СП — фаза 2 (таймер / ПУСК СП / ПУСК ОБЩИЙ).
	 * Раньше Start(zd+md) здесь + повторный Start(0,md) в фазе 2 давали двойной прожиг. */
	(void)zone;
}

static uint8_t Fire_SendPhase2Zone(uint8_t zone)
{
	/* Фаза 2: единственный пуск (zone_delay=0), только module_delay.
	 * Уже отработавшие спички (end_ack) пропускаем — как в дотушивании. */
	if (Fire_ZoneLaunchBlocked(zone)) {
		return 0u;
	}
	if (Fire_ZoneIsExtinguishLocked(zone)) {
		return 0u;
	}
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	for (uint8_t i = 0u; i < n; i++) {
		if (Fire_IgniterHasEndAck(&ign[i])) {
			continue;
		}
		uint8_t md = Fire_ModuleDelaySecForIgniter(&ign[i]);
		Fire_RetryQueueStart(&ign[i], 0u, md, HAL_GetTick());
	}
	/* Слот помечаем даже если все уже с end_ack — иначе зона навсегда «висит» без фазы 2. */
	return (n > 0u) ? 1u : 0u;
}

static uint8_t Fire_SendPhase2ZonePending(uint8_t zone)
{
	/* Дотушивание: повторно запускаем только незавершённые спички (без end_ack). */
	uint8_t sent = 0u;
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo *m = &g_active_devices[mi];
		if (!m->online || ((m->dev.zone & 0x7Fu) != (zone & 0x7Fu))) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			if (!m->vdevs[vi].online || m->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			if ((m->vdevs[vi].ack_flags & FIRE_IGNITER_END_ACK_MASK) != 0u) {
				continue;
			}
			FireIgniterAddr addr;
			addr.d_type = DEVICE_IGNITER_TYPE;
			addr.h_adr = m->dev.h_adr;
			addr.l_adr = m->vdevs[vi].v_l_adr & 0x3Fu;
			addr.zone = m->dev.zone & 0x7Fu;
			Fire_RetryQueueStart(&addr, 0u, Fire_ModuleDelaySecForIgniter(&addr), HAL_GetTick());
			sent = 1u;
		}
	}
	return sent;
}

static uint8_t Fire_LogMasterWagon(void)
{
	return PPKYConfig.UId.devId.h_adr;
}

static uint8_t Fire_LogDetectSource(uint32_t msg_id, const uint8_t *msg_data)
{
	uint8_t d_type;

	if (msg_data != NULL) {
		d_type = msg_data[1];
	} else {
		can_ext_id_t id;
		id.ID = msg_id & 0x0FFFFFFFu;
		d_type = (uint8_t)(id.field.d_type & 0x7Fu);
	}

	if (d_type == DEVICE_BUTTON_TYPE) {
		return FIRE_LOG_DET_BUTTON;
	}
	if (d_type == DEVICE_DPT_TYPE) {
		return FIRE_LOG_DET_DPT_R;
	}
	return FIRE_LOG_DET_EXTERNAL;
}

/* additional[2] в логах: mode зоны из zone_fire_mode[]; 0xFF если зона не задана. */
static uint8_t Fire_LogZoneModeByte(uint8_t zone_can)
{
	if (zone_can == 0u) {
		return 0xFFu;
	}
	return PPKY_ZoneEffectiveMode(zone_can);
}

static uint8_t Fire_LogStartTypeFromZone(uint8_t zone_can)
{
	uint8_t m = PPKY_ZoneEffectiveMode(zone_can);
	if (m == 1u) {
		return FIRE_LOG_START_AUTONOMOUS;
	}
	if (m == 0u) {
		return FIRE_LOG_START_AUTO;
	}
	return FIRE_LOG_START_MANUAL;
}

static void Fire_ClearSlotCountdown(FireZoneSlot *s)
{
	if (s == 0) {
		return;
	}
	s->phase2_deadline_ms = 0u;
	s->paused_remaining_ms = 0u;
	s->countdown_paused = 0u;
}

static void Fire_LogFireDetected(uint32_t msg_id, const uint8_t *msg_data, uint8_t zone)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.can_header = msg_id & 0x1FFFFFFFu;
	if (msg_data != NULL) {
		memcpy(payload.can_data, msg_data, 8u);
	}
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = Fire_LogDetectSource(msg_id, msg_data);
	(void)EventLog_Post(EVENT_LOG_FIRE_DETECTED, &payload);
}

static void Fire_LogExtinguishStart(uint8_t zone, uint8_t start_type)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = start_type;
	payload.additional[1] = zone & 0x7Fu;
	payload.additional[2] = Fire_ZoneDelaySec(zone);
	(void)EventLog_Post(EVENT_LOG_EXTINGUISH_START, &payload);
	g_fire.log_complete_posted = 0u;
}

static void Fire_LogForceStop(uint8_t stop_reason, uint8_t zone)
{
	EventLogPayload_t payload;

	if (stop_reason == FIRE_LOG_STOP_NO_EVENT) {
		return;
	}
	if (!Fire_AnyActiveSlot()) {
		return;
	}

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = stop_reason;
	payload.additional[1] = zone;
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	(void)EventLog_Post(EVENT_LOG_EXTINGUISH_FORCE_STOP, &payload);
}

static void Fire_LogExtinguishComplete(void)
{
	EventLogPayload_t payload;

	if (g_fire.log_complete_posted != 0u) {
		return;
	}

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = FIRE_LOG_COMPLETE_FULL;
	payload.additional[3] = 0u;
	(void)EventLog_Post(EVENT_LOG_EXTINGUISH_COMPLETE, &payload);
	g_fire.log_complete_posted = 1u;
}

static void Fire_LogExtinguishIncomplete(FireZoneSlot *slot, uint8_t abort_reason)
{
	EventLogPayload_t payload;

	if (slot == NULL || slot->log_incomplete_posted != 0u) {
		return;
	}

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = abort_reason;
	payload.additional[1] = (uint8_t)g_fire.state;
	payload.additional[2] = slot->zone & 0x7Fu;
	(void)EventLog_Post(EVENT_LOG_EXTINGUISH_INCOMPLETE, &payload);
	slot->log_incomplete_posted = 1u;
}

static void Fire_LogPanelButton(uint8_t button, uint8_t zone, uint8_t hold_s)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = button;
	payload.additional[1] = zone & 0x7Fu;
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	payload.additional[3] = hold_s;
	(void)EventLog_Post(EVENT_LOG_PANEL_BUTTON, &payload);
}

/* Сырое нажатие большой кнопки — всегда при касании, без проверки FSM. */
static void Fire_LogPanelBtnPress(uint8_t button, uint8_t zone)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = button;
	payload.additional[1] = zone & 0x7Fu;
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	(void)EventLog_Post(EVENT_LOG_PANEL_BTN_PRESS, &payload);
}

static void Fire_LogCountdownPause(uint8_t zone, uint8_t source)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = source;
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	(void)EventLog_Post(EVENT_LOG_COUNTDOWN_PAUSE, &payload);
}

static void Fire_LogCountdownResume(uint8_t zone, uint8_t source)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = source;
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	(void)EventLog_Post(EVENT_LOG_COUNTDOWN_RESUME, &payload);
}

/* GOST: сброс пожара (hold ОСТАНОВ ≥5 с). zone=0 — без явной зоны / глобально. */
static void Fire_LogFireReset(uint8_t zone, uint8_t source)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = source; /* 0=gost_panel_hold_stop */
	payload.additional[2] = Fire_LogZoneModeByte(zone);
	(void)EventLog_Post(EVENT_LOG_FIRE_RESET, &payload);
}

static void Fire_MarkSlotPhase2Sent(FireZoneSlot *slot, uint32_t now_ms, uint8_t start_type)
{
	if (slot == NULL) {
		return;
	}
	slot->phase2_sent = 1u;
	slot->fire1_waiting = 0u;
	slot->launch_stopped = 0u;
	slot->countdown_paused = 0u;
	slot->extinguish_aborted = 0u;
	slot->extinguish_locked = 1u;
	slot->ext_retry_attempts = 0u;
	slot->ext_retry_failed = 0u;
	slot->ext_seen_no_ack = 0u;
	slot->log_incomplete_posted = 0u;
	slot->ext_retry_next_ms = now_ms + Fire_ExtinguishGraceMs(slot->zone);
	Fire_LogExtinguishStart(slot->zone, start_type);
	/* Панель кэширует ZONES; форс следующего POLL со статусом ТУШЕНИЕ. */
	RsPanelV3Master_InvalidateFireZonesTx();
}

static void Fire_SendStopAllMcus(void)
{
	/* Остановка адресно по каждой активной спичке. */
	FireIgniterAddr ign[32];
	uint8_t n = Fire_CollectSortedIgniterTargetsAll(ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint32_t now_ms = HAL_GetTick();
	Fire_RetryCancelKind(FIRE_RETRY_START);
	Fire_RetryCancelKind(FIRE_RETRY_PAUSE);
	Fire_RetryCancelKind(FIRE_RETRY_RESUME);
	for (uint8_t i = 0u; i < n; i++) {
		Fire_RetryQueueStop(&ign[i], now_ms);
	}
}

static void Fire_PauseCountdownAndDispatch(uint32_t now_ms)
{
	if (g_fire.zone_countdown_stopped || g_fire.zone_countdown_paused) {
		return;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent || s->fire1_waiting || s->launch_stopped) {
			continue;
		}
		/* Ручной: таймера нет — не ставим pause и не трогаем дедлайн. */
		if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
			Fire_ClearSlotCountdown(s);
			continue;
		}
		if (now_ms >= s->phase2_deadline_ms) {
			s->paused_remaining_ms = 0u;
		} else {
			s->paused_remaining_ms = s->phase2_deadline_ms - now_ms;
		}
		s->countdown_paused = 1u;
		FireIgniterAddr ign[16];
		uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
		for (uint8_t j = 0u; j < n; j++) {
			Fire_RetryQueuePause(&ign[j], now_ms);
		}
	}
	g_fire.zone_countdown_paused = 1u;
	Fire_LogCountdownPause(0u, FIRE_LOG_PAUSE_SRC_CAN);
}

static void Fire_ResumeCountdownAndDispatch(uint32_t now_ms)
{
	if (!g_fire.zone_countdown_paused) {
		return;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent || s->fire1_waiting || s->launch_stopped) {
			continue;
		}
		/* Ручной/блок: resume не должен создать таймер. */
		if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
			Fire_ClearSlotCountdown(s);
			continue;
		}
		s->phase2_deadline_ms = now_ms + s->paused_remaining_ms;
		s->countdown_paused = 0u;
		FireIgniterAddr ign[16];
		uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
		for (uint8_t j = 0u; j < n; j++) {
			Fire_RetryQueueResume(&ign[j], now_ms);
		}
	}
	g_fire.zone_countdown_paused = 0u;
	Fire_LogCountdownResume(0u, FIRE_LOG_PAUSE_SRC_CAN);
}

static int8_t Fire_FindSlotZone(uint8_t zone)
{
	/* UI/панель: zone 0 и 1 — один ключ (СТЕНД). */
	uint8_t key = Fire_UiZoneKey(zone);
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && Fire_UiZoneKey(g_fire.slots[i].zone) == key) {
			return (int8_t)i;
		}
	}
	return -1;
}

static int8_t Fire_FindSlotZoneExact(uint8_t zone)
{
	uint8_t z = zone & 0x7Fu;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && ((g_fire.slots[i].zone & 0x7Fu) == z)) {
			return (int8_t)i;
		}
	}
	return -1;
}

#if GOST_MODE
static void Fire_PauseZoneCountdown(uint8_t zone, uint32_t now_ms)
{
	int8_t si = Fire_FindSlotZone(zone);
	if (si < 0) {
		return;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
	if (!s->active || s->phase2_sent || s->fire1_waiting || s->launch_stopped ||
	    s->countdown_paused != 0u || s->extinguish_aborted != 0u) {
		return;
	}
	if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
		Fire_ClearSlotCountdown(s);
		return;
	}
	if (now_ms >= s->phase2_deadline_ms) {
		s->paused_remaining_ms = 0u;
	} else {
		s->paused_remaining_ms = s->phase2_deadline_ms - now_ms;
	}
	s->countdown_paused = 1u;
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	for (uint8_t j = 0u; j < n; j++) {
		Fire_RetryQueuePause(&ign[j], now_ms);
	}
	Fire_LogCountdownPause(zone, FIRE_LOG_PAUSE_SRC_PANEL);
}

static void Fire_ResumeZoneCountdown(uint8_t zone, uint32_t now_ms)
{
	int8_t si = Fire_FindSlotZone(zone);
	if (si < 0) {
		return;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
	if (!s->active || s->countdown_paused == 0u) {
		return;
	}
	if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
		Fire_ClearSlotCountdown(s);
		return;
	}
	s->phase2_deadline_ms = now_ms + s->paused_remaining_ms;
	s->countdown_paused = 0u;
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	for (uint8_t j = 0u; j < n; j++) {
		Fire_RetryQueueResume(&ign[j], now_ms);
	}
	Fire_LogCountdownResume(zone, FIRE_LOG_PAUSE_SRC_PANEL);
}

static uint8_t Fire_AbortZoneExtinguishOperator(uint8_t zone, uint32_t now_ms)
{
	int8_t si = Fire_FindSlotZone(zone);
	if (si < 0) {
		return 0u;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
	if (!s->active || !s->phase2_sent || s->extinguish_aborted != 0u) {
		return 0u;
	}
	(void)now_ms;
	Fire_ResetSuppressAddFromSlot(s);
	Fire_SendStopZone(zone);
	Fire_AbortExtinguishRetriesForZone(zone);
	s->extinguish_aborted = 1u;
	s->ext_retry_failed = 0u;
	s->ext_retry_next_ms = 0u;
	s->launch_stopped = 1u;
	return 1u;
}
#endif

static int8_t Fire_AllocSlot(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			return (int8_t)i;
		}
	}
	return -1;
}

static uint8_t Fire_IsAndEnabledForZone(uint8_t zone)
{
	uint8_t zi = Fire_ZoneCanToIdx(zone);
	if (zi >= ZONE_NUMBER) {
		return 0u;
	}
	return (PPKYConfig.fire_and[zi] != 0u) ? 1u : 0u;
}

static uint8_t Fire_CountOnlineDptForZone(uint8_t zone)
{
	uint8_t count = 0u;
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo *m = &g_active_devices[mi];
		if (!m->online || ((m->dev.zone & 0x7Fu) != (zone & 0x7Fu))) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			if (m->vdevs[vi].online && m->vdevs[vi].v_d_type == DEVICE_DPT_TYPE) {
				if (count < 0xFFu) {
					count++;
				}
			}
		}
	}
	return count;
}

static uint8_t Fire_IsAndEffectiveForZone(uint8_t zone)
{
	if (!Fire_IsAndEnabledForZone(zone)) {
		return 0u;
	}
	return (Fire_CountOnlineDptForZone(zone) >= 2u) ? 1u : 0u;
}

/*
 * Ключ источника для слота / ZONES UI:
 *   [20:14] origin_d_type (ДПТ/КН/… из payload SetStatusFire data[1])
 *   [13:6]  mcu h_adr (из CAN ID отправителя-МКУ)
 *   [5:0]   origin_l_adr (канал из payload data[2])
 * Раньше брали d_type/l_adr из CAN ID (= тип МКУ) → панель не находила
 * «ДПТ»/«КН» и рисовала только имя зоны.
 */
static uint32_t Fire_SourceKeyFromFireMsg(uint32_t msg_id, const uint8_t *msg_data)
{
	can_ext_id_t id;
	uint8_t origin_type;
	uint8_t origin_l;
	uint8_t mcu_h;

	id.ID = msg_id & 0x0FFFFFFFu;
	mcu_h = (uint8_t)(id.field.h_adr & 0xFFu);
	if (msg_data != 0 &&
	    msg_data[0] == (uint8_t)ServiceCmd_Fire_SetStatusFire) {
		origin_type = (uint8_t)(msg_data[1] & 0x7Fu);
		origin_l = (uint8_t)(msg_data[2] & 0x3Fu);
	} else {
		origin_type = (uint8_t)(id.field.d_type & 0x7Fu);
		origin_l = (uint8_t)(id.field.l_adr & 0x3Fu);
	}
	return (((uint32_t)origin_type) << 14) |
	       (((uint32_t)mcu_h) << 6) |
	       ((uint32_t)origin_l);
}

static uint8_t Fire_AddSourceToSlot(FireZoneSlot *slot, uint32_t source_key)
{
	if (slot == NULL) {
		return 0u;
	}
	for (uint8_t i = 0u; i < slot->source_count && i < FIRE_MAX_SOURCES_PER_ZONE; i++) {
		if (slot->source_keys[i] == source_key) {
			return 0u;
		}
	}
	if (slot->source_count < FIRE_MAX_SOURCES_PER_ZONE) {
		slot->source_keys[slot->source_count] = source_key;
	}
	if (slot->source_count < 0xFFu) {
		slot->source_count++;
	}
	return 1u;
}

static uint8_t Fire_HasFire1Waiting(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && g_fire.slots[i].fire1_waiting) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Fire_PromoteFire1WhenAndUnavailable(uint32_t now_ms)
{
	/* Автопереход ПОЖАР1 -> ПОЖАР2:
	 * если зона ждёт второй источник, но условие "И" стало неэффективным
	 * (например, в онлайне остался 1 ДПТ), запускаем обычный сценарий. */
	uint8_t promoted = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || !s->fire1_waiting) {
			continue;
		}
		if (Fire_ZoneLaunchBlocked(s->zone)) {
			continue;
		}
		if (Fire_IsAndEffectiveForZone(s->zone)) {
			continue;
		}
		s->fire1_waiting = 0u;
		s->phase2_sent = 0u;
		if (Fire_ZoneIsManual(s->zone)) {
			/* Ручной: таймера нет — ждём внешний ПУСК. */
			Fire_ClearSlotCountdown(s);
			s->launch_stopped = 1u;
			Fire_SendPhase1Zone(s->zone);
			promoted = 1u;
			continue;
		}
		s->phase2_deadline_ms = now_ms + (uint32_t)Fire_ZoneDelaySec(s->zone) * 1000u;
		s->paused_remaining_ms = 0u;
		Fire_SendPhase1Zone(s->zone);
		promoted = 1u;
	}
	return promoted;
}

static uint8_t Fire_TryAddNewFireZone(uint8_t zone, uint32_t source_key, uint8_t and_effective,
				      uint32_t now_ms, uint32_t msg_id, const uint8_t *msg_data)
{
	/* Возвращает:
	 * 0 - без изменений (дубль источника),
	 * 1 - новая зона,
	 * 2 - переход зоны из ПОЖАР1 в ПОЖАР2. */
#if !GOST_MODE
	(void)msg_id;
	(void)msg_data;
#endif
#if GOST_MODE
	/* Квитированный источник: МКУ может ретраить SetStatusFire — не поднимаем пожар снова. */
	if (Fire_ResetSuppressHas(zone, source_key)) {
		return 0u;
	}
#endif
	/* Точная зона: иначе 0≡1 через UiZoneKey склеивает два пожара в один
	 * extinguish_locked слот → второй ПУСК СП no-op. */
	int8_t si = Fire_FindSlotZoneExact(zone);
	if (si >= 0) {
		FireZoneSlot *existing = &g_fire.slots[(uint8_t)si];
		if (!Fire_AddSourceToSlot(existing, source_key)) {
			return 0u;
		}
#if GOST_MODE
		/* Новый источник / повторный цикл — обновить подпись бегущей строки. */
#endif
		if (existing->extinguish_locked) {
			existing->fire_redisplay = 1u;
			/* Новый цикл: в GOST уйдёт в конец списка, иначе — наверх. */
			existing->appeared_ms = now_ms;
			return 3u;
		}
		if (Fire_ZoneLaunchBlocked(zone)) {
			existing->fire1_waiting = 0u;
			existing->phase2_deadline_ms = 0u;
			return 0u;
		}
		if (existing->fire1_waiting && existing->source_count >= 2u) {
			existing->fire1_waiting = 0u;
			existing->phase2_sent = 0u;
			if (Fire_ZoneIsManual(zone)) {
				Fire_ClearSlotCountdown(existing);
				existing->launch_stopped = 1u;
				Fire_SendPhase1Zone(zone);
				return 2u;
			}
			existing->phase2_deadline_ms = now_ms + (uint32_t)Fire_ZoneDelaySec(zone) * 1000u;
			existing->paused_remaining_ms = 0u;
			Fire_SendPhase1Zone(zone);
			return 2u;
		}
		return 0u;
	}

	int8_t alloc_si = Fire_AllocSlot();
	if (alloc_si < 0) {
		alloc_si = 0;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)alloc_si];
	memset(s, 0, sizeof(*s));
	s->active = 1u;
	s->zone = zone & 0x7Fu;
	s->appeared_ms = now_ms;
	(void)Fire_AddSourceToSlot(s, source_key);
#if GOST_MODE
#endif
	if (Fire_ZoneLaunchBlocked(zone)) {
		s->fire1_waiting = 0u;
		s->phase2_sent = 0u;
		s->phase2_deadline_ms = 0u;
		s->paused_remaining_ms = 0u;
		return 1u;
	}
	if (Fire_ZoneIsManual(zone)) {
		s->fire1_waiting = 0u;
		s->phase2_sent = 0u;
		s->phase2_deadline_ms = 0u;
		s->paused_remaining_ms = 0u;
		s->launch_stopped = 1u;
		Fire_SendPhase1Zone(zone);
		return 1u;
	}
	if (and_effective) {
		s->fire1_waiting = 1u;
		s->phase2_sent = 0u;
		s->phase2_deadline_ms = 0u;
		s->paused_remaining_ms = 0u;
	} else {
		s->fire1_waiting = 0u;
		s->phase2_sent = 0u;
		s->phase2_deadline_ms = now_ms + (uint32_t)Fire_ZoneDelaySec(zone) * 1000u;
		s->paused_remaining_ms = 0u;
		Fire_SendPhase1Zone(zone);
	}
	return 1u;
}

static void Fire_ClearAllSlots(void)
{
	memset(g_fire.slots, 0, sizeof(g_fire.slots));
	g_fire.zone_countdown_stopped = 0u;
	g_fire.zone_countdown_paused = 0u;
	g_fire.log_complete_posted = 0u;
}

#if GOST_MODE
static void Fire_ClearSlotIndex(uint8_t idx)
{
	if (idx >= FIRE_MAX_SLOTS) {
		return;
	}
	memset(&g_fire.slots[idx], 0, sizeof(g_fire.slots[idx]));
}

static void Fire_ResetSuppressClear(void)
{
	memset(g_fire_reset_suppress, 0, sizeof(g_fire_reset_suppress));
}

static void Fire_ResetSuppressAdd(uint8_t zone, uint32_t source_key)
{
	for (uint8_t i = 0u; i < FIRE_RESET_SUPPRESS_MAX; i++) {
		if (g_fire_reset_suppress[i].used != 0u &&
		    g_fire_reset_suppress[i].zone == (zone & 0x7Fu) &&
		    g_fire_reset_suppress[i].source_key == source_key) {
			return;
		}
	}
	for (uint8_t i = 0u; i < FIRE_RESET_SUPPRESS_MAX; i++) {
		if (g_fire_reset_suppress[i].used == 0u) {
			g_fire_reset_suppress[i].used = 1u;
			g_fire_reset_suppress[i].zone = zone & 0x7Fu;
			g_fire_reset_suppress[i].source_key = source_key;
			return;
		}
	}
}

static void Fire_ResetSuppressAddFromSlot(const FireZoneSlot *slot)
{
	if (slot == NULL || slot->active == 0u) {
		return;
	}
	uint8_t n = slot->source_count;
	if (n > FIRE_MAX_SOURCES_PER_ZONE) {
		n = FIRE_MAX_SOURCES_PER_ZONE;
	}
	if (n == 0u) {
		/* Зона без ключей (редко): блокируем зону целиком по ключу 0. */
		Fire_ResetSuppressAdd(slot->zone, 0u);
		return;
	}
	for (uint8_t i = 0u; i < n; i++) {
		Fire_ResetSuppressAdd(slot->zone, slot->source_keys[i]);
	}
}

static uint8_t Fire_ResetSuppressHas(uint8_t zone, uint32_t source_key)
{
	uint8_t z = zone & 0x7Fu;
	for (uint8_t i = 0u; i < FIRE_RESET_SUPPRESS_MAX; i++) {
		if (g_fire_reset_suppress[i].used == 0u) {
			continue;
		}
		if (g_fire_reset_suppress[i].zone != z) {
			continue;
		}
		if (g_fire_reset_suppress[i].source_key == source_key ||
		    g_fire_reset_suppress[i].source_key == 0u) {
			return 1u;
		}
	}
	return 0u;
}
#endif

static uint8_t Fire_AnyActiveSlot(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Fire_ZoneIsExtinguishLocked(uint8_t zone)
{
	int8_t si = Fire_FindSlotZoneExact(zone);
	if (si < 0) {
		return 0u;
	}
	return g_fire.slots[(uint8_t)si].extinguish_locked;
}

static uint8_t Fire_EnsureSlotForZone(uint8_t zone, uint32_t now_ms)
{
	int8_t si = Fire_FindSlotZoneExact(zone);
	if (si >= 0) {
		return 1u;
	}
	si = Fire_AllocSlot();
	if (si < 0) {
		return 0u;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
	memset(s, 0, sizeof(*s));
	s->active = 1u;
	s->zone = zone & 0x7Fu;
	s->phase2_deadline_ms = now_ms;
	return 1u;
}

static uint8_t Fire_MarkExternalExtinguishZone(uint8_t zone, uint32_t now_ms)
{
	if (Fire_ZoneLaunchBlocked(zone)) {
		return 0u;
	}
	if (Fire_ZoneIsExtinguishLocked(zone)) {
		return 0u;
	}
	if (!Fire_EnsureSlotForZone(zone, now_ms)) {
		return 0u;
	}
	int8_t si = Fire_FindSlotZoneExact(zone);
	if (si < 0) {
		return 0u;
	}
	FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
	if (s->phase2_sent) {
		return 0u;
	}
	Fire_MarkSlotPhase2Sent(s, now_ms, FIRE_LOG_START_MANUAL);
	RelayAuto_NotifyStartExtinguish(zone & 0x7Fu);
	return 1u;
}

static uint8_t Fire_MarkExternalExtinguishAllZones(uint32_t now_ms)
{
	FireIgniterAddr ign[64];
	uint8_t n = Fire_CollectSortedIgniterTargetsAll(ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint8_t zone_marked[128] = {0};
	uint8_t any_started = 0u;

	for (uint8_t i = 0u; i < n; i++) {
		uint8_t z = ign[i].zone & 0x7Fu;
		if (zone_marked[z]) {
			continue;
		}
		zone_marked[z] = 1u;
		if (Fire_MarkExternalExtinguishZone(z, now_ms)) {
			any_started = 1u;
		}
	}

	/* Слоты для всех зон с igniter (как при ПУСК ОБЩИЙ на панели). */
	for (uint8_t z = 0u; z < 128u; z++) {
		if (!zone_marked[z]) {
			continue;
		}
		if (Fire_FindSlotZone(z) >= 0) {
			continue;
		}
		if (!Fire_EnsureSlotForZone(z, now_ms)) {
			continue;
		}
		int8_t si = Fire_FindSlotZone(z);
		if (si < 0) {
			continue;
		}
		FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
		if (!s->phase2_sent) {
			Fire_MarkSlotPhase2Sent(s, now_ms, FIRE_LOG_START_MANUAL);
			any_started = 1u;
		}
	}

	return any_started;
}

static void Fire_UpdateExtinguishAckTracking(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || !s->phase2_sent) {
			continue;
		}
		if (!Fire_ZoneAllIgnitersEndAck(s->zone)) {
			s->ext_seen_no_ack = 1u;
		}
	}
}

static uint8_t Fire_CountPendingPhase2(void)
{
	uint8_t c = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && !g_fire.slots[i].phase2_sent &&
		    !g_fire.slots[i].fire1_waiting && !g_fire.slots[i].launch_stopped &&
		    !Fire_ZoneLaunchBlocked(g_fire.slots[i].zone)) {
			c++;
		}
	}
	return c;
}

/* Зоны, по которым ещё можно вручную дать ПУСК СП (в т.ч. после зонального ОСТАНОВ).
 * ПОЖАР1 (fire1_waiting) тоже: панель ретраит START_SP пока status==FIRE1;
 * операторский пуск снимает ожидание 2-го источника и даёт фазу 2. */
static uint8_t Fire_CountManualStartable(void)
{
	uint8_t c = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || Fire_ZoneLaunchBlocked(s->zone)) {
			continue;
		}
		if (s->extinguish_aborted != 0u) {
			c++;
			continue;
		}
		if (!s->phase2_sent) {
			c++;
		}
	}
	return c;
}

static uint8_t Fire_RemainingSecForZone(uint8_t zone, uint32_t now_ms)
{
	uint32_t best_ms = 0xFFFFFFFFu;
	uint8_t found = 0u;

	if (Fire_ZoneLaunchBlocked(zone)) {
		return 0u;
	}
	if (g_fire.zone_countdown_stopped) {
		return 0u;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent || s->fire1_waiting || s->launch_stopped) {
			continue;
		}
		if ((s->zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		found = 1u;
		uint32_t rem;
		if (g_fire.zone_countdown_paused || s->countdown_paused != 0u) {
			rem = s->paused_remaining_ms;
		} else if (now_ms >= s->phase2_deadline_ms) {
			return 0u;
		} else {
			rem = s->phase2_deadline_ms - now_ms;
		}
		if (rem < best_ms) {
			best_ms = rem;
		}
	}
	if (!found) {
		return 0u;
	}
	return (uint8_t)((best_ms + 999u) / 1000u);
}

static uint8_t Fire_ProcessAutoDeadlines(uint32_t now_ms)
{
	/* Автозапуск фазы 2 по дедлайнам только в WAIT_AUTO и только для pending-слотов. */
	uint8_t any_started = 0u;
	if (g_fire.zone_countdown_stopped || g_fire.zone_countdown_paused) {
		return 0u;
	}
	if (g_fire.state != FIRE_STATE_WAIT_AUTO) {
		return 0u;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent ||
		    g_fire.slots[i].fire1_waiting || g_fire.slots[i].launch_stopped ||
		    g_fire.slots[i].countdown_paused != 0u ||
		    g_fire.slots[i].extinguish_aborted != 0u) {
			continue;
		}
		if (Fire_ZoneIsManual(g_fire.slots[i].zone)) {
			continue;
		}
		if (now_ms >= g_fire.slots[i].phase2_deadline_ms) {
			if (Fire_SendPhase2Zone(g_fire.slots[i].zone)) {
				Fire_MarkSlotPhase2Sent(&g_fire.slots[i], now_ms,
				                       Fire_LogStartTypeFromZone(g_fire.slots[i].zone));
				any_started = 1u;
			}
		}
	}
	return any_started;
}

static uint8_t Fire_Phase2AllPending(void)
{
	uint8_t started = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		if (Fire_ZoneLaunchBlocked(g_fire.slots[i].zone)) {
			continue;
		}
		if (g_fire.slots[i].extinguish_aborted != 0u) {
			g_fire.slots[i].extinguish_aborted = 0u;
			g_fire.slots[i].phase2_sent = 0u;
			g_fire.slots[i].extinguish_locked = 0u;
			g_fire.slots[i].ext_retry_failed = 0u;
		}
		if (g_fire.slots[i].phase2_sent) {
			continue;
		}
		/* ПУСК СП/ОБЩИЙ: снять зональный ОСТАНОВ; ПОЖАР1 — только после успешной фазы 2. */
		g_fire.slots[i].launch_stopped = 0u;
		g_fire.slots[i].countdown_paused = 0u;
		if (Fire_SendPhase2Zone(g_fire.slots[i].zone)) {
			g_fire.slots[i].fire1_waiting = 0u;
			Fire_MarkSlotPhase2Sent(&g_fire.slots[i], HAL_GetTick(), FIRE_LOG_START_MANUAL);
			started = 1u;
		}
	}
	return started;
}

static uint8_t Fire_Phase2SelectedPending(uint8_t zone)
{
	uint8_t started = 0u;
	uint8_t key = Fire_UiZoneKey(zone);
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		/* UI/панель шлют Fire_UiZoneKey (0→1); слот может хранить raw 0. */
		if (Fire_UiZoneKey(g_fire.slots[i].zone) != key) {
			continue;
		}
		if (Fire_ZoneLaunchBlocked(g_fire.slots[i].zone)) {
			continue;
		}
		if (g_fire.slots[i].extinguish_aborted != 0u) {
			g_fire.slots[i].extinguish_aborted = 0u;
			g_fire.slots[i].phase2_sent = 0u;
			g_fire.slots[i].extinguish_locked = 0u;
			g_fire.slots[i].ext_retry_failed = 0u;
		}
		if (g_fire.slots[i].phase2_sent) {
			continue;
		}
		g_fire.slots[i].launch_stopped = 0u;
		g_fire.slots[i].countdown_paused = 0u;
		if (Fire_SendPhase2Zone(g_fire.slots[i].zone)) {
			g_fire.slots[i].fire1_waiting = 0u;
			Fire_MarkSlotPhase2Sent(&g_fire.slots[i], HAL_GetTick(), FIRE_LOG_START_MANUAL);
			started = 1u;
		}
	}
	return started;
}

static void Fire_AbortExtinguishRetriesForZone(uint8_t zone)
{
	/* После ОСТАНОВ: не давать Fire_ProcessExtinguishRetries слать Start. */
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active) {
			continue;
		}
		if ((s->zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		s->fire1_waiting = 0u;
		if (s->phase2_sent) {
			/* Останавливаем дотушивание без «ТУШ.ОШ.» (LED_ERR только при реальном fail). */
			s->ext_retry_next_ms = 0u;
			s->ext_retry_attempts = FIRE_EXT_RETRY_MAX_ATTEMPTS;
		}
	}
}

static void Fire_AbortExtinguishRetriesAll(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || !s->phase2_sent) {
			continue;
		}
		s->ext_retry_next_ms = 0u;
		s->ext_retry_attempts = FIRE_EXT_RETRY_MAX_ATTEMPTS;
	}
}

static void Fire_SendStopZone(uint8_t zone)
{
	/* Останов одной зоны: снять arm на МКУ и НЕ имитировать «фаза 2 ушла».
	 * Раньше phase2_sent=1 + ext_retry_next_ms=0 сразу запускали дотушивание (Start)
	 * → спички отрабатывали после ОСТАНОВ, UI уходил в ТУШ.ВЫП. */
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint32_t now_ms = HAL_GetTick();
	Fire_RetryCancelKind(FIRE_RETRY_START);
	for (uint8_t i = 0u; i < n; i++) {
		Fire_RetryQueueStop(&ign[i], now_ms);
	}
	Fire_AbortExtinguishRetriesForZone(zone);
	{
		int8_t si = Fire_FindSlotZone(zone);
		if (si >= 0) {
			g_fire.slots[(uint8_t)si].launch_stopped = 1u;
		}
	}
}

static uint8_t Fire_GetSelectedZoneFromUi(uint8_t *zone)
{
	uint8_t zones[FIRE_UI_MAX_ZONES];
	uint8_t n = Fire_BuildUiZoneList(zones, FIRE_UI_MAX_ZONES);
	if (zone == NULL || n == 0u) {
		return 0u;
	}
	if (g_fire_ui_selected_index >= n) {
		return 0u;
	}
	*zone = zones[g_fire_ui_selected_index];
	return 1u;
}

static uint8_t Fire_StartAllExistingZonesAndMarkSlots(void)
{
	/* ПУСК ОБЩИЙ без привязки к статусу ПОЖАРА:
	 * отправка фазы 2 во все существующие зоны igniter и синхронизация слотов. */
	uint8_t zone_sent[128] = {0};
	uint8_t any_started = 0u;
	uint32_t now_ms = HAL_GetTick();

	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		if (!Fire_DeviceHasIgniterVdev(&g_active_devices[i])) {
			continue;
		}
		uint8_t z = g_active_devices[i].dev.zone & 0x7Fu;
		if (zone_sent[z] || Fire_ZoneIsExtinguishLocked(z) || Fire_ZoneLaunchBlocked(z)) {
			continue;
		}
		if (Fire_SendPhase2Zone(z)) {
			zone_sent[z] = 1u;
			any_started = 1u;
		}
	}

	/* Для слотов пожара помечаем отправку фазы 2 только в реально запущенные зоны */
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent) {
			continue;
		}
		if (zone_sent[g_fire.slots[i].zone & 0x7Fu]) {
			Fire_MarkSlotPhase2Sent(&g_fire.slots[i], now_ms, FIRE_LOG_START_MANUAL);
		}
	}

	/* Если общего пуска запущен без активных пожарных слотов, создаём слоты зон,
	 * чтобы UI/индикация ПУСК работали как при ПУСК СП и предупреждения не перебивали экран. */
	for (uint8_t z = 0u; z < 128u; z++) {
		if (!zone_sent[z]) {
			continue;
		}
		if (Fire_FindSlotZone(z) >= 0) {
			continue;
		}
		int8_t si = Fire_AllocSlot();
		if (si < 0) {
			continue;
		}
		FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
		memset(s, 0, sizeof(*s));
		s->active = 1u;
		s->zone = z;
		Fire_MarkSlotPhase2Sent(s, now_ms, FIRE_LOG_START_MANUAL);
		s->phase2_deadline_ms = now_ms;
	}

	return any_started;
}

static uint8_t Fire_ZoneAllIgnitersEndAck(uint8_t zone)
{
	uint8_t has_igniters = 0u;
	uint8_t z = zone & 0x7Fu;
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || ((m->dev.zone & 0x7Fu) != z)) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			if (!m->vdevs[vi].online || m->vdevs[vi].v_d_type != DEVICE_IGNITER_TYPE) {
				continue;
			}
			has_igniters = 1u;
			if ((m->vdevs[vi].ack_flags & FIRE_IGNITER_END_ACK_MASK) == 0u) {
				return 0u;
			}
		}
	}
	return has_igniters;
}

static uint8_t Fire_AllActiveZonesEndAck(void)
{
	uint8_t any_zone = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		any_zone = 1u;
		if (!Fire_ZoneAllIgnitersEndAck(g_fire.slots[i].zone)) {
			return 0u;
		}
	}
	return any_zone;
}

static uint8_t Fire_HasExtinguishFailure(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		const FireZoneSlot *s = &g_fire.slots[i];
		if (s->active && s->phase2_sent && s->ext_retry_failed != 0u) {
			return 1u;
		}
	}
	return 0u;
}

static void Fire_ProcessExtinguishRetries(uint32_t now_ms)
{
	if (g_fire.state != FIRE_STATE_EXTINGUISHING) {
		return;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || !s->phase2_sent || s->ext_retry_failed != 0u ||
		    s->extinguish_aborted != 0u) {
			continue;
		}
		if (Fire_ZoneAllIgnitersEndAck(s->zone)) {
			s->ext_retry_failed = 0u;
			continue;
		}
		if ((int32_t)(now_ms - s->ext_retry_next_ms) < 0) {
			continue;
		}
		if (s->ext_retry_attempts >= FIRE_EXT_RETRY_MAX_ATTEMPTS) {
			s->ext_retry_failed = 1u;
			Fire_LogExtinguishIncomplete(s, FIRE_LOG_ABORT_NO_RESPONSE);
			continue;
		}
		(void)Fire_SendPhase2ZonePending(s->zone);
		s->ext_retry_attempts++;
		s->ext_retry_next_ms = now_ms + Fire_ExtinguishGraceMs(s->zone);
		if (s->ext_retry_attempts >= FIRE_EXT_RETRY_MAX_ATTEMPTS &&
		    !Fire_ZoneAllIgnitersEndAck(s->zone)) {
			s->ext_retry_failed = 1u;
			Fire_LogExtinguishIncomplete(s, FIRE_LOG_ABORT_NO_RESPONSE);
		}
	}
}

static void Fire_SyncStateFromSlots(void)
{
	/* Синхронизирует верхнеуровневое состояние FSM из фактического состояния слотов. */
	if (!Fire_AnyActiveSlot()) {
		g_fire.state = FIRE_STATE_IDLE;
		return;
	}
	if (Fire_CountPendingPhase2() == 0u && !Fire_HasFire1Waiting()) {
		g_fire.state = FIRE_STATE_EXTINGUISHING;
	} else if (g_fire.state == FIRE_STATE_EXTINGUISHING || g_fire.state == FIRE_STATE_IDLE) {
		/* Ручные зоны отсекаются по слоту (Fire_ZoneIsManual); глобально всегда WAIT_AUTO. */
		g_fire.state = FIRE_STATE_WAIT_AUTO;
	}
}

static uint8_t Fire_BuildUiZoneList(uint8_t *zones, uint8_t max_out)
{
	uint8_t nz = 0u;

	if (zones == NULL || max_out == 0u) {
		return 0u;
	}

	/* Все активные слоты в общем списке (в т.ч. уже потушенные) —
	 * чтобы можно было листать 1/N и видеть «ТУШ.ВЫП.» по зоне.
	 * Ключ — CAN-зона 1..N: zone==0 и zone==1 не должны давать две строки СТЕНД. */
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		uint8_t z = Fire_UiZoneKey(g_fire.slots[i].zone);
		uint8_t dup = 0u;
		for (uint8_t j = 0u; j < nz; j++) {
			if (zones[j] == z) {
				dup = 1u;
				break;
			}
		}
		if (!dup && nz < max_out) {
			zones[nz++] = z;
		}
	}
	/* Сортировка по appeared_ms:
	 * GOST — старее сверху (первый пришедший = индекс 0, новые внизу);
	 * иначе — новее сверху. */
	for (uint8_t a = 1u; a < nz; a++) {
		uint8_t key_z = zones[a];
		uint32_t key_ms = 0u;
		for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
			if (g_fire.slots[i].active && g_fire.slots[i].zone == key_z) {
				key_ms = g_fire.slots[i].appeared_ms;
				break;
			}
		}
		uint8_t b = a;
		while (b > 0u) {
			uint32_t prev_ms = 0u;
			for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
				if (g_fire.slots[i].active && g_fire.slots[i].zone == zones[b - 1u]) {
					prev_ms = g_fire.slots[i].appeared_ms;
					break;
				}
			}
#if GOST_MODE
			if (prev_ms <= key_ms) {
				break;
			}
#else
			if (prev_ms >= key_ms) {
				break;
			}
#endif
			zones[b] = zones[b - 1u];
			b--;
		}
		zones[b] = key_z;
	}
	return nz;
}

/* Режим/таймер одной зоны — как выбор на ППКУ1 (не общий mode на весь список).
 * Если на одну CAN-зону несколько слотов — берём наиболее «продвинутый» статус,
 * чтобы после тушения не оставалась вторая строка «ДО ПУСКА 0». */
static uint8_t Fire_UiModeRank(uint8_t mode)
{
	switch (mode) {
	case 3u: return 90u; /* ТУШ.ВЫП. */
	case 7u: return 80u; /* ТУШ.ОШ. */
	case 9u: return 70u; /* ТУШ.ОСТ. */
	case 2u: return 60u; /* ТУШЕНИЕ */
	case 5u: return 50u; /* ПАУЗА */
	case 4u: return 40u; /* ПОЖАР/ОСТ. */
	case 8u: return 30u; /* ПУСК БЛОК */
	case 6u: return 20u; /* ПОЖАР1 */
	case 1u: return 10u; /* ДО ПУСКА */
	default: return 0u;
	}
}

static void Fire_UiStatusFromSlot(const FireZoneSlot *s, uint32_t now_ms, uint8_t *mode_out, uint8_t *rem_out)
{
	uint8_t mode = 0u;
	uint8_t rem = 0u;

	if (s == 0 || mode_out == 0 || rem_out == 0) {
		return;
	}
	if (Fire_ZoneLaunchBlocked(s->zone)) {
		mode = 8u;
	} else if (s->extinguish_aborted != 0u) {
		mode = 9u;
	} else if (s->ext_retry_failed != 0u) {
		mode = 7u;
	} else if (s->phase2_sent != 0u && Fire_ZoneAllIgnitersEndAck(s->zone)) {
		mode = 3u;
	} else if (s->phase2_sent != 0u) {
		mode = 2u;
	} else if (s->countdown_paused != 0u) {
		mode = 5u;
		rem = (uint8_t)((s->paused_remaining_ms + 999u) / 1000u);
	} else if (s->launch_stopped != 0u) {
		mode = 4u;
	} else if (s->fire1_waiting != 0u) {
		mode = 6u;
	} else {
		mode = 1u;
		rem = Fire_RemainingSecForZone(s->zone, now_ms);
	}
	if (g_fire.zone_countdown_stopped != 0u && mode == 1u) {
		mode = 4u;
		rem = 0u;
	}
	*mode_out = mode;
	*rem_out = rem;
}

static void Fire_UiStatusForZone(uint8_t zone, uint32_t now_ms, uint8_t *mode_out, uint8_t *rem_out)
{
	uint8_t best_mode = 0u;
	uint8_t best_rem = 0u;
	uint8_t best_rank = 0u;
	uint8_t found = 0u;
	uint8_t key;

	if (mode_out == 0 || rem_out == 0) {
		return;
	}
	key = Fire_UiZoneKey(zone);
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		uint8_t mode = 0u;
		uint8_t rem = 0u;
		uint8_t rank;
		const FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || Fire_UiZoneKey(s->zone) != key) {
			continue;
		}
		Fire_UiStatusFromSlot(s, now_ms, &mode, &rem);
		rank = Fire_UiModeRank(mode);
		if (found == 0u || rank > best_rank) {
			best_mode = mode;
			best_rem = rem;
			best_rank = rank;
			found = 1u;
		}
	}
	*mode_out = found ? best_mode : 0u;
	*rem_out = found ? best_rem : 0u;
}

/* Обобщённый индикатор ПОЖАР: ПОЖАР1 непрерывно, ПОЖАР2 и ВНИМАНИЕ мигают.
 * Приоритет: ПОЖАР2 > ПОЖАР1 > ВНИМАНИЕ. */
#if GOST_MODE
static void Fire_GostResetFire(uint32_t now_ms)
{
	/* Сброс пожара с панели (удержание ОСТАНОВ ≥5 с).
	 * При выборе зоны на UI — квитирование только этой зоны; иначе все слоты.
	 * Источники заносятся в suppress, чтобы ретраи SetStatusFire с МКУ
	 * не поднимали пожар снова (датчик/МКУ часто остаются в Fire до reboot). */
	uint8_t log_zone = 0u;
	uint8_t zone_only = 0u;
	uint8_t sel_zone = 0u;

	if (g_fire_ui_manual_select_enabled && Fire_GetSelectedZoneFromUi(&sel_zone)) {
		zone_only = 1u;
		log_zone = sel_zone;
	} else if (g_fire_ui_manual_select_enabled) {
		(void)Fire_GetSelectedZoneFromUi(&log_zone);
	}
	if (g_fire_panel_btn_source != 0u) {
		Fire_LogPanelButton(FIRE_LOG_BTN_STOP, log_zone, 5u);
	}

	if (zone_only != 0u) {
		int8_t si = Fire_FindSlotZone(sel_zone);
		if (si >= 0) {
			Fire_ResetSuppressAddFromSlot(&g_fire.slots[(uint8_t)si]);
			Fire_LogFireReset(sel_zone, 0u);
			Fire_SendStopZone(sel_zone);
			Fire_AbortExtinguishRetriesForZone(sel_zone);
			Fire_ClearSlotIndex((uint8_t)si);
		} else {
			Fire_LogFireReset(sel_zone, 0u);
			Fire_SendStopZone(sel_zone);
		}
	} else {
		uint8_t logged_any = 0u;
		for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
			if (!g_fire.slots[i].active) {
				continue;
			}
			Fire_ResetSuppressAddFromSlot(&g_fire.slots[i]);
			Fire_LogFireReset(g_fire.slots[i].zone, 0u);
			logged_any = 1u;
		}
		if (logged_any == 0u) {
			Fire_LogFireReset(log_zone, 0u);
		}

		Fire_SendStopAllMcus();
		Fire_AbortExtinguishRetriesAll();
		Fire_RetryCancelAll();
		Fire_ClearAllSlots();
	}

	g_fire.start_launch_pressed_latched = 0u;
	g_fire.stop_launch_pressed_latched = 0u;
	g_fire.all_hold_active = 0u;
	g_fire.all_hold_ms = 0u;
	g_fire.sp_hold_active = 0u;
	g_fire.sp_hold_ms = 0u;
	g_fire.btn_start_all_hold_latched = 0u;

	Fire_SyncStateFromSlots();
	g_fire.state_start_ms = now_ms;
	g_fire.reply_received = 0u;

	if (!Fire_AnyActiveSlot()) {
		g_fire.state = FIRE_STATE_IDLE;
		g_fire.zone_countdown_stopped = 0u;
		g_fire.zone_countdown_paused = 0u;
	} else {
		Fire_Transition(FIRE_EVENT_TICK_1MS, now_ms);
	}
}
#endif

#if !GOST_MODE
static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text, uint8_t stop_reason)
{
	/* Общее поведение "ОСТАНОВ ПУСКА": таймеры остановлены, arm на МКУ снят,
	 * дотушивание (повторный Start) запрещено, manual UI/LED. */
	Fire_LogForceStop(stop_reason, 0u);
	g_fire.zone_countdown_stopped = 1u;
	g_fire.zone_countdown_paused = 0u;
	g_fire.start_launch_pressed_latched = 0u;
	g_fire.stop_launch_pressed_latched = 1u;
	(void)blink_stop_text;
	(void)now_ms;
	Fire_SendStopAllMcus();
	Fire_AbortExtinguishRetriesAll();
	g_fire.all_hold_active = 0u;
	g_fire.all_hold_ms = 0u;
	g_fire.sp_hold_active = 0u;
	g_fire.sp_hold_ms = 0u;
	g_fire.btn_start_all_hold_latched = 0u;
	if (Fire_CountPendingPhase2() > 0u) {
		g_fire.state = FIRE_STATE_WAIT_MANUAL;
	}
}
#endif

static void Fire_ApplyFireModePolicy(uint32_t now_ms)
{
	/* Страховка: ручной/блок не держат countdown (resume/cmd13 не создают таймер). */
	uint8_t i;
	(void)now_ms;
	for (i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent != 0u) {
			continue;
		}
		if (Fire_ZoneLaunchBlocked(s->zone)) {
			Fire_ClearSlotCountdown(s);
			s->fire1_waiting = 0u;
			continue;
		}
		if (Fire_ZoneIsManual(s->zone)) {
			Fire_ClearSlotCountdown(s);
			s->launch_stopped = 1u;
		}
	}
}

static uint8_t Fire_ButtonPressedEvent(uint8_t button_id, uint8_t *latched_flag)
{
	ButtonState st = Button_GetState(button_id);
	if ((st == ButtonStatePress || st == ButtonStateLongPress) && (*latched_flag == 0u)) {
		*latched_flag = 1u;
		return 1u;
	}
	if (st == ButtonStateReset || st == ButtonStateError) {
		*latched_flag = 0u;
	}
	return 0u;
}

static void Fire_Transition(FireEvent ev, uint32_t now_ms)
{
	/* Единая точка обработки событий сценария пожара:
	 * FSM, автодедлайны, звук, LED. Состояние зон — Fire_FillV3Zones (POLL). */
	uint8_t fire_processed = 0u;
	uint8_t start_processed = 0u;

	switch (ev) {
	case FIRE_EVENT_STATUS_FIRE:
		g_fire.start_launch_pressed_latched = 0u;
		if (g_fire.state == FIRE_STATE_IDLE) {
			g_fire.state = FIRE_STATE_WAIT_AUTO;
			g_fire.reply_received = 0u;
		}
		Beeper_ResumeSoundOnNewEvent();
		fire_processed = 1u;
		break;
	case FIRE_EVENT_STATUS_FIRE_REDISPLAY:
		/* Повторный пожар в зоне, где тушение уже запускалось: только индикация. */
		if (g_fire.state == FIRE_STATE_IDLE) {
			g_fire.state = FIRE_STATE_WAIT_AUTO;
		}
		Beeper_ResumeSoundOnNewEvent();
		fire_processed = 1u;
		break;
	case FIRE_EVENT_REPLY_FIRE:
		g_fire.reply_received = 1u;
		break;
	case FIRE_EVENT_STOP_EXT:
		/* Команда StopExtinguishment с CAN: остановка таймеров/автопуска/дотушивания, без сброса слотов */
		Fire_LogForceStop(FIRE_LOG_STOP_OTHER, 0u);
		Fire_SendStopAllMcus();
		Fire_AbortExtinguishRetriesAll();
		g_fire.zone_countdown_stopped = 1u;
		g_fire.zone_countdown_paused = 0u;
		g_fire.start_launch_pressed_latched = 0u;
		g_fire.all_hold_active = 0u;
		g_fire.all_hold_ms = 0u;
		g_fire.sp_hold_active = 0u;
		g_fire.sp_hold_ms = 0u;
		g_fire.btn_start_all_hold_latched = 0u;
		if (Fire_CountPendingPhase2() > 0u) {
			g_fire.state = FIRE_STATE_WAIT_AUTO;
		}
		break;
	case FIRE_EVENT_BTN_START_SP:
		/* ПУСК СП: только при активных слотах пожара и есть зоны без фазы 2 */
		if (!Fire_AnyActiveSlot()) {
			break;
		}
		if (g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL ||
		    g_fire.state == FIRE_STATE_EXTINGUISHING) {
			uint8_t started = 0u;
			uint8_t log_zone = 0u;
#if GOST_MODE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				/* Выбранная зона (стрелки панели → ApplyPanelZoneSelect). */
				if (Fire_GetSelectedZoneFromUi(&sel_zone)) {
					started = Fire_Phase2SelectedPending(sel_zone);
					if (started != 0u) {
						log_zone = sel_zone;
					} else if (Fire_CountManualStartable() > 0u) {
						/* Выбранная уже ТУШ.ВЫП/не стартует — все awaiting
						 * (внешняя кнопка / zone=0; иначе второй пожар silent no-op). */
						started = Fire_Phase2AllPending();
					}
				} else if (Fire_CountManualStartable() > 0u) {
					/* Выбор сброшен (зона исчезла из UI) — все awaiting. */
					started = Fire_Phase2AllPending();
				}
			} else
#endif
			{
				if (Fire_CountManualStartable() == 0u) {
					break;
				}
				started = Fire_Phase2AllPending();
			}
			if (started == 0u) {
				break;
			}
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_START_SP, log_zone, 0u);
			}
			g_fire.start_launch_pressed_latched = 1u;
			g_fire.stop_launch_pressed_latched = 0u;
			Fire_SyncStateFromSlots();
			fire_processed = 1u;
			start_processed = 1u;
		}
		break;
	case FIRE_EVENT_BTN_START_ALL:
		/* ПУСК ОБЩИЙ: запуск тушения всех существующих зон с module_delay (zone_delay=0),
		 * независимо от статуса ПОЖАРА/слотов */
		{
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_START_ALL, 0u, 3u);
			}
			BroadcastSetStatusFire(0u, FIRE_STATUS_SRC_PPKU_START_ALL,
			                       DEVICE_PPKY_TYPE, 0u);
			uint8_t any_started = Fire_StartAllExistingZonesAndMarkSlots();
			/* Pending countdown + FIRE1/abort/launch_stopped — Phase2All.
			 * CountPendingPhase2 не видит fire1/launch_stopped → без этого
			 * ПУСК ОБЩИЙ при уже потушенных зонах молча no-op. */
			if (Fire_CountPendingPhase2() > 0u ||
			    Fire_CountManualStartable() > 0u) {
				if (Fire_Phase2AllPending() != 0u) {
					any_started = 1u;
				}
			}
			if (any_started) {
				g_fire.start_launch_pressed_latched = 1u;
				g_fire.stop_launch_pressed_latched = 0u;
				Fire_SyncStateFromSlots();
				fire_processed = 1u;
				start_processed = 1u;
			}
		}
		break;
	case FIRE_EVENT_BUS_START_EXT: {
		/* Пуск тушения с виртуальной кнопки (CAN StartExtinguishment broadcast). */
		uint8_t zone = g_fire_bus_cmd_zone;

		if (g_fire_bus_cmd_launch_type == START_EXT_DELAY_MODULE_ONLY) {
			uint8_t any_started = 0u;
			if (zone == 0u) {
				any_started = Fire_MarkExternalExtinguishAllZones(now_ms);
			} else {
				any_started = Fire_MarkExternalExtinguishZone(zone, now_ms);
			}
			if (any_started) {
				g_fire.start_launch_pressed_latched = 1u;
				g_fire.stop_launch_pressed_latched = 0u;
				Fire_SyncStateFromSlots();
				fire_processed = 1u;
				start_processed = 1u;
			}
		}
	} break;
	case FIRE_EVENT_BTN_STOP:
		/* ОСТАНОВ ПУСКА.
		 * GOST: по выбранной зоне — pause/resume таймера или стоп тушения.
		 * Ручные зоны без таймера: pause/resume no-op (см. Fire_Pause/ResumeZoneCountdown). */
		/* Не игнорировать ОСТАНОВ после ПУСК без phase2: иначе нельзя прервать
		 * уже идущее тушение/таймер (на ППКУ1 кнопки локальные — доходило всегда). */
		g_fire.start_launch_pressed_latched = 0u;
		if (!Fire_AnyActiveSlot()) {
			/* Нет активного слота: для ПОЖАР1 — только дежурный звук. */
			if (Fire_HasFire1Waiting()) {
				fire_processed = 1u;
				start_processed = 0u;
			}
			break;
		}
		{
#if GOST_MODE
			uint8_t sel_zone = 0u;
			uint8_t have_zone = 0u;
			if (g_fire_ui_manual_select_enabled) {
				have_zone = Fire_GetSelectedZoneFromUi(&sel_zone);
			}
			if (have_zone) {
				int8_t si = Fire_FindSlotZone(sel_zone);
				if (g_fire_panel_btn_source != 0u) {
					Fire_LogPanelButton(FIRE_LOG_BTN_STOP, sel_zone, 0u);
				}
				if (si >= 0) {
					FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
					if (s->phase2_sent && s->extinguish_aborted == 0u &&
					    s->ext_retry_failed == 0u &&
					    !Fire_ZoneAllIgnitersEndAck(sel_zone)) {
						Fire_LogForceStop(FIRE_LOG_STOP_OPERATOR, sel_zone);
						(void)Fire_AbortZoneExtinguishOperator(sel_zone, now_ms);
						g_fire.start_launch_pressed_latched = 0u;
						g_fire.stop_launch_pressed_latched = 1u;
						Fire_SyncStateFromSlots();
						fire_processed = 1u;
						break;
					}
					if (!s->phase2_sent && !s->fire1_waiting &&
					    s->extinguish_aborted == 0u) {
						if (s->countdown_paused != 0u) {
							Fire_ResumeZoneCountdown(sel_zone, now_ms);
							g_fire.stop_launch_pressed_latched = 0u;
						} else {
							Fire_PauseZoneCountdown(sel_zone, now_ms);
							g_fire.stop_launch_pressed_latched = 1u;
						}
						g_fire.start_launch_pressed_latched = 0u;
						Fire_SyncStateFromSlots();
						fire_processed = 1u;
						break;
					}
					/* ТУШ.ВЫП / ТУШ.ОСТ / ТУШ.ОШ / ПОЖАР1: короткий ОСТАНОВ
					 * не переводит в «ПОЖАР/ОСТ.ПУСКА» (сброс — удержанием ≥5 с). */
					break;
				}
			} else {
				/* zone=0 / без выбора: пауза/resume ВСЕХ countdown-слотов.
				 * Раньше брали первый active — часто EXT_DONE → silent no-op. */
				uint8_t any_cd = 0u;
				uint8_t any_done = 0u;
				uint8_t i;
				if (g_fire_panel_btn_source != 0u) {
					Fire_LogPanelButton(FIRE_LOG_BTN_STOP, 0u, 0u);
				}
				for (i = 0u; i < FIRE_MAX_SLOTS; i++) {
					FireZoneSlot *s = &g_fire.slots[i];
					if (!s->active || s->phase2_sent || s->fire1_waiting ||
					    s->launch_stopped || s->extinguish_aborted) {
						continue;
					}
					if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
						continue;
					}
					if (s->countdown_paused != 0u) {
						any_done = 1u;
					} else {
						any_cd = 1u;
					}
				}
				if (any_cd != 0u || any_done != 0u) {
					uint8_t do_pause = (any_cd != 0u) ? 1u : 0u;
					for (i = 0u; i < FIRE_MAX_SLOTS; i++) {
						FireZoneSlot *s = &g_fire.slots[i];
						if (!s->active || s->phase2_sent || s->fire1_waiting ||
						    s->launch_stopped || s->extinguish_aborted) {
							continue;
						}
						if (Fire_ZoneIsManual(s->zone) || Fire_ZoneLaunchBlocked(s->zone)) {
							continue;
						}
						if (do_pause != 0u) {
							if (s->countdown_paused == 0u) {
								Fire_PauseZoneCountdown(s->zone, now_ms);
							}
						} else if (s->countdown_paused != 0u) {
							Fire_ResumeZoneCountdown(s->zone, now_ms);
						}
					}
					g_fire.start_launch_pressed_latched = 0u;
					g_fire.stop_launch_pressed_latched = do_pause;
					Fire_SyncStateFromSlots();
					fire_processed = 1u;
				}
			}
			break;
#else
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_STOP, 0u, 0u);
			}
			Fire_EnterManualStop(now_ms, 1u, FIRE_LOG_STOP_OPERATOR);
			fire_processed = 1u;
#endif
		}
		break;
	case FIRE_EVENT_TICK_1MS:
	default:
		break;
	}

	if (Fire_ProcessAutoDeadlines(now_ms)) {
		g_fire.start_launch_pressed_latched = 1u;
		g_fire.stop_launch_pressed_latched = 0u;
		fire_processed = 1u;
		start_processed = 1u;
	}
	Fire_ProcessExtinguishRetries(now_ms);
	Fire_UpdateExtinguishAckTracking();
	Fire_SyncStateFromSlots();
	if (fire_processed != 0u && g_fire.state != FIRE_STATE_IDLE) {
		Beeper_ResumeSoundOnNewEvent();
	}
	(void)start_processed;

	/* Журнал «тушение произведено» — раньше срабатывал в ветке MAIN_FIRE ui_mode. */
	if (g_fire.state == FIRE_STATE_EXTINGUISHING && Fire_AllActiveZonesEndAck()) {
		Fire_LogExtinguishComplete();
	}
}

/* Инициализация модуля пожара: сброс FSM, слотов и базовой индикации. */
void Fire_Init(void)
{
	/* Инициализация контекста пожара; полный сброс слотов только при старте/перезапуске. */
	memset(&g_fire, 0, sizeof(g_fire));
	Fire_RetryCancelAll();
	/* Полный сброс слотов только при перезапуске ППКУ */
	Fire_ClearAllSlots();
#if GOST_MODE
	Fire_ResetSuppressClear();
#endif
	g_fire.state = FIRE_STATE_IDLE;
	g_fire_ui_manual_select_enabled = 0u;
	g_fire_ui_selected_index = 0u;
}

/* Периодический тик 1 мс: FSM, таймеры автопуска, LED/звук. */
void Fire_Timer1ms(void)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	/* 1мс-путь: крутит FSM при активном сценарии или удержании ПУСК ОБЩИЙ. */
	uint32_t now = HAL_GetTick();
	Fire_ApplyFireModePolicy(now);
	if (Fire_PromoteFire1WhenAndUnavailable(now)) {
		/* Обновляем индикацию/звук тем же событием, что и при штатном входе пожара. */
		Fire_Transition(FIRE_EVENT_STATUS_FIRE, now);
	}
	Fire_RetryProcess(now);
	if (g_fire.state == FIRE_STATE_IDLE && !Fire_AnyActiveSlot() && !g_fire.all_hold_active) {
		return;
	}
	Fire_Transition(FIRE_EVENT_TICK_1MS, now);
}

/* Периодический тик 10 мс: обработка кнопок и удержания ПУСК ОБЩИЙ. */
void Fire_Timer10ms(void)
{
	uint32_t now_ms = HAL_GetTick();
	uint8_t panel_small = RsPanelMaster_IsSmallPanel();

	/* ПУСК ОБЩИЙ обрабатываем даже во время config-сессии: удержание поднимает
	 * all_hold_active, UI возвращает на главный экран со счётчиком 3с. */
	if (panel_small == 0u) {
		ButtonState st_start_all = Button_GetState(BUT_FORCE);
		uint8_t start_all_down =
		    (st_start_all == ButtonStatePress || st_start_all == ButtonStateLongPress) ? 1u : 0u;
		if (start_all_down != 0u) {
			if (!g_fire.all_hold_active) {
				g_fire.all_hold_active = 1u;
				g_fire.all_hold_ms = 0u;
				g_fire.state_start_ms = now_ms;
				/* Сразу включить мигание кнопки/подписи — до первого Fire_Transition. */
				/* Сырое касание ПУСК ОБЩИЙ — до удержания 3 с (код 14 после hold). */
				Fire_LogPanelBtnPress(FIRE_LOG_BTN_START_ALL, 0u);
			} else {
				if (g_fire.all_hold_ms < FIRE_START_ALL_HOLD_MS) {
					g_fire.all_hold_ms = (uint16_t)(g_fire.all_hold_ms + 10u);
				}
				if (g_fire.all_hold_ms >= FIRE_START_ALL_HOLD_MS &&
				    g_fire.btn_start_all_hold_latched == 0u) {
					g_fire.btn_start_all_hold_latched = 1u;
					if (g_fire.last_btn_start_all_action_ms == 0u ||
					    (now_ms - g_fire.last_btn_start_all_action_ms) >= 500u) {
						g_fire.last_btn_start_all_action_ms = now_ms;
						g_fire_panel_btn_source = 1u;
						Fire_Transition(FIRE_EVENT_BTN_START_ALL, now_ms);
						g_fire_panel_btn_source = 0u;
					}
				}
			}
		} else if (g_fire.all_hold_active) {
			/* Отпускание / сбой чтения: любой не-Press отменяет отсчёт.
			 * Раньше только ButtonStateReset — при потере RELEASE на RS таймер
			 * продолжал и всё равно выполнял общий пуск. */
			g_fire.all_hold_active = 0u;
			g_fire.all_hold_ms = 0u;
			g_fire.btn_start_all_hold_latched = 0u;
			/* Явно вернуть дежурный вид: при отпускании в фазе blink_off
			 * на панели могли остаться BUT/STR=OFF и full brightness. */
			/* Повторный push: один кадр LED легко теряется на half-duplex. */
			Fire_Transition(FIRE_EVENT_TICK_1MS, now_ms);
		}
	}

#if GOST_MODE
	/* Удержание ОСТАНОВ ≥5 с: полный сброс пожара (слоты + таймеры + UI). */
	{
		ButtonState st_stop_hold = Button_GetState(BUT_STOP);
		uint8_t fire_present = (Fire_AnyActiveSlot() || g_fire.state != FIRE_STATE_IDLE) ? 1u : 0u;
		if ((st_stop_hold == ButtonStatePress || st_stop_hold == ButtonStateLongPress) &&
		    (fire_present != 0u || g_fire.gost_stop_hold_active != 0u)) {
			if (g_fire.gost_stop_hold_active == 0u) {
				g_fire.gost_stop_hold_active = 1u;
				g_fire.gost_stop_hold_ms = 0u;
				g_fire.gost_stop_reset_latched = 0u;
			} else {
				if (g_fire.gost_stop_hold_ms < FIRE_GOST_STOP_RESET_HOLD_MS) {
					g_fire.gost_stop_hold_ms += 10u;
				}
				if (g_fire.gost_stop_hold_ms >= FIRE_GOST_STOP_RESET_HOLD_MS &&
				    g_fire.gost_stop_reset_latched == 0u &&
				    (Fire_AnyActiveSlot() || g_fire.state != FIRE_STATE_IDLE)) {
					g_fire.gost_stop_reset_latched = 1u;
					g_fire_panel_btn_source = 1u;
					Fire_GostResetFire(HAL_GetTick());
					g_fire_panel_btn_source = 0u;
				}
			}
		} else if (g_fire.gost_stop_hold_active != 0u) {
			/* Отпускание / Error: сброс удержания сброса пожара (как у ПУСК ОБЩИЙ). */
			g_fire.gost_stop_hold_active = 0u;
			g_fire.gost_stop_hold_ms = 0u;
			g_fire.gost_stop_reset_latched = 0u;
		}
	}
#endif

	/* Малая панель: ПУСК СП с длинным удержанием = ПУСК ОБЩИЙ. */
	if (panel_small != 0u) {
		ButtonState st_sp = Button_GetState(BUT_FIRE);
		uint8_t sp_down =
		    (st_sp == ButtonStatePress || st_sp == ButtonStateLongPress) ? 1u : 0u;
		if (sp_down != 0u) {
			if (g_fire.sp_hold_active == 0u) {
				g_fire.sp_hold_active = 1u;
				g_fire.sp_hold_ms = 0u;
				g_fire.btn_start_all_hold_latched = 0u;
				Fire_LogPanelBtnPress(FIRE_LOG_BTN_START_SP, 0u);
			} else if (g_fire.sp_hold_ms < FIRE_START_ALL_HOLD_MS) {
				g_fire.sp_hold_ms = (uint16_t)(g_fire.sp_hold_ms + 10u);
			}
			if (g_fire.sp_hold_ms >= FIRE_SP_TO_ALL_HOLD_MS) {
				if (g_fire.all_hold_active == 0u) {
					g_fire.all_hold_active = 1u;
					g_fire.state_start_ms = now_ms;
					Fire_LogPanelBtnPress(FIRE_LOG_BTN_START_ALL, 0u);
				}
				g_fire.all_hold_ms = g_fire.sp_hold_ms;
				if (g_fire.sp_hold_ms >= FIRE_START_ALL_HOLD_MS &&
				    g_fire.btn_start_all_hold_latched == 0u) {
					g_fire.btn_start_all_hold_latched = 1u;
					if (g_fire.last_btn_start_all_action_ms == 0u ||
					    (now_ms - g_fire.last_btn_start_all_action_ms) >= 500u) {
						g_fire.last_btn_start_all_action_ms = now_ms;
						g_fire_panel_btn_source = 1u;
						Fire_Transition(FIRE_EVENT_BTN_START_ALL, now_ms);
						g_fire_panel_btn_source = 0u;
					}
				}
			}
		} else if (g_fire.sp_hold_active != 0u) {
			uint16_t held = g_fire.sp_hold_ms;
			g_fire.sp_hold_active = 0u;
			g_fire.sp_hold_ms = 0u;
			if (g_fire.all_hold_active != 0u) {
				g_fire.all_hold_active = 0u;
				g_fire.all_hold_ms = 0u;
				g_fire.btn_start_all_hold_latched = 0u;
				Fire_Transition(FIRE_EVENT_TICK_1MS, now_ms);
			}
			/* <1с: обычный ПУСК СП. ≥1с и отпустили до 3с: ничего. */
			if (held < FIRE_SP_TO_ALL_HOLD_MS &&
			    MenuUi_IsConfigSessionActive() == 0u &&
			    (g_fire.state != FIRE_STATE_IDLE || Fire_AnyActiveSlot())) {
				if (g_fire.last_btn_start_sp_action_ms == 0u ||
				    (now_ms - g_fire.last_btn_start_sp_action_ms) >= 500u) {
					g_fire.last_btn_start_sp_action_ms = now_ms;
					g_fire_panel_btn_source = 1u;
					Fire_Transition(FIRE_EVENT_BTN_START_SP, now_ms);
					g_fire_panel_btn_source = 0u;
				}
			}
		}
	}

	/* Сырое нажатие ПУСК СП / ОСТАНОВ — всегда (в т.ч. IDLE и config).
	 * На малой панели ПУСК СП обрабатывается удержанием выше. */
	uint8_t pressed_sp = 0u;
	if (panel_small == 0u) {
		pressed_sp = Fire_ButtonPressedEvent(BUT_FIRE, &g_fire.btn_start_sp_latched);
	}
	uint8_t pressed_stop = Fire_ButtonPressedEvent(BUT_STOP, &g_fire.btn_stop_latched);
	if (pressed_sp != 0u || pressed_stop != 0u) {
		uint8_t press_zone = 0u;
#if GOST_MODE
		if (g_fire_ui_manual_select_enabled) {
			(void)Fire_GetSelectedZoneFromUi(&press_zone);
		}
#endif
		if (pressed_sp != 0u) {
			Fire_LogPanelBtnPress(FIRE_LOG_BTN_START_SP, press_zone);
		}
		if (pressed_stop != 0u) {
			Fire_LogPanelBtnPress(FIRE_LOG_BTN_STOP, press_zone);
		}
	}

	if (MenuUi_IsConfigSessionActive()) {
		return;
	}

	/* ОСТАНОВ — до early-return по IDLE: иначе при гонке слотов событие терялось. */
	if (pressed_stop != 0u) {
		g_fire_panel_btn_source = 1u;
		Fire_Transition(FIRE_EVENT_BTN_STOP, now_ms);
		g_fire_panel_btn_source = 0u;
	}

	if (g_fire.state == FIRE_STATE_IDLE && !Fire_AnyActiveSlot()) {
		return;
	}

	if (pressed_sp != 0u) {
		if (g_fire.last_btn_start_sp_action_ms == 0u ||
		    (now_ms - g_fire.last_btn_start_sp_action_ms) >= 500u) {
			g_fire.last_btn_start_sp_action_ms = now_ms;
			g_fire_panel_btn_source = 1u;
			Fire_Transition(FIRE_EVENT_BTN_START_SP, now_ms);
			g_fire_panel_btn_source = 0u;
		}
	}
}

/* Входящее событие ПОЖАР от МКУ: добавляет зону и запускает сценарий. */
void Fire_OnStatusFire(uint32_t msg_id, const uint8_t *msg_data)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	/* Вход статуса пожара от МКУ: учитываем уникальные источники в зоне
	 * и поддерживаем ПОЖАР1/ПОЖАР2 для режима fire_and[]. */
	can_ext_id_t id;
	id.ID = msg_id & 0x0FFFFFFF;
	/* zone в формате CAN (обычно 1..N), без декремента:
	 * именно это значение нужно для адресации МКУ этой зоны. */
	uint8_t zone = (uint8_t)(id.field.zone & 0x7Fu);
	uint32_t source_key = Fire_SourceKeyFromFireMsg(msg_id, msg_data);
	uint32_t now = HAL_GetTick();

	/* Mode 4: лог + ACK МКУ, без слота/звука/сценария. */
	if (PPKY_ZoneFireBlockedByCanZone(zone) != 0u) {
#if GOST_MODE
		if (Fire_ResetSuppressHas(zone, source_key) == 0u) {
			Fire_LogFireDetected(msg_id, msg_data, zone);
			Fire_ResetSuppressAdd(zone, source_key);
		}
#else
		Fire_LogFireDetected(msg_id, msg_data, zone);
#endif
		SetReplyStatusFire(zone);
		return;
	}

	{
		uint8_t and_effective = Fire_IsAndEffectiveForZone(zone);
		uint8_t res = Fire_TryAddNewFireZone(zone, source_key, and_effective, now,
		                                    msg_id, msg_data);
		if (res == 1u || res == 2u) {
			Fire_LogFireDetected(msg_id, msg_data, zone);
			Fire_Transition(FIRE_EVENT_STATUS_FIRE, now);
		} else if (res == 3u) {
			Fire_Transition(FIRE_EVENT_STATUS_FIRE_REDISPLAY, now);
		}
	}
	SetReplyStatusFire(zone);
}

static volatile uint32_t bus_sp_cnt = 0;

void Fire_OnBusStartSpButton(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	/* MKU button StartSP: broadcast d_type/h/l=0, cmd 163. */
	if (!Backend_IsIgniterBroadcastId(msg_id)) {
		return;
	}
	bus_sp_cnt++;
	/* Тот же вход, что панель с zone=0 (все awaiting, без стрелочного выбора). */
	Fire_OnPanelStartSp(0u);
}

void Fire_OnStartExtinguishment(uint32_t msg_id, const uint8_t *msg_data)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	if (msg_data == NULL) {
		return;
	}
	if (!Backend_IsIgniterBroadcastId(msg_id)) {
		return;
	}
	if (msg_data[0] != (uint8_t)ServiceCmd_Fire_StartExtinguishment) {
		return;
	}
	if (msg_data[4] != START_EXT_DELAY_MODULE_ONLY) {
		return;
	}

	g_fire_bus_cmd_zone = msg_data[1] & 0x7Fu;
	g_fire_bus_cmd_launch_type = msg_data[4];
	Fire_Transition(FIRE_EVENT_BUS_START_EXT, HAL_GetTick());
}

/* Входящий ReplyStatusFire от МКУ (подтверждение статуса пожара). */
void Fire_OnReplyStatusFire(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	(void)msg_id;
	Fire_Transition(FIRE_EVENT_REPLY_FIRE, HAL_GetTick());
}

/* Входящая команда StopExtinguishment от МКУ/CAN. */
void Fire_OnStopExtinguishment(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	(void)msg_id;
	Fire_Transition(FIRE_EVENT_STOP_EXT, HAL_GetTick());
}

void Fire_OnPauseExtinguishmentTimer(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	(void)msg_id;
	Fire_PauseCountdownAndDispatch(HAL_GetTick());
}

void Fire_OnResumeExtinguishmentTimer(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	(void)msg_id;
	Fire_ResumeCountdownAndDispatch(HAL_GetTick());
}

void Fire_OnReplyStartExtinguishment(uint32_t msg_id)
{
	Fire_RetryAckByMsgId(FIRE_RETRY_START, msg_id);
}

void Fire_OnReplyStopExtinguishment(uint32_t msg_id)
{
	Fire_RetryAckByMsgId(FIRE_RETRY_STOP, msg_id);
}

void Fire_OnReplyPauseExtinguishmentTimer(uint32_t msg_id)
{
	Fire_RetryAckByMsgId(FIRE_RETRY_PAUSE, msg_id);
}

void Fire_OnReplyResumeExtinguishmentTimer(uint32_t msg_id)
{
	Fire_RetryAckByMsgId(FIRE_RETRY_RESUME, msg_id);
}

/* Возвращает 1, если пожарный сценарий сейчас активен. */
uint8_t Fire_IsActive(void)
{
	/* Пожар считается активным, пока FSM не в IDLE или есть активные слоты зон. */
	return (g_fire.state != FIRE_STATE_IDLE || Fire_AnyActiveSlot()) ? 1u : 0u;
}

uint8_t Fire_HasExtinguishIncomplete(void)
{
	return Fire_HasExtinguishFailure();
}

uint8_t Fire_IsStartAllHoldActive(void)
{
	/* ПУСК ОБЩИЙ или удержание ПУСК СП на малой (до/после порога 1 с). */
	return (g_fire.all_hold_active || g_fire.sp_hold_active) ? 1u : 0u;
}

uint8_t Fire_IsExtinguishIndicationActive(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		const FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent == 0u) {
			continue;
		}
		if (s->extinguish_aborted != 0u || s->ext_retry_failed != 0u) {
			continue;
		}
		if (!Fire_ZoneAllIgnitersEndAck(s->zone)) {
			return 1u;
		}
	}
	return 0u;
}

void Fire_UiSetManualSelection(uint8_t enabled, uint8_t selected_ui_index)
{
	uint8_t en = enabled ? 1u : 0u;
	if (g_fire_ui_manual_select_enabled == en &&
	    g_fire_ui_selected_index == selected_ui_index) {
		return;
	}
	g_fire_ui_manual_select_enabled = en;
	g_fire_ui_selected_index = selected_ui_index;
}

uint8_t Fire_UiGetSelectedIndex(void)
{
	return g_fire_ui_selected_index;
}

void Fire_NotifyZoneModeChanged(void)
{
	/* Применить zone_fire_mode[] к активным слотам (ручной/блок → без таймера). */
	Fire_ApplyFireModePolicy(HAL_GetTick());
	Fire_SyncStateFromSlots();
}

static void Fire_ApplyPanelZoneSelect(uint8_t zone)
{
	if (zone == 0u) {
		g_fire_ui_manual_select_enabled = 0u;
		g_fire_ui_selected_index = 0u;
		return;
	}
	{
		uint8_t zones[FIRE_UI_MAX_ZONES];
		uint8_t n = Fire_BuildUiZoneList(zones, FIRE_UI_MAX_ZONES);
		uint8_t i;
		for (i = 0u; i < n; i++) {
			if (zones[i] == zone) {
				g_fire_ui_manual_select_enabled = 1u;
				g_fire_ui_selected_index = i;
				return;
			}
		}
	}
	g_fire_ui_manual_select_enabled = 0u;
}

void Fire_OnPanelStartAllCommit(void)
{
	uint32_t now = HAL_GetTick();
	g_fire_panel_btn_source = 1u;
	Fire_Transition(FIRE_EVENT_BTN_START_ALL, now);
	g_fire_panel_btn_source = 0u;
}
static volatile uint32_t sp_cnt = 0;
void Fire_OnPanelStartSp(uint8_t zone)
{
	sp_cnt++;
	uint32_t now = HAL_GetTick();
	Fire_ApplyPanelZoneSelect(zone);
	g_fire_panel_btn_source = 1u;
	Fire_Transition(FIRE_EVENT_BTN_START_SP, now);
	g_fire_panel_btn_source = 0u;
}

void Fire_OnPanelStopLaunch(uint8_t zone)
{
	uint32_t now = HAL_GetTick();
	Fire_ApplyPanelZoneSelect(zone);
	g_fire_panel_btn_source = 1u;
	Fire_Transition(FIRE_EVENT_BTN_STOP, now);
	g_fire_panel_btn_source = 0u;
}

void Fire_OnPanelFireReset(uint8_t zone)
{
	uint32_t now = HAL_GetTick();
	Fire_ApplyPanelZoneSelect(zone);
#if GOST_MODE
	g_fire_panel_btn_source = 1u;
	Fire_GostResetFire(now);
	g_fire_panel_btn_source = 0u;
#else
	(void)now;
	(void)zone;
#endif
}

void Fire_FillV3Zones(RsPanelV3Zones *out, uint32_t now_ms)
{
	/* ZONES TLV: zone/status/remaining + источник (mcu_h_adr/ch_type/ch_l_adr).
	 * Имена зон — каталог ZONE_NAMES; суффикс «МКУ N ДПТk» собирает панель. */
	uint8_t zm[FIRE_UI_MAX_ZONES];
	uint8_t zr[FIRE_UI_MAX_ZONES];
	uint8_t zones[FIRE_UI_MAX_ZONES];
	uint8_t nz;
	uint8_t i;

	if (out == 0) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (g_fire.state == FIRE_STATE_IDLE &&
	    !(g_fire.all_hold_active && g_fire.all_hold_ms < FIRE_START_ALL_HOLD_MS)) {
		return;
	}
	if (g_fire.all_hold_active != 0u && g_fire.all_hold_ms < FIRE_START_ALL_HOLD_MS) {
		return;
	}
	nz = Fire_BuildUiZoneList(zones, FIRE_UI_MAX_ZONES);
	if (nz > RS_PANEL_V3_MAX_ZONES) {
		nz = RS_PANEL_V3_MAX_ZONES;
	}
	for (i = 0u; i < nz; i++) {
		int8_t si;
		Fire_UiStatusForZone(zones[i], now_ms, &zm[i], &zr[i]);
		out->items[i].zone = zones[i];
		out->items[i].status = zm[i];
		out->items[i].remaining_s = zr[i];
		out->items[i].mcu_h_adr = 0u;
		out->items[i].ch_type = 0u;
		out->items[i].ch_l_adr = 0u;
		/* Первый источник слота → суффикс бегущей на панели (как неисправности). */
		si = Fire_FindSlotZoneExact(zones[i]);
		if (si >= 0) {
			const FireZoneSlot *s = &g_fire.slots[(uint8_t)si];
			if (s->source_count > 0u) {
				const uint32_t key = s->source_keys[0];
				out->items[i].ch_type = (uint8_t)((key >> 14) & 0x7Fu);
				out->items[i].mcu_h_adr = (uint8_t)((key >> 6) & 0xFFu);
				out->items[i].ch_l_adr = (uint8_t)(key & 0x3Fu);
			}
		}
	}
	out->count = nz;
}
