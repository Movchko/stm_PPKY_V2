#include "fire.h"

#include "main.h"
#include "button.h"
#include "beeper.h"
#include "led.h"
#include "backend.h"
#include "device_config.h"
#include "sound_profiles.h"
#include <string.h>
#include <stdio.h>
#include "app.hpp"

extern PPKYCfg PPKYConfig;
extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;

/* Отладка: зона 1 → индекс 0; пока 3 зоны, далее — из конфига МКУ */
#define FIRE_DEBUG_ZONES     3u
#define FIRE_MAX_SLOTS       16u
#define FIRE_UI_MAX_ZONES    16u
#define FIRE_UI_NAME_LEN     (ZONE_NAME_SIZE + 1u)
#define FIRE_STOP_TEXT_BLINK_PERIOD_MS       1600u
#define FIRE_START_SP_TEXT_BLINK_PERIOD_MS   1600u
#define FIRE_START_ALL_TEXT_BLINK_PERIOD_MS  1600u
#define FIRE_START_ALL_SOUND_PERIOD_MS       SOUND_START_ALL_HOLD_PERIOD_MS
#define FIRE_START_ALL_SOUND_DUTY_MS         SOUND_START_ALL_HOLD_DUTY_MS
#define FIRE_START_LED_HOLD_MS               3000u
/* ack_flags у IGNITER: предполагаем бит 1 = end_ack */
#define FIRE_IGNITER_END_ACK_MASK            0x02u
#define FIRE_CMD_RETRY_TIMEOUT_MS            200u
#define FIRE_CMD_RETRY_TIMEOUT_STOP_RETRY_S	10
#define FIRE_CMD_RETRY_MAX_ATTEMPTS          (FIRE_CMD_RETRY_TIMEOUT_STOP_RETRY_S * 1000 / FIRE_CMD_RETRY_TIMEOUT_MS)//    10u
#define FIRE_CMD_RETRY_MAX_ITEMS             64u
/* Спецрежим: в ручном выборе пожара кнопки ПУСК СП/СТОП действуют только на выбранную зону. */
#define FIRE_SELECTED_ZONE_BUTTONS_ENABLE    1u
#define FIRE_MAX_SOURCES_PER_ZONE            8u

static uint8_t debug_zone_delay[FIRE_DEBUG_ZONES] = { 15, 30u, 30 };
static uint8_t debug_module_delay[FIRE_DEBUG_ZONES][2] = {
	{ 0u, 0u },
	{ 0u, 5u },
	{ 5u, 10u },
};

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
	FIRE_EVENT_TICK_1MS
} FireEvent;

typedef struct {
	uint8_t  zone;
	uint8_t  active;
	uint8_t  phase2_sent;
	uint8_t  fire1_waiting;
	uint8_t  source_count;
	uint32_t phase2_deadline_ms;
	uint32_t paused_remaining_ms;
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
	uint32_t  state_start_ms;
	uint32_t  led_toggle_ms;
	uint8_t   beeper_alert_active;
	uint8_t   beeper_duty_active;
	uint8_t   beeper_start_pattern_active;
	uint8_t   start_all_hold_sound_active;
	uint32_t  start_pattern_started_ms;
	uint32_t  start_led_hold_until_ms;
	uint32_t  stop_text_blink_until_ms;
	uint32_t  start_sp_text_blink_until_ms;
	uint8_t   stop_launch_pressed_latched;
	uint8_t   start_launch_pressed_latched;
	uint8_t   led_fire_on;
	uint8_t   btn_start_all_hold_latched;
	uint8_t   btn_start_sp_latched;
	uint8_t   btn_stop_latched;
	uint8_t   start_all_is_bright;
	uint8_t   last_ui_active;
	uint8_t   last_ui_mode;
	uint8_t   last_ui_remaining;
	uint8_t   last_ui_nzones;
	uint8_t   last_fire_mode;
	uint32_t  last_ui_force_names_ms;
	char      last_ui_names[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
	/* После «ОСТАНОВ ПУСКА»: авто-отсчёт зон и его отображение отключены, слоты пожара сохраняются */
	uint8_t   zone_countdown_stopped;
	/* После команды PauseExtinguishmentTimer: таймеры зон "заморожены". */
	uint8_t   zone_countdown_paused;
	FireZoneSlot slots[FIRE_MAX_SLOTS];
} FireContext;

static FireContext g_fire;
static FireRetryItem g_fire_retry_items[FIRE_CMD_RETRY_MAX_ITEMS];
static uint8_t g_fire_ui_manual_select_enabled = 0u;
static uint8_t g_fire_ui_selected_index = 0u;

/* Управляет яркостью кнопки/подписи ПУСК ОБЩИЙ (обычная/активная). */
static void Fire_SetStartAllBrightness(uint8_t bright);
/* Отправляет широкую команду STOP всем МКУ пожарного контура. */
static void Fire_SendStopAllMcus(void);
/* Отправляет фазу 1 запуска по конкретной зоне. */
static void Fire_SendPhase1Zone(uint8_t zone);
/* Отправляет фазу 2 запуска по конкретной зоне. */
static void Fire_SendPhase2Zone(uint8_t zone);
/* Запускает фазу 2 по всем слотам, где она ещё не отправлялась. */
static void Fire_Phase2AllPending(void);
/* Полностью очищает слоты пожара и флаги остановки таймеров. */
static void Fire_ClearAllSlots(void);
/* Синхронизирует состояние FSM исходя из состояния слотов/фаз. */
static void Fire_SyncStateFromSlots(void);
/* Нормализует номер зоны в диапазон debug-порогов. */
static uint8_t Fire_DebugZoneIndex(uint8_t zone);
/* Переводит номер зоны CAN (1..N) в индекс массива имён (0..N-1). */
static uint8_t Fire_ZoneCanToIdx(uint8_t zone_can);
/* Возвращает задержку зоны (сек) перед фазой 2. */
static uint8_t Fire_ZoneDelaySec(uint8_t zone);
/* Возвращает задержки модулей внутри зоны (две линии). */
static void Fire_GetModuleDelays(uint8_t zone, uint8_t *m0, uint8_t *m1);
/* Ищет слот по номеру зоны, возвращает индекс или -1. */
static int8_t Fire_FindSlotZone(uint8_t zone);
/* Выделяет свободный слот пожара, возвращает индекс или -1. */
static int8_t Fire_AllocSlot(void);
/** @return 1 если зона добавлена впервые в цикле, 0 если по этой зоне пожар уже учтён */
static uint8_t Fire_TryAddNewFireZone(uint8_t zone, uint32_t source_key, uint8_t and_effective, uint32_t now_ms);
/* Операции с "И"-логикой пожара и источниками зоны. */
static uint8_t Fire_IsAndEnabledForZone(uint8_t zone);
static uint8_t Fire_CountOnlineDptForZone(uint8_t zone);
static uint8_t Fire_IsAndEffectiveForZone(uint8_t zone);
static uint32_t Fire_SourceKeyFromMsgId(uint32_t msg_id);
static uint8_t Fire_AddSourceToSlot(FireZoneSlot *slot, uint32_t source_key);
static uint8_t Fire_HasFire1Waiting(void);
static uint8_t Fire_ZoneIsFire1Waiting(uint8_t zone);
static uint8_t Fire_ShouldUseFire1Sound(void);
static uint8_t Fire_PromoteFire1WhenAndUnavailable(uint32_t now_ms);
/* Есть ли хотя бы один активный слот пожара. */
static uint8_t Fire_AnyActiveSlot(void);
/* Считает зоны, где фаза 2 ещё не отправлена. */
static uint8_t Fire_CountPendingPhase2(void);
/* Минимальный оставшийся таймер (сек) среди pending-зон. */
static uint8_t Fire_MinRemainingSec(uint32_t now_ms);
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
/* Формирует список имён зон для UI (уникальные, отсортированные). */
static void Fire_FillZoneNamesForUi(char (*out_names)[FIRE_UI_NAME_LEN], uint8_t *out_n);
/* Формирует список zone CAN (по тем же правилам, что и Fire_FillZoneNamesForUi). */
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
static void Fire_SendTemporaryRelayToggleByZone(uint8_t zone);
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
/* Переводит пищалку в тревожный режим ПОЖАР1/ПОЖАР2. */
static void Fire_BeeperEnterAlert(uint8_t fire1_sound);
/* Переводит пищалку в дежурный режим ПОЖАР1/ПОЖАР2. */
static void Fire_BeeperEnterDuty(uint8_t fire1_sound);
/* Прерывистый звук для пуска (скважность 2, период 2.4с). */
static void Fire_BeeperEnterStartPattern(uint32_t now_ms);
/* Тик/переключение между паттернами. */
static void Fire_BeeperTick(uint32_t now_ms);
/* Звук подтверждения удержания ПУСК ОБЩИЙ (1.6с, скважность 2). */
static void Fire_StartAllHoldSoundOn(void);
static void Fire_StartAllHoldSoundOff(void);
static uint8_t Fire_Phase2SelectedPending(uint8_t zone);
static void Fire_SendStopZone(uint8_t zone);
static uint8_t Fire_GetSelectedZoneFromUi(uint8_t *zone);
static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text);
static void Fire_ApplyFireModePolicy(uint32_t now_ms);
static void Fire_PauseCountdownAndDispatch(uint32_t now_ms);
static void Fire_ResumeCountdownAndDispatch(uint32_t now_ms);

extern void Fire_UiUpdate(uint8_t active, uint8_t mode, uint8_t remaining_s, uint8_t n_zones,
			  char (*zone_names)[FIRE_UI_NAME_LEN]);

static void Fire_BeeperEnterAlert(uint8_t fire1_sound)
{
	/* Сигнальный режим: для ПОЖАР1 отдельный паттерн, для ПОЖАР2 штатный сигнал. */
	g_fire.beeper_alert_active = 1u;
	g_fire.beeper_duty_active = 0u;
	g_fire.beeper_start_pattern_active = 0u;
	Beeper_StopPattern();
	if (fire1_sound) {
		Beeper_ContinuousOff();
		Beeper_StartPulseTrain(SOUND_FIRE1_SIGNAL_ON_MS, SOUND_FIRE1_SIGNAL_OFF_MS,
				       SOUND_FIRE1_SIGNAL_PULSES, SOUND_FIRE1_SIGNAL_REPEAT_MS);
	} else {
		Beeper_FireAlarmOn();
	}
}

static void Fire_BeeperEnterDuty(uint8_t fire1_sound)
{
	/* После обработки пожара: дежурный профиль зависит от ПОЖАР1/ПОЖАР2. */
	g_fire.beeper_alert_active = 0u;
	g_fire.beeper_duty_active = 1u;
	g_fire.beeper_start_pattern_active = 0u;
	Beeper_StopPattern();
	if (fire1_sound) {
		Beeper_StartPulseTrain(SOUND_FIRE1_DUTY_ON_MS, SOUND_FIRE1_DUTY_OFF_MS,
				       SOUND_FIRE1_DUTY_PULSES, SOUND_FIRE1_DUTY_REPEAT_MS);
	} else {
		Beeper_StartPulseTrain(BEEPER_PATTERN_FIRE_ON_MS, BEEPER_PATTERN_FIRE_OFF_MS,
				       BEEPER_PATTERN_FIRE_PULSES, BEEPER_PATTERN_FIRE_REPEAT_MS);
	}
}

static void Fire_BeeperEnterStartPattern(uint32_t now_ms)
{
	(void)now_ms;
	/* ПУСК: прерывистый звук  */
	g_fire.beeper_alert_active = 0u;
	g_fire.beeper_duty_active = 0u;
	g_fire.beeper_start_pattern_active = 1u;
	g_fire.start_pattern_started_ms = HAL_GetTick();
	Beeper_ContinuousOff();
	Beeper_StartPulseTrain(BEEPER_PATTERN_START_ON_MS, BEEPER_PATTERN_START_OFF_MS,
			       BEEPER_PATTERN_START_PULSES, BEEPER_PATTERN_START_REPEAT_MS);
}

static void Fire_BeeperTick(uint32_t now_ms)
{
	if (g_fire.start_all_hold_sound_active) {
		return;
	}
	if (g_fire.beeper_alert_active) {
		return;
	}
	if (g_fire.beeper_start_pattern_active && Fire_AllActiveZonesEndAck()) {
		g_fire.beeper_start_pattern_active = 0u;
		g_fire.start_led_hold_until_ms = now_ms + FIRE_START_LED_HOLD_MS;
		Fire_BeeperEnterDuty(Fire_ShouldUseFire1Sound());
	}
}

static void Fire_StartAllHoldSoundOn(void)
{
	if (g_fire.start_all_hold_sound_active) {
		return;
	}
	g_fire.start_all_hold_sound_active = 1u;
	Beeper_ContinuousOff();
	/* Непрерывное мигание/звук 0.8/0.8с без дополнительной паузы между циклами. */
	Beeper_StartPulseTrain(FIRE_START_ALL_SOUND_DUTY_MS, FIRE_START_ALL_SOUND_DUTY_MS, 1u, 0u);
}

static void Fire_StartAllHoldSoundOff(void)
{
	if (!g_fire.start_all_hold_sound_active) {
		return;
	}
	g_fire.start_all_hold_sound_active = 0u;
	Beeper_StopPattern();
	if (g_fire.beeper_alert_active) {
		Beeper_StartPulseTrain(BEEPER_PATTERN_FIRE_ON_MS, BEEPER_PATTERN_FIRE_OFF_MS,
				       BEEPER_PATTERN_FIRE_PULSES, BEEPER_PATTERN_FIRE_REPEAT_MS);
	} else if (g_fire.beeper_start_pattern_active) {
		Beeper_StartPulseTrain(BEEPER_PATTERN_START_ON_MS, BEEPER_PATTERN_START_OFF_MS,
				       BEEPER_PATTERN_START_PULSES, BEEPER_PATTERN_START_REPEAT_MS);
	} else if (g_fire.beeper_duty_active) {
		Beeper_StartPulseTrain(BEEPER_PATTERN_FIRE_ON_MS, BEEPER_PATTERN_FIRE_OFF_MS,
				       BEEPER_PATTERN_FIRE_PULSES, BEEPER_PATTERN_FIRE_REPEAT_MS);
	}
}

static uint8_t Fire_ZoneCanToIdx(uint8_t zone_can)
{
	/* В CAN зоне обычно приходят как 1..N; в UI/массивах имён используем 0..N-1. */
	if (zone_can == 0u) {
		return 0u;
	}
	return (uint8_t)(zone_can - 1u);
}

static uint8_t Fire_DebugZoneIndex(uint8_t zone)
{
	uint8_t idx = Fire_ZoneCanToIdx(zone);
	if (idx < FIRE_DEBUG_ZONES) {
		return idx;
	}
	return FIRE_DEBUG_ZONES - 1u;
}

static uint8_t Fire_ZoneDelaySec(uint8_t zone)
{
	return debug_zone_delay[Fire_DebugZoneIndex(zone)];
}

static void Fire_GetModuleDelays(uint8_t zone, uint8_t *m0, uint8_t *m1)
{
	uint8_t z = Fire_DebugZoneIndex(zone);
	*m0 = debug_module_delay[z][0];
	*m1 = debug_module_delay[z][1];
}

static uint8_t Fire_CollectSortedIgniterTargetsByZone(uint8_t zone, FireIgniterAddr *out, uint8_t max_out)
{
	uint8_t n = 0u;
	for (uint8_t i = 0u; i < g_active_devices_count && n < max_out; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online || ad->dev.zone != zone) {
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
	/* Фаза 1: старт в зоне с zone_delay + module_delay. */
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint8_t zd = Fire_ZoneDelaySec(zone);
	uint8_t m0, m1;
	Fire_GetModuleDelays(zone, &m0, &m1);
	for (uint8_t i = 0u; i < n; i++) {
		uint8_t md = (i == 0u) ? m0 : m1;
		Fire_RetryQueueStart(&ign[i], zd, md, HAL_GetTick());
	}
}

static void Fire_SendPhase2Zone(uint8_t zone)
{
	/* Фаза 2: немедленный старт (zone_delay=0), учитываем только module_delay. */
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint8_t m0, m1;
	Fire_GetModuleDelays(zone, &m0, &m1);
	for (uint8_t i = 0u; i < n; i++) {
		uint8_t md = (i == 0u) ? m0 : m1;
		Fire_RetryQueueStart(&ign[i], 0u, md, HAL_GetTick());
	}
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
		if (!s->active || s->phase2_sent || s->fire1_waiting) {
			continue;
		}
		if (now_ms >= s->phase2_deadline_ms) {
			s->paused_remaining_ms = 0u;
		} else {
			s->paused_remaining_ms = s->phase2_deadline_ms - now_ms;
		}
		FireIgniterAddr ign[16];
		uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
		for (uint8_t j = 0u; j < n; j++) {
			Fire_RetryQueuePause(&ign[j], now_ms);
		}
	}
	g_fire.zone_countdown_paused = 1u;
}

static void Fire_ResumeCountdownAndDispatch(uint32_t now_ms)
{
	if (!g_fire.zone_countdown_paused) {
		return;
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent || s->fire1_waiting) {
			continue;
		}
		s->phase2_deadline_ms = now_ms + s->paused_remaining_ms;
		FireIgniterAddr ign[16];
		uint8_t n = Fire_CollectSortedIgniterTargetsByZone(s->zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
		for (uint8_t j = 0u; j < n; j++) {
			Fire_RetryQueueResume(&ign[j], now_ms);
		}
	}
	g_fire.zone_countdown_paused = 0u;
}

static void Fire_SendTemporaryRelayToggleByZone(uint8_t zone)
{
	/* Временная логика: при новом пожаре зоны инвертируем реле этой зоны (cmd=10 без параметра). */
	for (uint8_t mi = 0u; mi < 32u; mi++) {
		const MKUCfg *m = &PPKYConfig.CfgDevices[mi];
		const Device *mdev = &m->UId.devId;
		if (mdev->d_type == 0u) {
			continue;
		}
		if ((mdev->zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		for (uint8_t slot = 0u; slot < NUM_DEV_IN_MCU; slot++) {
			if ((uint8_t)m->VDtype[slot] != DEVICE_RELAY_TYPE) {
				continue;
			}
			can_ext_id_t can_id;
			uint8_t data[8] = {0};
			can_id.ID = 0u;
			can_id.field.dir = 0u;
			can_id.field.d_type = DEVICE_RELAY_TYPE;
			can_id.field.h_adr = mdev->h_adr;
			can_id.field.l_adr = (uint8_t)((slot + 1u) & 0x3Fu);
			can_id.field.zone = mdev->zone & 0x7Fu;
			data[0] = 10u; /* DeviceRelay::CommandCB toggle */
			SendMessageFull(can_id, data, 0u, BUS_CAN12);
		}
	}
}

static int8_t Fire_FindSlotZone(uint8_t zone)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && g_fire.slots[i].zone == zone) {
			return (int8_t)i;
		}
	}
	return -1;
}

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

static uint32_t Fire_SourceKeyFromMsgId(uint32_t msg_id)
{
	can_ext_id_t id;
	id.ID = msg_id & 0x0FFFFFFFu;
	return (((uint32_t)(id.field.d_type & 0x7Fu)) << 14) |
	       (((uint32_t)(id.field.h_adr & 0xFFu)) << 6) |
	       ((uint32_t)(id.field.l_adr & 0x3Fu));
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

static uint8_t Fire_ZoneIsFire1Waiting(uint8_t zone)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || !g_fire.slots[i].fire1_waiting) {
			continue;
		}
		if ((g_fire.slots[i].zone & 0x7Fu) == (zone & 0x7Fu)) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Fire_ShouldUseFire1Sound(void)
{
	return (Fire_HasFire1Waiting() && Fire_CountPendingPhase2() == 0u) ? 1u : 0u;
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
		if (Fire_IsAndEffectiveForZone(s->zone)) {
			continue;
		}
		s->fire1_waiting = 0u;
		s->phase2_sent = 0u;
		s->phase2_deadline_ms = now_ms + (uint32_t)Fire_ZoneDelaySec(s->zone) * 1000u;
		s->paused_remaining_ms = 0u;
		Fire_SendPhase1Zone(s->zone);
		promoted = 1u;
	}
	return promoted;
}

static uint8_t Fire_TryAddNewFireZone(uint8_t zone, uint32_t source_key, uint8_t and_effective, uint32_t now_ms)
{
	/* Возвращает:
	 * 0 - без изменений (дубль источника),
	 * 1 - новая зона,
	 * 2 - переход зоны из ПОЖАР1 в ПОЖАР2. */
	int8_t si = Fire_FindSlotZone(zone);
	if (si >= 0) {
		FireZoneSlot *existing = &g_fire.slots[(uint8_t)si];
		if (!Fire_AddSourceToSlot(existing, source_key)) {
			return 0u;
		}
		if (existing->fire1_waiting && existing->source_count >= 2u) {
			existing->fire1_waiting = 0u;
			existing->phase2_sent = 0u;
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
	s->zone = zone;
	(void)Fire_AddSourceToSlot(s, source_key);
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
}

static uint8_t Fire_AnyActiveSlot(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Fire_CountPendingPhase2(void)
{
	uint8_t c = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && !g_fire.slots[i].phase2_sent && !g_fire.slots[i].fire1_waiting) {
			c++;
		}
	}
	return c;
}

static uint8_t Fire_MinRemainingSec(uint32_t now_ms)
{
	if (g_fire.zone_countdown_stopped) {
		return 0u;
	}
	if (g_fire.zone_countdown_paused) {
		uint32_t best_ms = 0xFFFFFFFFu;
		uint8_t found = 0u;
		for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
			if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
				continue;
			}
			found = 1u;
			if (g_fire.slots[i].paused_remaining_ms < best_ms) {
				best_ms = g_fire.slots[i].paused_remaining_ms;
			}
		}
		if (!found) {
			return 0u;
		}
		return (uint8_t)((best_ms + 999u) / 1000u);
	}
	uint32_t best_ms = 0xFFFFFFFFu;
	uint8_t found = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
			continue;
		}
		found = 1u;
		if (now_ms >= g_fire.slots[i].phase2_deadline_ms) {
			return 0u;
		}
		uint32_t rem = g_fire.slots[i].phase2_deadline_ms - now_ms;
		if (rem < best_ms) {
			best_ms = rem;
		}
	}
	if (!found) {
		return 0u;
	}
	return (uint8_t)((best_ms + 999u) / 1000u);
}

static uint8_t Fire_RemainingSecForZone(uint8_t zone, uint32_t now_ms)
{
	uint32_t best_ms = 0xFFFFFFFFu;
	uint8_t found = 0u;
	if (g_fire.zone_countdown_stopped) {
		return 0u;
	}
	if (g_fire.zone_countdown_paused) {
		for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
			if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
				continue;
			}
			if ((g_fire.slots[i].zone & 0x7Fu) != (zone & 0x7Fu)) {
				continue;
			}
			found = 1u;
			if (g_fire.slots[i].paused_remaining_ms < best_ms) {
				best_ms = g_fire.slots[i].paused_remaining_ms;
			}
		}
		if (!found) {
			return 0u;
		}
		return (uint8_t)((best_ms + 999u) / 1000u);
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
			continue;
		}
		if ((g_fire.slots[i].zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		found = 1u;
		if (now_ms >= g_fire.slots[i].phase2_deadline_ms) {
			return 0u;
		}
		{
			uint32_t rem = g_fire.slots[i].phase2_deadline_ms - now_ms;
			if (rem < best_ms) {
				best_ms = rem;
			}
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
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
			continue;
		}
		if (now_ms >= g_fire.slots[i].phase2_deadline_ms) {
			Fire_SendPhase2Zone(g_fire.slots[i].zone);
			g_fire.slots[i].phase2_sent = 1u;
			any_started = 1u;
		}
	}
	return any_started;
}

static void Fire_Phase2AllPending(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
			continue;
		}
		Fire_SendPhase2Zone(g_fire.slots[i].zone);
		g_fire.slots[i].phase2_sent = 1u;
	}
}

static uint8_t Fire_Phase2SelectedPending(uint8_t zone)
{
	uint8_t started = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent || g_fire.slots[i].fire1_waiting) {
			continue;
		}
		if ((g_fire.slots[i].zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		Fire_SendPhase2Zone(g_fire.slots[i].zone);
		g_fire.slots[i].phase2_sent = 1u;
		started = 1u;
	}
	return started;
}

static void Fire_SendStopZone(uint8_t zone)
{
	FireIgniterAddr ign[16];
	uint8_t n = Fire_CollectSortedIgniterTargetsByZone(zone, ign, (uint8_t)(sizeof(ign) / sizeof(ign[0])));
	uint32_t now_ms = HAL_GetTick();
	for (uint8_t i = 0u; i < n; i++) {
		Fire_RetryQueueStop(&ign[i], now_ms);
	}
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		if ((g_fire.slots[i].zone & 0x7Fu) == (zone & 0x7Fu)) {
			g_fire.slots[i].phase2_sent = 1u;
			g_fire.slots[i].fire1_waiting = 0u;
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
		if (zone_sent[z]) {
			continue;
		}
		zone_sent[z] = 1u;
		Fire_SendPhase2Zone(z);
		any_started = 1u;
	}

	/* Для слотов пожара помечаем отправку фазы 2 только в реально запущенные зоны */
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].phase2_sent) {
			continue;
		}
		if (zone_sent[g_fire.slots[i].zone & 0x7Fu]) {
			g_fire.slots[i].phase2_sent = 1u;
			g_fire.slots[i].fire1_waiting = 0u;
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
		s->phase2_sent = 1u;
		s->phase2_deadline_ms = now_ms;
	}

	return any_started;
}

static uint8_t Fire_ZoneAllIgnitersEndAck(uint8_t zone)
{
	uint8_t has_igniters = 0u;
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || m->dev.zone != zone) {
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
		g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
	}
}

static uint8_t Fire_BuildUiZoneList(uint8_t *zones, uint8_t max_out)
{
	uint8_t nz = 0u;
	uint8_t show_all_history = Fire_AllActiveZonesEndAck();

	if (zones == NULL || max_out == 0u) {
		return 0u;
	}

	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		/* До полного завершения тушения показываем только "текущие" непотушенные зоны.
		 * После завершения по всем зонам (end_ack) показываем исторический список. */
		if (!show_all_history && Fire_ZoneAllIgnitersEndAck(g_fire.slots[i].zone)) {
			continue;
		}
		uint8_t z = g_fire.slots[i].zone;
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
	for (uint8_t a = 1u; a < nz; a++) {
		uint8_t key = zones[a];
		uint8_t b = a;
		while (b > 0u && zones[b - 1u] > key) {
			zones[b] = zones[b - 1u];
			b--;
		}
		zones[b] = key;
	}
	return nz;
}

static void Fire_FillZoneNamesForUi(char (*out_names)[FIRE_UI_NAME_LEN], uint8_t *out_n)
{
	/* Готовит уникальный отсортированный список имён зон для TouchGFX. */
	uint8_t zones[FIRE_UI_MAX_ZONES];
	uint8_t nz = Fire_BuildUiZoneList(zones, FIRE_UI_MAX_ZONES);
	*out_n = nz;
	for (uint8_t i = 0u; i < nz; i++) {
		uint8_t z_can = zones[i];
		uint8_t zi = Fire_ZoneCanToIdx(z_can);
		char *dst = out_names[i];
		if (zi >= ZONE_NUMBER) {
			dst[0] = '\0';
			continue;
		}
		char name[ZONE_NAME_SIZE + 1u];
		memcpy(name, PPKYConfig.zone_name[zi], ZONE_NAME_SIZE);
		name[ZONE_NAME_SIZE] = '\0';
		for (int s = (int)ZONE_NAME_SIZE - 1; s >= 0; s--) {
			if (name[s] == ' ' || name[s] == '\0') {
				name[s] = '\0';
			} else {
				break;
			}
		}
		if (name[0] == '\0') {
			(void)snprintf(dst, (size_t)FIRE_UI_NAME_LEN, "Зона %u", (unsigned)z_can);
		} else {
			(void)snprintf(dst, (size_t)FIRE_UI_NAME_LEN, "%s", name);
		}
		dst[FIRE_UI_NAME_LEN - 1u] = '\0';
	}
}

static void Fire_UpdateUiText(uint8_t active, uint8_t mode, uint8_t remaining_s, uint8_t n_zones,
			      char (*zone_names)[FIRE_UI_NAME_LEN])
{
	/* Пуш в UI только при изменениях; есть защита от редкой рассинхронизации n_zones==0. */
	uint8_t same = (g_fire.last_ui_active == active && g_fire.last_ui_mode == mode &&
			g_fire.last_ui_remaining == remaining_s &&
			g_fire.last_ui_nzones == n_zones);
	if (same && n_zones > 0u) {
		same = (memcmp(g_fire.last_ui_names, zone_names,
			       (size_t)n_zones * (size_t)FIRE_UI_NAME_LEN) == 0);
	}
	/*
	 * Раньше при n_zones==0 кэш считал одинаковым (active, remaining, 0) и годами не вызывал
	 * Fire_UiUpdate, пока не сменится секунда таймера — имя зоны не доходило до TouchGFX.
	 * При активных слотах список имён должен быть непуст: периодически пробиваем кэш.
	 */
	if (same && active && n_zones == 0u && Fire_AnyActiveSlot()) {
		uint32_t t = HAL_GetTick();
		if ((t - g_fire.last_ui_force_names_ms) >= 50u) {
			g_fire.last_ui_force_names_ms = t;
			same = 0u;
		}
	}
	if (same) {
		return;
	}
	g_fire.last_ui_active = active;
	g_fire.last_ui_mode = mode;
	g_fire.last_ui_remaining = remaining_s;
	g_fire.last_ui_nzones = n_zones;
	if (n_zones > 0u) {
		memcpy(g_fire.last_ui_names, zone_names,
		       (size_t)n_zones * (size_t)FIRE_UI_NAME_LEN);
	}
	Fire_UiUpdate(active, mode, remaining_s, n_zones, zone_names);
}

static void Fire_SetIdleIndication(void)
{
	/* Полный дежурный профиль индикаторов/звука в состоянии IDLE. */
	Led_Set(LED_BUT_START_SP, 0);
	Led_Set(LED_STR_START_SP, 0);
	Led_Set(LED_BUT_START_ALL, 0);
	Fire_SetStartAllBrightness(0u);
	Led_Set(LED_STR_START_ALL, 1u);
	Led_Set(LED_BUT_STOP, 0);
	Led_Set(LED_STR_STOP, 0);
	Led_Set(LED_START, 0);
	Led_Set(LED_STOP, 0);
	Led_Set(LED_AUTO_OFF, (PPKYConfig.fire_mode == 2u) ? 1u : 0u);
	Led_Set(LED_FIRE, 0);
	g_fire.stop_launch_pressed_latched = 0u;
	g_fire.beeper_alert_active = 0u;
	g_fire.beeper_duty_active = 0u;
	g_fire.beeper_start_pattern_active = 0u;
	Beeper_ContinuousOff();
	if (!g_fire.start_all_hold_sound_active) {
		Beeper_StopPattern();
	}
}

static void Fire_SetStartAllBrightness(uint8_t bright)
{
	if (g_fire.start_all_is_bright == bright) {
		return;
	}
	g_fire.start_all_is_bright = bright;
	uint8_t pwr = bright ? LED_BUT_MAX_BRIGHTNESS : LED_BUT_DIM_BRIGHTNESS;
	Led_SetBrightness(LED_BUT_START_ALL, pwr);
	Led_SetBrightness(LED_STR_START_ALL, pwr);
}

static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text)
{
	/* Общее поведение "ОСТАНОВ ПУСКА": таймеры остановлены, запуск заблокирован, manual UI/LED. */
	g_fire.zone_countdown_stopped = 1u;
	g_fire.zone_countdown_paused = 0u;
	g_fire.start_launch_pressed_latched = 0u;
	g_fire.stop_launch_pressed_latched = 1u;
	g_fire.stop_text_blink_until_ms = blink_stop_text ? (now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u)) : 0u;
	Fire_SendStopAllMcus();
	g_fire.all_hold_active = 0u;
	g_fire.all_hold_ms = 0u;
	g_fire.btn_start_all_hold_latched = 0u;
	Fire_StartAllHoldSoundOff();
	if (Fire_CountPendingPhase2() > 0u) {
		g_fire.state = FIRE_STATE_WAIT_MANUAL;
	}
}

static void Fire_ApplyFireModePolicy(uint32_t now_ms)
{
	uint8_t mode = PPKYConfig.fire_mode;
	if (g_fire.last_fire_mode == mode) {
		return;
	}
	g_fire.last_fire_mode = mode;

	/* Внешний перевод в manual во время активного пожара должен вести себя как кнопка STOP. */
	if ((mode == 2u) && Fire_AnyActiveSlot() && (Fire_CountPendingPhase2() > 0u) && !g_fire.zone_countdown_stopped) {
		Fire_EnterManualStop(now_ms, 1u);
		return;
	}

	/* 0 и 1 трактуем как auto-поведение на стороне ППКУ. */
	if ((mode != 2u) && (g_fire.state == FIRE_STATE_WAIT_MANUAL) &&
	    !g_fire.zone_countdown_stopped && (Fire_CountPendingPhase2() > 0u)) {
		g_fire.state = FIRE_STATE_WAIT_AUTO;
	}
}

static uint8_t Fire_ButtonPressedEvent(uint8_t button_id, uint8_t *latched_flag)
{
	ButtonState st = Button_GetState(button_id);
	if ((st == ButtonStatePress || st == ButtonStateLongPress) && (*latched_flag == 0u)) {
		*latched_flag = 1u;
		return 1u;
	}
	if (st == ButtonStateReset) {
		*latched_flag = 0u;
	}
	return 0u;
}

static void Fire_ApplyStateLeds(uint32_t now_ms)
{
	/* Профиль индикации для не-IDLE состояний; отдельные override применяются в Fire_Transition(). */
	if (PPKYConfig.fire_mode == 2u) {
		Led_Set(LED_AUTO_OFF, 1);
	} else {
		Led_Set(LED_AUTO_OFF, 0);
	}

	uint8_t pending = Fire_CountPendingPhase2();

	if (pending > 0u) {
		Led_Set(LED_BUT_START_SP, 1);
		if ((int32_t)(now_ms - g_fire.start_sp_text_blink_until_ms) < 0) {
			uint8_t blink_on = (((now_ms / (FIRE_START_SP_TEXT_BLINK_PERIOD_MS / 2u)) & 1u) != 0u) ? 1u : 0u;
			Led_Set(LED_STR_START_SP, blink_on);
		} else {
			Led_Set(LED_STR_START_SP, 1);
		}
		Led_Set(LED_BUT_STOP, 1);
		if ((int32_t)(now_ms - g_fire.stop_text_blink_until_ms) < 0) {
			uint8_t blink_on = (((now_ms / (FIRE_STOP_TEXT_BLINK_PERIOD_MS / 2u)) & 1u) != 0u) ? 1u : 0u;
			Led_Set(LED_STR_STOP, blink_on);
		} else {
			Led_Set(LED_STR_STOP, 1);
		}
		if (g_fire.all_hold_active) {
			Fire_SetStartAllBrightness(1u);
			Led_Set(LED_BUT_START_ALL, 0u);
		} else {
			Fire_SetStartAllBrightness(0u);
			Led_Set(LED_BUT_START_ALL, 0u);
			Led_Set(LED_STR_START_ALL, 1u);
		}
		if (g_fire.beeper_start_pattern_active) {
			uint32_t phase = (now_ms - g_fire.start_pattern_started_ms) %
					 (BEEPER_PATTERN_START_ON_MS + BEEPER_PATTERN_START_OFF_MS);
			Led_Set(LED_START, (phase < BEEPER_PATTERN_START_ON_MS) ? 1u : 0u);
		} else {
			Led_Set(LED_START, ((int32_t)(now_ms - g_fire.start_led_hold_until_ms) < 0) ? 1u : 0u);
		}
	} else {
		Led_Set(LED_BUT_START_SP, 0);
		Led_Set(LED_STR_START_SP, 0);
		Led_Set(LED_BUT_STOP, 0);
		Led_Set(LED_STR_STOP, 0);
		Fire_SetStartAllBrightness(0u);
		Led_Set(LED_BUT_START_ALL, 0);
		Led_Set(LED_STR_START_ALL, 1);
		if (g_fire.beeper_start_pattern_active) {
			uint32_t phase = (now_ms - g_fire.start_pattern_started_ms) %
					 (BEEPER_PATTERN_START_ON_MS + BEEPER_PATTERN_START_OFF_MS);
			Led_Set(LED_START, (phase < BEEPER_PATTERN_START_ON_MS) ? 1u : 0u);
		} else {
			Led_Set(LED_START, ((int32_t)(now_ms - g_fire.start_led_hold_until_ms) < 0) ? 1u : 0u);
		}
	}
	Led_Set(LED_STOP, g_fire.stop_launch_pressed_latched ? 1u : 0u);
}

static void Fire_Transition(FireEvent ev, uint32_t now_ms)
{
	/* Единая точка обработки событий сценария пожара:
	 * FSM, автодедлайны, звук, LED и публикация состояния на UI. */
	uint8_t ui_active = 0u;
	uint8_t ui_mode = 0u;
	uint8_t ui_remaining = 0u;
	uint8_t fire_processed = 0u;
	uint8_t start_processed = 0u;

	switch (ev) {
	case FIRE_EVENT_STATUS_FIRE:
		g_fire.start_launch_pressed_latched = 0u;
		if (g_fire.state == FIRE_STATE_IDLE) {
			g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
			g_fire.reply_received = 0u;
		}
		Led_ForceStatusBright(LED_FIRE);
		/* Новый пожар: профиль звука зависит от ПОЖАР1/ПОЖАР2. */
		Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
		if (PPKYConfig.fire_mode == 2u) {
			/* В ручном режиме пожар сразу обрабатывается как "ОСТАНОВ ПУСКА". */
			Fire_EnterManualStop(now_ms, 0u);
			fire_processed = 1u;
		}
		break;
	case FIRE_EVENT_REPLY_FIRE:
		g_fire.reply_received = 1u;
		break;
	case FIRE_EVENT_STOP_EXT:
		/* Команда StopExtinguishment с CAN: только остановка таймеров/автопуска, без сброса слотов */
		Fire_SendStopAllMcus();
		g_fire.zone_countdown_stopped = 1u;
		g_fire.zone_countdown_paused = 0u;
		g_fire.start_launch_pressed_latched = 0u;
		g_fire.all_hold_active = 0u;
		g_fire.all_hold_ms = 0u;
		g_fire.btn_start_all_hold_latched = 0u;
		Fire_StartAllHoldSoundOff();
		if (Fire_CountPendingPhase2() > 0u) {
			g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
		}
		break;
	case FIRE_EVENT_BTN_START_SP:
		/* ПУСК СП: только при активных слотах пожара и есть зоны без фазы 2 */
		if (!Fire_AnyActiveSlot() || Fire_CountPendingPhase2() == 0u) {
			break;
		}
		if (g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL ||
		    g_fire.state == FIRE_STATE_EXTINGUISHING) {
#if FIRE_SELECTED_ZONE_BUTTONS_ENABLE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				if (Fire_GetSelectedZoneFromUi(&sel_zone) && Fire_Phase2SelectedPending(sel_zone)) {
					g_fire.start_launch_pressed_latched = 1u;
					g_fire.stop_launch_pressed_latched = 0u;
					g_fire.start_sp_text_blink_until_ms = now_ms + (FIRE_START_SP_TEXT_BLINK_PERIOD_MS * 3u);
					Fire_SyncStateFromSlots();
					fire_processed = 1u;
					start_processed = 1u;
				}
				break;
			}
#endif
			/* Пуск тушения обработан — индикацию «ОСТАНОВ ПУСКА» снимаем */
			g_fire.start_launch_pressed_latched = 1u;
			g_fire.stop_launch_pressed_latched = 0u;
			g_fire.start_sp_text_blink_until_ms = now_ms + (FIRE_START_SP_TEXT_BLINK_PERIOD_MS * 3u);
			Fire_Phase2AllPending();
			Fire_SyncStateFromSlots();
			fire_processed = 1u;
			start_processed = 1u;
		}
		break;
	case FIRE_EVENT_BTN_START_ALL:
		/* ПУСК ОБЩИЙ: запуск тушения всех существующих зон с module_delay (zone_delay=0),
		 * независимо от статуса ПОЖАРА/слотов */
		{
			uint8_t any_started = Fire_StartAllExistingZonesAndMarkSlots();
			/* Если есть активные пожарные слоты — ручной общий пуск должен
			 * завершить их ожидание фазы 2 (таймер далее не идёт). */
			if (Fire_CountPendingPhase2() > 0u) {
				Fire_Phase2AllPending();
				any_started = 1u;
			}
			if (any_started) {
			g_fire.start_launch_pressed_latched = 1u;
			g_fire.stop_launch_pressed_latched = 0u;
			g_fire.start_sp_text_blink_until_ms = now_ms + (FIRE_START_SP_TEXT_BLINK_PERIOD_MS * 3u);
			Fire_SyncStateFromSlots();
			fire_processed = 1u;
			start_processed = 1u;
		}
		}
		break;
	case FIRE_EVENT_BTN_STOP:
		/* ОСТАНОВ ПУСКА: ручной режим, стоп МКУ, отображение/авто-таймеры зон off, пожар и слоты сохраняются */
		if (g_fire.start_launch_pressed_latched) {
			break;
		}
		if (Fire_CountPendingPhase2() == 0u) {
			/* Для ПОЖАР1 в зоне "И": STOP только переводит звук в дежурный,
			 * без перехода в ручной режим и без остановки сценария. */
			if (Fire_HasFire1Waiting()) {
				fire_processed = 1u;
				start_processed = 0u;
			}
			break;
		}
		{
#if FIRE_SELECTED_ZONE_BUTTONS_ENABLE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				if (Fire_GetSelectedZoneFromUi(&sel_zone)) {
					Fire_SendStopZone(sel_zone);
					g_fire.stop_launch_pressed_latched = 1u;
					g_fire.stop_text_blink_until_ms = now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u);
					fire_processed = 1u;
				}
				break;
			}
#endif
			uint8_t manual_mode_initial = (PPKYConfig.fire_mode == 2u) ? 1u : 0u;
			PPKYConfig.fire_mode = 2u;
			g_fire.last_fire_mode = 2u;
			Fire_EnterManualStop(now_ms, manual_mode_initial ? 0u : 1u);
			fire_processed = 1u;
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
	Fire_SyncStateFromSlots();
	/* Пока решение не принято (до ПУСК/СТОП/автопуска), удерживаем тревожный звук активным.
	 * Это защищает от внешних пересечений индикации, которые могут сбросить звук. */
	if ((g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL) &&
	    (g_fire.start_launch_pressed_latched == 0u) &&
	    (g_fire.stop_launch_pressed_latched == 0u) &&
	    !g_fire.all_hold_active &&
	    !g_fire.beeper_alert_active &&
	    !g_fire.beeper_start_pattern_active &&
	    !g_fire.beeper_duty_active) {
		Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
	}
	if (fire_processed && g_fire.state != FIRE_STATE_IDLE) {
		if (start_processed) {
			Fire_BeeperEnterStartPattern(now_ms);
		} else {
			/* Пожар обработан остановом — дежурный паттерн 2x0.2с/5с. */
			Fire_BeeperEnterDuty(Fire_ShouldUseFire1Sound());
		}
		Led_SetBrightness(LED_FIRE, LED_STATUS_DIM_BRIGHTNESS);
	}
	Fire_BeeperTick(now_ms);

	if (g_fire.all_hold_active && g_fire.all_hold_ms < 3000u) {
		uint32_t rem_ms = 3000u - g_fire.all_hold_ms;
		ui_remaining = (uint8_t)((rem_ms + 999u) / 1000u);
	} else if (g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL) {
		if (!g_fire.zone_countdown_stopped) {
#if FIRE_SELECTED_ZONE_BUTTONS_ENABLE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				if (Fire_GetSelectedZoneFromUi(&sel_zone)) {
					if (Fire_ZoneIsFire1Waiting(sel_zone)) {
						ui_mode = 6u; /* ПОЖАР1 */
						ui_remaining = 0u;
					} else {
						ui_remaining = Fire_RemainingSecForZone(sel_zone, now_ms);
					}
				} else {
					ui_remaining = Fire_MinRemainingSec(now_ms);
				}
			} else {
				ui_remaining = Fire_MinRemainingSec(now_ms);
			}
#else
			ui_remaining = Fire_MinRemainingSec(now_ms);
#endif
			if (ui_mode == 0u) {
				if (Fire_CountPendingPhase2() == 0u && Fire_HasFire1Waiting()) {
					ui_mode = 6u; /* ПОЖАР1 */
					ui_remaining = 0u;
				} else {
					ui_mode = g_fire.zone_countdown_paused ? 5u : 1u; /* ПАУЗА или ДО ПУСКА */
				}
			}
		} else {
			ui_remaining = 0u;
			ui_mode = 4u; /* ПОЖАР/ОСТ. ПУСКА */
		}
	} else if (g_fire.state == FIRE_STATE_EXTINGUISHING) {
		ui_remaining = 0u;
		ui_mode = 2u; /* ТУШЕНИЕ */
		if (Fire_AllActiveZonesEndAck()) {
			ui_mode = 3u; /* ТУШЕНИЕ ПРОИЗВЕДЕНО */
		}
	}

	if (g_fire.state == FIRE_STATE_IDLE) {
		Led_Set(LED_FIRE, 0);
		g_fire.led_fire_on = 0u;
	} else {
		Led_Set(LED_FIRE, 1u);
		g_fire.led_fire_on = 1u;
		/* До принятия решения по пожару (стоп/пуск/автопуск) держим ПОЖАР ярким,
		 * несмотря на глобальный механизм автозатухания в led.c. */
		if (g_fire.beeper_alert_active) {
			Led_ForceStatusBright(LED_FIRE);
		}
	}

	if (g_fire.state == FIRE_STATE_IDLE) {
		if (g_fire.all_hold_active && g_fire.all_hold_ms < 3000u) {
			/* Без пожара: показываем только 3-сек таймер удержания ПУСК ОБЩИЙ и мигание подписи */
			Fire_SetIdleIndication();
			{
				uint8_t blink_on = (((now_ms / (FIRE_START_ALL_TEXT_BLINK_PERIOD_MS / 2u)) & 1u) != 0u) ? 1u : 0u;
				Fire_SetStartAllBrightness(1u);
				Led_Set(LED_BUT_START_ALL, 0u);
				Led_Set(LED_STR_START_ALL, blink_on);
			}
			{
				char z0[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
				Fire_UpdateUiText(1u, 1u, ui_remaining, 0u, z0);
			}
			return;
		}
		Fire_SetIdleIndication();
		if (g_fire.start_launch_pressed_latched) {
			Led_Set(LED_START, 1u);
		}
		{
			char z0[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
			Fire_UpdateUiText(0u, 0u, 0u, 0u, z0);
		}
		return;
	}

	Fire_ApplyStateLeds(now_ms);
	if ((g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL) &&
	    g_fire.all_hold_active) {
		uint8_t blink_on = (((now_ms / (FIRE_START_ALL_TEXT_BLINK_PERIOD_MS / 2u)) & 1u) != 0u) ? 1u : 0u;
		Fire_SetStartAllBrightness(1u);
		Led_Set(LED_BUT_START_ALL, 0u);
		Led_Set(LED_STR_START_ALL, blink_on);
	}

	ui_active = 1u;
	{
		char zn[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
		uint8_t nzn = 0u;
		Fire_FillZoneNamesForUi(zn, &nzn);
		Fire_UpdateUiText(ui_active, ui_mode, ui_remaining, nzn, zn);
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
	g_fire.state = FIRE_STATE_IDLE;
	g_fire.start_all_is_bright = 0xFFu;
	Fire_SetStartAllBrightness(0u);
	Led_Set(LED_BUT_START_ALL, 0u);
	Led_Set(LED_STR_START_ALL, 1u);
	g_fire.start_all_is_bright = 0u;
	g_fire.last_ui_nzones = 0u;
	g_fire.last_fire_mode = PPKYConfig.fire_mode;
	g_fire_ui_manual_select_enabled = 0u;
	g_fire_ui_selected_index = 0u;
}

/* Периодический тик 1 мс: FSM, таймеры автопуска и UI-обновления. */
void Fire_Timer1ms(void)
{
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
	/* 10мс-путь: кнопки (в т.ч. удержание ПУСК ОБЩИЙ 3с) и edge-trigger событий. */
	ButtonState st_start_all = Button_GetState(BUT_FORCE);
	if (st_start_all == ButtonStatePress || st_start_all == ButtonStateLongPress) {
		if (!g_fire.all_hold_active) {
			g_fire.all_hold_active = 1u;
			g_fire.all_hold_ms = 0u;
			g_fire.state_start_ms = HAL_GetTick();
			Fire_StartAllHoldSoundOn();
		} else {
			if (g_fire.all_hold_ms < 3000u) {
				g_fire.all_hold_ms += 10u;
			}
			if (g_fire.all_hold_ms >= 3000u && g_fire.btn_start_all_hold_latched == 0u) {
				g_fire.btn_start_all_hold_latched = 1u;
				Fire_StartAllHoldSoundOff();
				Fire_Transition(FIRE_EVENT_BTN_START_ALL, HAL_GetTick());
			}
		}
	} else if (st_start_all == ButtonStateReset && g_fire.all_hold_active) {
		g_fire.all_hold_active = 0u;
		g_fire.all_hold_ms = 0u;
		g_fire.btn_start_all_hold_latched = 0u;
		Fire_StartAllHoldSoundOff();
		/* Обновить UI/LED сразу после отпускания кнопки, даже если пожара нет */
		Fire_Transition(FIRE_EVENT_TICK_1MS, HAL_GetTick());
	}

	if (g_fire.state == FIRE_STATE_IDLE && !Fire_AnyActiveSlot()) {
		return;
	}

	if (Fire_ButtonPressedEvent(BUT_FIRE, &g_fire.btn_start_sp_latched)) {
		Fire_Transition(FIRE_EVENT_BTN_START_SP, HAL_GetTick());
	}
	if (Fire_ButtonPressedEvent(BUT_STOP, &g_fire.btn_stop_latched)) {
		Fire_Transition(FIRE_EVENT_BTN_STOP, HAL_GetTick());
	}
}

/* Входящее событие ПОЖАР от МКУ: добавляет зону и запускает сценарий. */
void Fire_OnStatusFire(uint32_t msg_id)
{
	/* Вход статуса пожара от МКУ: учитываем уникальные источники в зоне
	 * и поддерживаем ПОЖАР1/ПОЖАР2 для режима fire_and[]. */
	can_ext_id_t id;
	id.ID = msg_id & 0x0FFFFFFF;
	/* zone в формате CAN (обычно 1..N), без декремента:
	 * именно это значение нужно для адресации МКУ этой зоны. */
	uint8_t zone = (uint8_t)(id.field.zone & 0x7Fu);
	uint32_t source_key = Fire_SourceKeyFromMsgId(msg_id);
	uint8_t and_effective = Fire_IsAndEffectiveForZone(zone);
	uint32_t now = HAL_GetTick();
	uint8_t res = Fire_TryAddNewFireZone(zone, source_key, and_effective, now);
	if (res == 1u) {
		Fire_SendTemporaryRelayToggleByZone(zone);
		/* Новая зона: слот, фаза 1, затем FSM — UI видит все активные зоны */
		Fire_Transition(FIRE_EVENT_STATUS_FIRE, now);
	} else if (res == 2u) {
		/* Переход ПОЖАР1 -> ПОЖАР2 после второго датчика в зоне "И". */
		Fire_Transition(FIRE_EVENT_STATUS_FIRE, now);
	}
	SetReplyStatusFire(zone);
}

/* Входящий ReplyStatusFire от МКУ (подтверждение статуса пожара). */
void Fire_OnReplyStatusFire(uint32_t msg_id)
{
	(void)msg_id;
	Fire_Transition(FIRE_EVENT_REPLY_FIRE, HAL_GetTick());
}

/* Входящая команда StopExtinguishment от МКУ/CAN. */
void Fire_OnStopExtinguishment(uint32_t msg_id)
{
	(void)msg_id;
	Fire_Transition(FIRE_EVENT_STOP_EXT, HAL_GetTick());
}

void Fire_OnPauseExtinguishmentTimer(uint32_t msg_id)
{
	(void)msg_id;
	Fire_PauseCountdownAndDispatch(HAL_GetTick());
}

void Fire_OnResumeExtinguishmentTimer(uint32_t msg_id)
{
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

void Fire_UiSetManualSelection(uint8_t enabled, uint8_t selected_ui_index)
{
	g_fire_ui_manual_select_enabled = enabled ? 1u : 0u;
	g_fire_ui_selected_index = selected_ui_index;
}
