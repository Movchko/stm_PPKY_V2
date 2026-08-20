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
#include "menu_ui.h"
#include "event_log.h"
#include "warning.h"
#include "gost_mode.h"
#include "config_zone_block.h"
#include "tick_time.h"

extern PPKYCfg PPKYConfig;
extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;

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
/* ГОСТ 53325 п.7.6.1.4 прим.2 / Тр. 3.5: частоты мигания обобщённого LED_FIRE */
#define FIRE_LED_ATTENTION_HALF_MS           1250u /* 0,4 Гц (диапазон 0,2–0,5 Гц) */
#define FIRE_LED_FIRE1_HALF_MS                400u /* 1,25 Гц (диапазон 1,0–2,0 Гц) */

typedef enum {
	FIRE_LED_MODE_OFF = 0,
	FIRE_LED_MODE_FIRE1,
	FIRE_LED_MODE_FIRE2,
	FIRE_LED_MODE_ATTENTION
} FireLedMode;

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
#if GOST_MODE
	uint8_t  ui_src_valid;   /* бегущая строка: МКУ + канал источника */
	uint8_t  ui_mcu_h_adr;
	uint8_t  ui_ch_d_type;
	uint8_t  ui_ch_l_adr;
#endif
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
	uint8_t   led_fire_mode; /* FireLedMode */
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

/* Управляет яркостью кнопки/подписи ПУСК ОБЩИЙ (обычная/активная). */
static void Fire_SetStartAllBrightness(uint8_t bright);
/* Отправляет широкую команду STOP всем МКУ пожарного контура. */
static void Fire_SendStopAllMcus(void);
/* Отправляет фазу 1 запуска по конкретной зоне. */
static void Fire_SendPhase1Zone(uint8_t zone);
/* Отправляет фазу 2 запуска по конкретной зоне. */
static uint8_t Fire_SendPhase2Zone(uint8_t zone);
/* Отправляет фазу 2 только тем спичкам зоны, у которых нет end_ack. */
static uint8_t Fire_SendPhase2ZonePending(uint8_t zone);
/* Запускает фазу 2 по всем слотам, где она ещё не отправлялась. */
static void Fire_Phase2AllPending(void);
/* Отметить отправку фазы 2 в слоте и инициализировать контур дотушивания. */
static void Fire_MarkSlotPhase2Sent(FireZoneSlot *slot, uint32_t now_ms, uint8_t start_type);
/* Полностью очищает слоты пожара и флаги остановки таймеров. */
static void Fire_ClearAllSlots(void);
#if GOST_MODE
/* GOST: удержание ОСТАНОВ ≥5 с — сброс активного пожара (слоты, таймеры, UI). */
static void Fire_GostResetFire(uint32_t now_ms);
static void Fire_ResetSuppressClear(void);
static void Fire_ResetSuppressAddFromSlot(const FireZoneSlot *slot);
static uint8_t Fire_ResetSuppressHas(uint8_t zone, uint32_t source_key);
static void Fire_ClearSlotIndex(uint8_t idx);
#endif
static void Fire_Transition(FireEvent ev, uint32_t now_ms);
/* Синхронизирует состояние FSM исходя из состояния слотов/фаз. */
static void Fire_SyncStateFromSlots(void);
/* Переводит номер зоны CAN (1..N) в индекс массива имён (0..N-1). */
static uint8_t Fire_ZoneCanToIdx(uint8_t zone_can);
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
/* Ищет слот по номеру зоны, возвращает индекс или -1. */
static int8_t Fire_FindSlotZone(uint8_t zone);
/* Выделяет свободный слот пожара, возвращает индекс или -1. */
static int8_t Fire_AllocSlot(void);
/** @return 1 если зона добавлена впервые в цикле, 0 если по этой зоне пожар уже учтён */
static uint8_t Fire_TryAddNewFireZone(uint8_t zone, uint32_t source_key, uint8_t and_effective,
				      uint32_t now_ms, uint32_t msg_id, const uint8_t *msg_data);
/* Операции с "И"-логикой пожара и источниками зоны. */
static uint8_t Fire_IsAndEnabledForZone(uint8_t zone);
static uint8_t Fire_CountOnlineDptForZone(uint8_t zone);
static uint8_t Fire_IsAndEffectiveForZone(uint8_t zone);
static uint32_t Fire_SourceKeyFromMsgId(uint32_t msg_id);
static uint8_t Fire_AddSourceToSlot(FireZoneSlot *slot, uint32_t source_key);
static uint8_t Fire_HasFire1Waiting(void);
static uint8_t Fire_ZoneIsFire1Waiting(uint8_t zone);
static uint8_t Fire_ShouldUseFire1Sound(void);
static uint8_t Fire_ShouldUseFire1Led(void);
static uint8_t Fire_PromoteFire1WhenAndUnavailable(uint32_t now_ms);
/* ГОСТ: управление LED_FIRE (ВНИМАНИЕ / ПОЖАР1 / ПОЖАР2). */
static void Fire_UpdateLedFire(uint32_t now_ms);
/* Возвращает 1, если по зоне тушение уже запускалось и повторный пуск запрещён. */
static uint8_t Fire_ZoneIsExtinguishLocked(uint8_t zone);
/* Отметить зону как тушащуюся по команде с шины (без повторной отправки CAN). */
static uint8_t Fire_MarkExternalExtinguishZone(uint8_t zone, uint32_t now_ms);
/* Обновляет флаг «видели спички без end_ack» для корректного UI-списка зон. */
static void Fire_UpdateExtinguishAckTracking(void);
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
/* Возвращает 1, если все активные пожарные зоны в терминальном состоянии (успех/ошибка). */
static uint8_t Fire_AllActiveZonesTerminal(void);
/* Возвращает 1, если есть хотя бы одна зона с исчерпанными попытками дотушивания. */
static uint8_t Fire_HasExtinguishFailure(void);
/* Периодический дотушивающий перезапуск только незавершённых спичек. */
static void Fire_ProcessExtinguishRetries(uint32_t now_ms);
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
static void Fire_AbortExtinguishRetriesForZone(uint8_t zone);
static uint8_t Fire_GetSelectedZoneFromUi(uint8_t *zone);
static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text, uint8_t stop_reason);
static void Fire_ApplyFireModePolicy(uint32_t now_ms);
static void Fire_PauseCountdownAndDispatch(uint32_t now_ms);
static void Fire_ResumeCountdownAndDispatch(uint32_t now_ms);

extern void Fire_UiUpdate(uint8_t active, uint8_t mode, uint8_t remaining_s, uint8_t n_zones,
			  char (*zone_names)[FIRE_UI_NAME_LEN]);

static void Fire_BeeperEnterAlert(uint8_t fire1_sound)
{
	Beeper_ResumeSoundOnNewEvent();
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
	Beeper_ResumeSoundOnNewEvent();
	/* ПУСК: прерывистый звук  */
	g_fire.beeper_alert_active = 0u;
	g_fire.beeper_duty_active = 0u;
	g_fire.beeper_start_pattern_active = 1u;
	g_fire.start_pattern_started_ms = HAL_GetTick();
	Beeper_ContinuousOff();
	Beeper_StartPulseTrain(BEEPER_PATTERN_START_ON_MS, BEEPER_PATTERN_START_OFF_MS,
			       BEEPER_PATTERN_START_PULSES, BEEPER_PATTERN_START_REPEAT_MS);
}

/* Есть зона, у которой тушение ещё в процессе (фаза 2 ушла, end_ack нет). */
static uint8_t Fire_AnyZoneExtinguishingInProgress(void)
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

static void Fire_BeeperTick(uint32_t now_ms)
{
	if (g_fire.start_all_hold_sound_active) {
		return;
	}
	if (g_fire.beeper_alert_active) {
		return;
	}
	if (g_fire.beeper_start_pattern_active) {
		if (Fire_AllActiveZonesEndAck()) {
			/* Все активные зоны потушены — дежурный профиль. */
			g_fire.beeper_start_pattern_active = 0u;
			g_fire.start_led_hold_until_ms = now_ms + FIRE_START_LED_HOLD_MS;
			Fire_BeeperEnterDuty(Fire_ShouldUseFire1Sound());
		} else if (Fire_AnyZoneExtinguishingInProgress() == 0u) {
			/* Тушение части зон закончилось, но пожар ещё есть (другие зоны
			 * в останове/ожидании) — вернуть звук пожара, не держать ПУСК. */
			Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
		}
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

static uint8_t Fire_ZoneLaunchBlocked(uint8_t zone_can)
{
	return PPKY_ZoneLaunchBlockedByCanZone(zone_can);
}

static uint8_t Fire_ZoneIsManual(uint8_t zone_can)
{
	return PPKY_ZoneIsManualByCanZone(zone_can);
}

static void Fire_RefreshAutoOffLed(void)
{
	Led_Set(LED_AUTO_OFF, PPKY_AnyZoneManualOrBlocked() ? 1u : 0u);
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

#if GOST_MODE
static const char *Fire_ChannelTypeShort(uint8_t v_d_type)
{
	switch (v_d_type & 0x7Fu) {
	case DEVICE_DPT_TYPE: return "ДПТ";
	case DEVICE_IGNITER_TYPE: return "СП";
	case DEVICE_BUTTON_TYPE: return "КН";
	case DEVICE_LSWITCH_TYPE: return "КОН";
	default: return "К";
	}
}

static uint8_t Fire_ResolveMcuHAdr(uint8_t zone, uint8_t ch_d_type, uint8_t ch_l_adr,
				   uint8_t id_h_adr, uint8_t id_d_type)
{
	if (Fire_IsMcuDType(id_d_type) && id_h_adr != 0u) {
		return id_h_adr;
	}
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online || !Fire_IsMcuDType(ad->dev.d_type) ||
		    (ad->dev.zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		if (ch_d_type == 0u && ch_l_adr == 0u) {
			return ad->dev.h_adr;
		}
		for (uint8_t vi = 0u; vi < ad->vdev_count; vi++) {
			if ((ad->vdevs[vi].v_d_type & 0x7Fu) == (ch_d_type & 0x7Fu) &&
			    (ad->vdevs[vi].v_l_adr & 0x3Fu) == (ch_l_adr & 0x3Fu)) {
				return ad->dev.h_adr;
			}
		}
	}
	return id_h_adr;
}

static void Fire_SlotRememberUiSource(FireZoneSlot *slot, uint32_t msg_id, const uint8_t *msg_data)
{
	can_ext_id_t id;
	uint8_t ch_d_type = 0u;
	uint8_t ch_l_adr = 0u;

	if (slot == NULL) {
		return;
	}
	id.ID = msg_id & 0x0FFFFFFFu;
	if (msg_data != NULL) {
		/* SetStatusFire: [0]=cmd [1]=origin_d_type [2]=origin_l_adr */
		ch_d_type = msg_data[1];
		ch_l_adr = msg_data[2] & 0x3Fu;
	}
	slot->ui_ch_d_type = ch_d_type;
	slot->ui_ch_l_adr = ch_l_adr;
	slot->ui_mcu_h_adr = Fire_ResolveMcuHAdr(slot->zone, ch_d_type, ch_l_adr,
						 (uint8_t)id.field.h_adr,
						 (uint8_t)(id.field.d_type & 0x7Fu));
	slot->ui_src_valid = 1u;
}
#endif

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

static uint8_t Fire_LogStartTypeFromMode(void)
{
	if (PPKYConfig.fire_mode == 1u) {
		return FIRE_LOG_START_AUTONOMOUS;
	}
	if (PPKYConfig.fire_mode == 0u) {
		return FIRE_LOG_START_AUTO;
	}
	return FIRE_LOG_START_MANUAL;
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
	payload.additional[2] = PPKYConfig.fire_mode;
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
	payload.additional[2] = PPKYConfig.fire_mode;
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
	payload.additional[2] = PPKYConfig.fire_mode;
	(void)EventLog_Post(EVENT_LOG_PANEL_BTN_PRESS, &payload);
}

static void Fire_LogCountdownPause(uint8_t zone, uint8_t source)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = source;
	payload.additional[2] = PPKYConfig.fire_mode;
	(void)EventLog_Post(EVENT_LOG_COUNTDOWN_PAUSE, &payload);
}

static void Fire_LogCountdownResume(uint8_t zone, uint8_t source)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = Fire_LogMasterWagon();
	payload.additional[0] = zone & 0x7Fu;
	payload.additional[1] = source;
	payload.additional[2] = PPKYConfig.fire_mode;
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
	payload.additional[2] = PPKYConfig.fire_mode;
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
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && g_fire.slots[i].zone == zone) {
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

/* Для LED_FIRE: ПОЖАР1 только если все активные зоны ещё в ожидании второй сработки. */
static uint8_t Fire_ShouldUseFire1Led(void)
{
	uint8_t has_fire1 = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		if (!g_fire.slots[i].fire1_waiting) {
			return 0u; /* есть ПОЖАР2 / сценарий / тушение — выше приоритет */
		}
		has_fire1 = 1u;
	}
	return has_fire1;
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
	int8_t si = Fire_FindSlotZone(zone);
	if (si >= 0) {
		FireZoneSlot *existing = &g_fire.slots[(uint8_t)si];
		if (!Fire_AddSourceToSlot(existing, source_key)) {
			return 0u;
		}
#if GOST_MODE
		/* Новый источник / повторный цикл — обновить подпись бегущей строки. */
		Fire_SlotRememberUiSource(existing, msg_id, msg_data);
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
	s->appeared_ms = now_ms;
	(void)Fire_AddSourceToSlot(s, source_key);
#if GOST_MODE
	Fire_SlotRememberUiSource(s, msg_id, msg_data);
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
	int8_t si = Fire_FindSlotZone(zone);
	if (si < 0) {
		return 0u;
	}
	return g_fire.slots[(uint8_t)si].extinguish_locked;
}

static uint8_t Fire_EnsureSlotForZone(uint8_t zone, uint32_t now_ms)
{
	int8_t si = Fire_FindSlotZone(zone);
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
	int8_t si = Fire_FindSlotZone(zone);
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

static uint8_t Fire_CountBlockedFireSlots(void)
{
	uint8_t c = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && !g_fire.slots[i].phase2_sent &&
		    !g_fire.slots[i].fire1_waiting &&
		    Fire_ZoneLaunchBlocked(g_fire.slots[i].zone)) {
			c++;
		}
	}
	return c;
}

/* Зоны, по которым ещё можно вручную дать ПУСК СП (в т.ч. после зонального ОСТАНОВ). */
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
		if (!s->phase2_sent && !s->fire1_waiting) {
			c++;
		}
	}
	return c;
}

static uint8_t Fire_AnyLaunchStopped(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (g_fire.slots[i].active && g_fire.slots[i].launch_stopped) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Fire_MinRemainingSec(uint32_t now_ms)
{
	if (g_fire.zone_countdown_stopped) {
		return 0u;
	}
	uint32_t best_ms = 0xFFFFFFFFu;
	uint8_t found = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active || s->phase2_sent || s->fire1_waiting || s->launch_stopped ||
		    Fire_ZoneLaunchBlocked(s->zone)) {
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
				Fire_MarkSlotPhase2Sent(&g_fire.slots[i], now_ms, Fire_LogStartTypeFromMode());
				any_started = 1u;
			}
		}
	}
	return any_started;
}

static void Fire_Phase2AllPending(void)
{
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active || g_fire.slots[i].fire1_waiting) {
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
		/* ПУСК СП/ОБЩИЙ снимает зональный ОСТАНОВ и запускает фазу 2. */
		g_fire.slots[i].launch_stopped = 0u;
		g_fire.slots[i].countdown_paused = 0u;
		if (Fire_SendPhase2Zone(g_fire.slots[i].zone)) {
			Fire_MarkSlotPhase2Sent(&g_fire.slots[i], HAL_GetTick(), FIRE_LOG_START_MANUAL);
		}
	}
}

static uint8_t Fire_Phase2SelectedPending(uint8_t zone)
{
	uint8_t started = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
			continue;
		}
		if ((g_fire.slots[i].zone & 0x7Fu) != (zone & 0x7Fu)) {
			continue;
		}
		if (g_fire.slots[i].fire1_waiting) {
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

static uint8_t Fire_AllActiveZonesTerminal(void)
{
	uint8_t any_zone = 0u;
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		const FireZoneSlot *s = &g_fire.slots[i];
		if (!s->active) {
			continue;
		}
		any_zone = 1u;
		/* Операторский стоп: зона остаётся в ПОЖАР — не считаем «все терминальны»
		 * (иначе Sync уйдёт в IDLE и баннер ПОЖАР пропадёт). */
		if (s->extinguish_aborted != 0u) {
			return 0u;
		}
		if (!s->phase2_sent) {
			return 0u;
		}
		if (!Fire_ZoneAllIgnitersEndAck(s->zone) && s->ext_retry_failed == 0u) {
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
		g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
	}
}

static uint8_t Fire_BuildUiZoneList(uint8_t *zones, uint8_t max_out)
{
	uint8_t nz = 0u;

	if (zones == NULL || max_out == 0u) {
		return 0u;
	}

	/* Все активные слоты в общем списке (в т.ч. уже потушенные) —
	 * чтобы можно было листать 1/N и видеть «ТУШ.ВЫП.» по зоне. */
	for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
		if (!g_fire.slots[i].active) {
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
			(void)snprintf(name, sizeof(name), "Зона %u", (unsigned)z_can);
		}
#if GOST_MODE
		{
			int8_t si = Fire_FindSlotZone(z_can);
			if (si >= 0 && g_fire.slots[(uint8_t)si].ui_src_valid != 0u) {
				const FireZoneSlot *slot = &g_fire.slots[(uint8_t)si];
				char suffix[28];
				size_t suffix_len;
				size_t name_max;
				if (slot->ui_mcu_h_adr != 0u) {
					(void)snprintf(suffix, sizeof(suffix), " МКУ %u %s%u",
						       (unsigned)slot->ui_mcu_h_adr,
						       Fire_ChannelTypeShort(slot->ui_ch_d_type),
						       (unsigned)slot->ui_ch_l_adr);
				} else {
					(void)snprintf(suffix, sizeof(suffix), " %s%u",
						       Fire_ChannelTypeShort(slot->ui_ch_d_type),
						       (unsigned)slot->ui_ch_l_adr);
				}
				suffix_len = strlen(suffix);
				name_max = (FIRE_UI_NAME_LEN > (suffix_len + 1u))
						   ? (size_t)(FIRE_UI_NAME_LEN - 1u - suffix_len)
						   : 0u;
				(void)snprintf(dst, (size_t)FIRE_UI_NAME_LEN, "%.*s%s",
					       (int)name_max, name, suffix);
			} else {
				(void)snprintf(dst, (size_t)FIRE_UI_NAME_LEN, "%s", name);
			}
		}
#else
		(void)snprintf(dst, (size_t)FIRE_UI_NAME_LEN, "%s", name);
#endif
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

/* Обобщённый индикатор ПОЖАР: ПОЖАР1 непрерывно, ПОЖАР2 и ВНИМАНИЕ мигают.
 * Приоритет: ПОЖАР2 > ПОЖАР1 > ВНИМАНИЕ. */
static void Fire_UpdateLedFire(uint32_t now_ms)
{
	uint8_t mode = FIRE_LED_MODE_OFF;

	if (g_fire.state != FIRE_STATE_IDLE || Fire_AnyActiveSlot()) {
		mode = Fire_ShouldUseFire1Led() ? FIRE_LED_MODE_FIRE1 : FIRE_LED_MODE_FIRE2;
	} else if (Warning_HasActiveAttention()) {
		mode = FIRE_LED_MODE_ATTENTION;
	}

	if (mode != g_fire.led_fire_mode) {
		g_fire.led_fire_mode = mode;
		g_fire.led_toggle_ms = now_ms;
		g_fire.led_fire_on = (mode != FIRE_LED_MODE_OFF) ? 1u : 0u;
	}

	if (mode == FIRE_LED_MODE_OFF) {
		Led_Set(LED_FIRE, 0);
		g_fire.led_fire_on = 0u;
		return;
	}

	if (mode == FIRE_LED_MODE_FIRE1) {
		Led_Set(LED_FIRE, 1u);
		g_fire.led_fire_on = 1u;
		if (g_fire.beeper_alert_active) {
			Led_ForceStatusBright(LED_FIRE);
		}
		return;
	}

	{
		uint32_t half_ms = (mode == FIRE_LED_MODE_FIRE2) ?
				   FIRE_LED_FIRE1_HALF_MS : FIRE_LED_ATTENTION_HALF_MS;
		if (TickAgeExpiredMs(now_ms, g_fire.led_toggle_ms, half_ms) != 0u) {
			g_fire.led_toggle_ms = now_ms;
			g_fire.led_fire_on = (uint8_t)!g_fire.led_fire_on;
		}
		Led_Set(LED_FIRE, g_fire.led_fire_on);
		if (g_fire.led_fire_on) {
			if (g_fire.beeper_alert_active || mode == FIRE_LED_MODE_ATTENTION) {
				Led_ForceStatusBright(LED_FIRE);
			}
		}
	}
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
	Fire_RefreshAutoOffLed();
	/* LED_FIRE — в Fire_UpdateLedFire (в т.ч. ВНИМАНИЕ при IDLE). */
	g_fire.stop_launch_pressed_latched = 0u;
	g_fire.beeper_alert_active = 0u;
	g_fire.beeper_duty_active = 0u;
	g_fire.beeper_start_pattern_active = 0u;
	Beeper_ContinuousOff();
	if (!g_fire.start_all_hold_sound_active) {
		Beeper_StopPattern();
	}
}

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
	g_fire.start_led_hold_until_ms = 0u;
	g_fire.stop_text_blink_until_ms = 0u;
	g_fire.start_sp_text_blink_until_ms = 0u;
	g_fire.all_hold_active = 0u;
	g_fire.all_hold_ms = 0u;
	g_fire.btn_start_all_hold_latched = 0u;
	Fire_StartAllHoldSoundOff();

	Fire_SyncStateFromSlots();
	g_fire.state_start_ms = now_ms;
	g_fire.reply_received = 0u;

	if (!Fire_AnyActiveSlot()) {
		g_fire.state = FIRE_STATE_IDLE;
		g_fire.zone_countdown_stopped = 0u;
		g_fire.zone_countdown_paused = 0u;
		Fire_SetIdleIndication();
		Fire_UpdateLedFire(now_ms);
		{
			char z0[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
			g_fire.last_ui_active = 0xFFu;
			Fire_UpdateUiText(0u, 0u, 0u, 0u, z0);
		}
	} else {
		/* Остались другие зоны — обновить UI/звук через обычный тик FSM. */
		Fire_Transition(FIRE_EVENT_TICK_1MS, now_ms);
	}
}
#endif

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

static void Fire_EnterManualStop(uint32_t now_ms, uint8_t blink_stop_text, uint8_t stop_reason)
{
	/* Общее поведение "ОСТАНОВ ПУСКА": таймеры остановлены, arm на МКУ снят,
	 * дотушивание (повторный Start) запрещено, manual UI/LED. */
	Fire_LogForceStop(stop_reason, 0u);
	g_fire.zone_countdown_stopped = 1u;
	g_fire.zone_countdown_paused = 0u;
	g_fire.start_launch_pressed_latched = 0u;
	g_fire.stop_launch_pressed_latched = 1u;
	g_fire.stop_text_blink_until_ms = blink_stop_text ? (now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u)) : 0u;
	Fire_SendStopAllMcus();
	Fire_AbortExtinguishRetriesAll();
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
	Fire_RefreshAutoOffLed();

	/* Внешний перевод в manual во время активного пожара должен вести себя как кнопка STOP. */
#if !GOST_MODE
	if ((mode == 2u) && Fire_AnyActiveSlot() && (Fire_CountPendingPhase2() > 0u) && !g_fire.zone_countdown_stopped) {
		Fire_EnterManualStop(now_ms, 1u, FIRE_LOG_STOP_MODE_CHANGE);
		return;
	}

	/* 0 и 1 трактуем как auto-поведение на стороне ППКУ. */
	if ((mode != 2u) && (g_fire.state == FIRE_STATE_WAIT_MANUAL) &&
	    !g_fire.zone_countdown_stopped && (Fire_CountPendingPhase2() > 0u)) {
		g_fire.state = FIRE_STATE_WAIT_AUTO;
	}
#else
	(void)now_ms;
	/* В GOST режим по зонам — через zone_fire_mode; глобальный fire_mode не стопает все зоны. */
#endif
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
	Fire_RefreshAutoOffLed();

	uint8_t manual_startable = Fire_CountManualStartable();

	if (manual_startable > 0u) {
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
#if GOST_MODE
			/* Тр. 3.6 / ГОСТ 7.6.3.2а: ПУСК — непрерывное свечение (звук остаётся прерывистым). */
			Led_Set(LED_START, 1u);
#else
			uint32_t phase = (now_ms - g_fire.start_pattern_started_ms) %
					 (BEEPER_PATTERN_START_ON_MS + BEEPER_PATTERN_START_OFF_MS);
			Led_Set(LED_START, (phase < BEEPER_PATTERN_START_ON_MS) ? 1u : 0u);
#endif
		} else {
			Led_Set(LED_START, ((int32_t)(now_ms - g_fire.start_led_hold_until_ms) < 0) ? 1u : 0u);
		}
	} else {
		Led_Set(LED_BUT_START_SP, 0);
		Led_Set(LED_STR_START_SP, 0);
		if (g_fire.stop_launch_pressed_latched || Fire_AnyLaunchStopped()) {
			Led_Set(LED_BUT_STOP, 1);
			if ((int32_t)(now_ms - g_fire.stop_text_blink_until_ms) < 0) {
				uint8_t blink_on = (((now_ms / (FIRE_STOP_TEXT_BLINK_PERIOD_MS / 2u)) & 1u) != 0u) ? 1u : 0u;
				Led_Set(LED_STR_STOP, blink_on);
			} else {
				Led_Set(LED_STR_STOP, 1);
			}
		} else {
			Led_Set(LED_BUT_STOP, 0);
			Led_Set(LED_STR_STOP, 0);
		}
		Fire_SetStartAllBrightness(0u);
		Led_Set(LED_BUT_START_ALL, 0);
		Led_Set(LED_STR_START_ALL, 1);
		if (g_fire.beeper_start_pattern_active) {
#if GOST_MODE
			Led_Set(LED_START, 1u);
#else
			uint32_t phase = (now_ms - g_fire.start_pattern_started_ms) %
					 (BEEPER_PATTERN_START_ON_MS + BEEPER_PATTERN_START_OFF_MS);
			Led_Set(LED_START, (phase < BEEPER_PATTERN_START_ON_MS) ? 1u : 0u);
#endif
		} else {
			Led_Set(LED_START, ((int32_t)(now_ms - g_fire.start_led_hold_until_ms) < 0) ? 1u : 0u);
		}
	}
	Led_Set(LED_STOP, (g_fire.stop_launch_pressed_latched || Fire_AnyLaunchStopped()) ? 1u : 0u);
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
#if GOST_MODE
			g_fire.state = FIRE_STATE_WAIT_AUTO;
#else
			g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
#endif
			g_fire.reply_received = 0u;
		}
		Led_ForceStatusBright(LED_FIRE);
		/* Новый пожар: профиль звука зависит от ПОЖАР1/ПОЖАР2. */
		Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
#if !GOST_MODE
		if (PPKYConfig.fire_mode == 2u) {
			/* В ручном режиме пожар сразу обрабатывается как "ОСТАНОВ ПУСКА". */
			Fire_EnterManualStop(now_ms, 0u, FIRE_LOG_STOP_NO_EVENT);
			fire_processed = 1u;
		}
#else
		Fire_RefreshAutoOffLed();
		fire_processed = 1u;
#endif
		break;
	case FIRE_EVENT_STATUS_FIRE_REDISPLAY:
		/* Повторный пожар в зоне, где тушение уже запускалось: только индикация. */
		if (g_fire.state == FIRE_STATE_IDLE) {
			g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
		}
		Led_ForceStatusBright(LED_FIRE);
		Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
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
		g_fire.btn_start_all_hold_latched = 0u;
		Fire_StartAllHoldSoundOff();
		if (Fire_CountPendingPhase2() > 0u) {
			g_fire.state = (PPKYConfig.fire_mode == 2u) ? FIRE_STATE_WAIT_MANUAL : FIRE_STATE_WAIT_AUTO;
		}
		break;
	case FIRE_EVENT_BTN_START_SP:
		/* ПУСК СП: только при активных слотах пожара и есть зоны без фазы 2 */
		if (!Fire_AnyActiveSlot()) {
			break;
		}
		if (g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL ||
		    g_fire.state == FIRE_STATE_EXTINGUISHING) {
#if GOST_MODE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				/* launch_stopped зоны тоже можно снова запустить ПУСК СП. */
				if (Fire_GetSelectedZoneFromUi(&sel_zone) && Fire_Phase2SelectedPending(sel_zone)) {
					if (g_fire_panel_btn_source != 0u) {
						Fire_LogPanelButton(FIRE_LOG_BTN_START_SP, sel_zone, 0u);
					}
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
			if (Fire_CountPendingPhase2() == 0u) {
				break;
			}
			/* Пуск тушения обработан — индикацию «ОСТАНОВ ПУСКА» снимаем */
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_START_SP, 0u, 0u);
			}
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
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_START_ALL, 0u, 3u);
			}
			BroadcastSetStatusFire(0u, FIRE_STATUS_SRC_PPKU_START_ALL,
			                       DEVICE_PPKY_TYPE, 0u);
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
				g_fire.start_sp_text_blink_until_ms = now_ms + (FIRE_START_SP_TEXT_BLINK_PERIOD_MS * 3u);
				Fire_SyncStateFromSlots();
				fire_processed = 1u;
				start_processed = 1u;
			}
		}
	} break;
	case FIRE_EVENT_BTN_STOP:
		/* ОСТАНОВ ПУСКА.
		 * GOST: по выбранной зоне — pause/resume таймера или стоп тушения (не fire_mode=2).
		 * Иначе: глобальный ручной стоп всех текущих пожаров (как раньше). */
		if (g_fire.start_launch_pressed_latched) {
			/* После ПУСК/автопуска флаг держит LED/звук «ПУСК», но ОСТАНОВ
			 * уже запущенного тушения (фаза 2) должен работать — иначе смысла нет. */
			uint8_t any_phase2 = 0u;
			for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
				if (g_fire.slots[i].active && g_fire.slots[i].phase2_sent != 0u) {
					any_phase2 = 1u;
					break;
				}
			}
			if (any_phase2 == 0u) {
				break;
			}
			g_fire.start_launch_pressed_latched = 0u;
		}
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
			if (!have_zone) {
				/* Без выбора — первая активная зона. */
				for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
					if (g_fire.slots[i].active) {
						sel_zone = g_fire.slots[i].zone;
						have_zone = 1u;
						break;
					}
				}
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
						g_fire.stop_text_blink_until_ms =
							now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u);
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
							g_fire.stop_text_blink_until_ms =
								now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u);
						}
						g_fire.start_launch_pressed_latched = 0u;
						Fire_SyncStateFromSlots();
						fire_processed = 1u;
						break;
					}
					/* ПОЖАР1 / уже ост. / туш.вып — как зональный стоп launch_stopped */
					Fire_LogForceStop(FIRE_LOG_STOP_OPERATOR, sel_zone);
					Fire_SendStopZone(sel_zone);
					g_fire.start_launch_pressed_latched = 0u;
					g_fire.stop_launch_pressed_latched = 1u;
					g_fire.stop_text_blink_until_ms =
						now_ms + (FIRE_STOP_TEXT_BLINK_PERIOD_MS * 3u);
					Fire_SyncStateFromSlots();
					fire_processed = 1u;
					break;
				}
			}
			break;
#else
			if (g_fire_panel_btn_source != 0u) {
				Fire_LogPanelButton(FIRE_LOG_BTN_STOP, 0u, 0u);
			}
			uint8_t manual_mode_initial = (PPKYConfig.fire_mode == 2u) ? 1u : 0u;
			PPKYConfig.fire_mode = 2u;
			g_fire.last_fire_mode = 2u;
			Fire_EnterManualStop(now_ms, manual_mode_initial ? 0u : 1u, FIRE_LOG_STOP_OPERATOR);
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
	/* Пока решение не принято (до ПУСК/СТОП/автопуска), удерживаем тревожный звук активным.
	 * Это защищает от внешних пересечений индикации, которые могут сбросить звук. */
	if ((g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL) &&
	    (g_fire.start_launch_pressed_latched == 0u) &&
	    ((g_fire.stop_launch_pressed_latched == 0u) ||
	     (Fire_CountPendingPhase2() > 0u) || Fire_HasFire1Waiting()) &&
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
			/* Останов: если есть ещё pending/ПОЖАР1 по другим зонам — тревога, иначе дежурный. */
			if (Fire_CountPendingPhase2() > 0u || Fire_HasFire1Waiting()) {
				Fire_BeeperEnterAlert(Fire_ShouldUseFire1Sound());
			} else {
				Fire_BeeperEnterDuty(Fire_ShouldUseFire1Sound());
			}
		}
		Led_SetBrightness(LED_FIRE, LED_STATUS_DIM_BRIGHTNESS);
	}
	Fire_BeeperTick(now_ms);

	if (g_fire.all_hold_active && g_fire.all_hold_ms < 3000u) {
		uint32_t rem_ms = 3000u - g_fire.all_hold_ms;
		ui_remaining = (uint8_t)((rem_ms + 999u) / 1000u);
	} else if (g_fire.state == FIRE_STATE_WAIT_AUTO || g_fire.state == FIRE_STATE_WAIT_MANUAL) {
		if (!g_fire.zone_countdown_stopped) {
#if GOST_MODE
			if (g_fire_ui_manual_select_enabled) {
				uint8_t sel_zone = 0u;
				if (Fire_GetSelectedZoneFromUi(&sel_zone)) {
					int8_t si = Fire_FindSlotZone(sel_zone);
					if (Fire_ZoneLaunchBlocked(sel_zone)) {
						ui_mode = 8u; /* ПУСК ЗАБЛ. */
						ui_remaining = 0u;
					} else if (si >= 0 && g_fire.slots[(uint8_t)si].extinguish_aborted != 0u) {
						ui_mode = 9u; /* ТУШЕНИЕ ОСТ. */
						ui_remaining = 0u;
					} else if (si >= 0 && g_fire.slots[(uint8_t)si].phase2_sent != 0u &&
						   Fire_ZoneAllIgnitersEndAck(sel_zone)) {
						ui_mode = 3u; /* ТУШ.ВЫП. по выбранной зоне */
						ui_remaining = 0u;
					} else if (si >= 0 && g_fire.slots[(uint8_t)si].countdown_paused != 0u) {
						ui_mode = 5u; /* ПАУЗА */
						ui_remaining = (uint8_t)((g_fire.slots[(uint8_t)si].paused_remaining_ms + 999u) / 1000u);
					} else if (si >= 0 && g_fire.slots[(uint8_t)si].launch_stopped) {
						ui_mode = 4u; /* ПОЖАР/ОСТ. ПУСКА по выбранной зоне */
						ui_remaining = 0u;
					} else if (Fire_ZoneIsFire1Waiting(sel_zone)) {
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
				} else if (Fire_CountPendingPhase2() == 0u && Fire_CountBlockedFireSlots() > 0u) {
					ui_mode = 8u; /* ПУСК ЗАБЛ. */
					ui_remaining = 0u;
				} else {
					uint8_t any_pending = 0u;
					uint8_t all_paused = 1u;
					for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
						const FireZoneSlot *s = &g_fire.slots[i];
						if (!s->active || s->phase2_sent || s->fire1_waiting ||
						    s->launch_stopped || Fire_ZoneLaunchBlocked(s->zone)) {
							continue;
						}
						any_pending = 1u;
						if (s->countdown_paused == 0u && g_fire.zone_countdown_paused == 0u) {
							all_paused = 0u;
						}
					}
					ui_mode = (any_pending && all_paused) || g_fire.zone_countdown_paused
						  ? 5u : 1u; /* ПАУЗА или ДО ПУСКА */
				}
			}
		} else {
			ui_remaining = 0u;
			ui_mode = 4u; /* ПОЖАР/ОСТ. ПУСКА */
		}
	} else if (g_fire.state == FIRE_STATE_EXTINGUISHING) {
		ui_remaining = 0u;
		ui_mode = 2u; /* ТУШЕНИЕ */
#if GOST_MODE
		if (g_fire_ui_manual_select_enabled) {
			uint8_t sel_zone = 0u;
			if (Fire_GetSelectedZoneFromUi(&sel_zone)) {
				int8_t si = Fire_FindSlotZone(sel_zone);
				if (si >= 0 && g_fire.slots[(uint8_t)si].extinguish_aborted != 0u) {
					ui_mode = 9u; /* ТУШЕНИЕ ОСТ. */
				} else if (si >= 0 && g_fire.slots[(uint8_t)si].ext_retry_failed != 0u) {
					ui_mode = 7u;
				} else if (si >= 0 && g_fire.slots[(uint8_t)si].phase2_sent != 0u &&
					   Fire_ZoneAllIgnitersEndAck(sel_zone)) {
					ui_mode = 3u; /* ТУШ.ВЫП. по выбранной зоне */
				} else if (si >= 0 && g_fire.slots[(uint8_t)si].launch_stopped != 0u &&
					   g_fire.slots[(uint8_t)si].phase2_sent == 0u) {
					ui_mode = 4u; /* ещё в останове пуска */
				} else if (si >= 0 && g_fire.slots[(uint8_t)si].countdown_paused != 0u) {
					ui_mode = 5u;
					ui_remaining = (uint8_t)((g_fire.slots[(uint8_t)si].paused_remaining_ms + 999u) / 1000u);
				} else if (Fire_ZoneIsFire1Waiting(sel_zone)) {
					ui_mode = 6u;
				} else if (si >= 0 && g_fire.slots[(uint8_t)si].phase2_sent == 0u &&
					   !Fire_ZoneLaunchBlocked(sel_zone)) {
					ui_remaining = Fire_RemainingSecForZone(sel_zone, now_ms);
					ui_mode = 1u;
				}
			}
		}
#endif
		if (ui_mode == 2u) {
			if (Fire_AllActiveZonesEndAck()) {
				ui_mode = 3u; /* ТУШЕНИЕ ПРОИЗВЕДЕНО */
				Fire_LogExtinguishComplete();
			} else if (Fire_AllActiveZonesTerminal() && Fire_HasExtinguishFailure()) {
				ui_mode = 7u; /* ТУШЕНИЕ НЕ ВЫПОЛНЕНО */
#if GOST_MODE
			} else if (!g_fire_ui_manual_select_enabled) {
				/* Все фазы 2 остановлены оператором — «ТУШ.ОСТ.» без UI-выбора. */
				uint8_t any_p2 = 0u;
				uint8_t all_aborted = 1u;
				for (uint8_t i = 0u; i < FIRE_MAX_SLOTS; i++) {
					const FireZoneSlot *s = &g_fire.slots[i];
					if (!s->active || !s->phase2_sent) {
						continue;
					}
					any_p2 = 1u;
					if (s->extinguish_aborted == 0u) {
						all_aborted = 0u;
						break;
					}
				}
				if (any_p2 && all_aborted) {
					ui_mode = 9u;
				}
#endif
			}
		}
	}

	if (g_fire.state == FIRE_STATE_IDLE) {
		if (g_fire.all_hold_active && g_fire.all_hold_ms < 3000u) {
			/* Без пожара: показываем только 3-сек таймер удержания ПУСК ОБЩИЙ и мигание подписи */
			Fire_SetIdleIndication();
			Fire_UpdateLedFire(now_ms);
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
		Fire_UpdateLedFire(now_ms);
		if (g_fire.start_launch_pressed_latched) {
			Led_Set(LED_START, 1u);
		}
		{
			char z0[FIRE_UI_MAX_ZONES][FIRE_UI_NAME_LEN];
			Fire_UpdateUiText(0u, 0u, 0u, 0u, z0);
		}
		return;
	}

	Fire_UpdateLedFire(now_ms);
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
#if GOST_MODE
	Fire_ResetSuppressClear();
#endif
	g_fire.state = FIRE_STATE_IDLE;
	g_fire.start_all_is_bright = 0xFFu;
	Fire_SetStartAllBrightness(0u);
	Led_Set(LED_BUT_START_ALL, 0u);
	Led_Set(LED_STR_START_ALL, 1u);
	g_fire.start_all_is_bright = 0u;
	g_fire.last_ui_nzones = 0u;
	g_fire.last_fire_mode = PPKYConfig.fire_mode;
	Fire_RefreshAutoOffLed();
	g_fire_ui_manual_select_enabled = 0u;
	g_fire_ui_selected_index = 0u;
}

/* Периодический тик 1 мс: FSM, таймеры автопуска и UI-обновления. */
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
		/* IDLE: LED_FIRE (ВНИМАНИЕ) + актуальный LED_AUTO_OFF по режимам зон. */
		Fire_UpdateLedFire(now);
		Fire_RefreshAutoOffLed();
		return;
	}
	Fire_Transition(FIRE_EVENT_TICK_1MS, now);
}

/* Периодический тик 10 мс: обработка кнопок и удержания ПУСК ОБЩИЙ. */
void Fire_Timer10ms(void)
{
	uint32_t now_ms = HAL_GetTick();
	/* ПУСК ОБЩИЙ обрабатываем даже во время config-сессии: удержание поднимает
	 * all_hold_active, UI возвращает на главный экран со счётчиком 3с. */
	ButtonState st_start_all = Button_GetState(BUT_FORCE);
	if (st_start_all == ButtonStatePress || st_start_all == ButtonStateLongPress) {
		if (!g_fire.all_hold_active) {
			g_fire.all_hold_active = 1u;
			g_fire.all_hold_ms = 0u;
			g_fire.state_start_ms = now_ms;
			Fire_StartAllHoldSoundOn();
			/* Сырое касание ПУСК ОБЩИЙ — до удержания 3 с (код 14 после hold). */
			Fire_LogPanelBtnPress(FIRE_LOG_BTN_START_ALL, 0u);
		} else {
			if (g_fire.all_hold_ms < 3000u) {
				g_fire.all_hold_ms += 10u;
			}
			if (g_fire.all_hold_ms >= 3000u && g_fire.btn_start_all_hold_latched == 0u) {
				g_fire.btn_start_all_hold_latched = 1u;
				Fire_StartAllHoldSoundOff();
				if (g_fire.last_btn_start_all_action_ms == 0u ||
				    (now_ms - g_fire.last_btn_start_all_action_ms) >= 500u) {
					g_fire.last_btn_start_all_action_ms = now_ms;
					g_fire_panel_btn_source = 1u;
					Fire_Transition(FIRE_EVENT_BTN_START_ALL, now_ms);
					g_fire_panel_btn_source = 0u;
				}
			}
		}
	} else if (st_start_all == ButtonStateReset && g_fire.all_hold_active) {
		g_fire.all_hold_active = 0u;
		g_fire.all_hold_ms = 0u;
		g_fire.btn_start_all_hold_latched = 0u;
		Fire_StartAllHoldSoundOff();
		/* Сброс кэша UI: иначе после неполного удержания мог не уйти active=0. */
		g_fire.last_ui_active = 0xFFu;
		/* Обновить UI/LED сразу после отпускания кнопки, даже если пожара нет */
		Fire_Transition(FIRE_EVENT_TICK_1MS, now_ms);
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
		} else if (st_stop_hold == ButtonStateReset && g_fire.gost_stop_hold_active != 0u) {
			g_fire.gost_stop_hold_active = 0u;
			g_fire.gost_stop_hold_ms = 0u;
			g_fire.gost_stop_reset_latched = 0u;
		}
	}
#endif

	/* Сырое нажатие ПУСК СП / ОСТАНОВ — всегда (в т.ч. IDLE и config). */
	uint8_t pressed_sp = Fire_ButtonPressedEvent(BUT_FIRE, &g_fire.btn_start_sp_latched);
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
	if (pressed_stop != 0u) {
		g_fire_panel_btn_source = 1u;
		Fire_Transition(FIRE_EVENT_BTN_STOP, HAL_GetTick());
		g_fire_panel_btn_source = 0u;
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
	uint32_t source_key = Fire_SourceKeyFromMsgId(msg_id);
	uint8_t and_effective = Fire_IsAndEffectiveForZone(zone);
	uint32_t now = HAL_GetTick();
	uint8_t res = Fire_TryAddNewFireZone(zone, source_key, and_effective, now, msg_id, msg_data);
	if (res == 1u || res == 2u) {
		Fire_LogFireDetected(msg_id, msg_data, zone);
		/* Новая зона / ПОЖАР1->ПОЖАР2: слот и фаза 1, затем FSM */
		Fire_Transition(FIRE_EVENT_STATUS_FIRE, now);
	} else if (res == 3u) {
		/* Новый пожар в зоне с уже запущенным тушением: только отображение. */
		Fire_Transition(FIRE_EVENT_STATUS_FIRE_REDISPLAY, now);
	}
	SetReplyStatusFire(zone);
}

void Fire_OnBusStartSpButton(uint32_t msg_id)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}
	if (!Backend_IsIgniterBroadcastId(msg_id)) {
		return;
	}
	Fire_Transition(FIRE_EVENT_BTN_START_SP, HAL_GetTick());
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
	return g_fire.all_hold_active ? 1u : 0u;
}

void Fire_UiSetManualSelection(uint8_t enabled, uint8_t selected_ui_index)
{
	g_fire_ui_manual_select_enabled = enabled ? 1u : 0u;
	g_fire_ui_selected_index = selected_ui_index;
}

void Fire_NotifyZoneModeChanged(void)
{
	const uint8_t on = PPKY_AnyZoneManualOrBlocked() ? 1u : 0u;
	Led_Set(LED_AUTO_OFF, on);
	if (on != 0u) {
		Led_ForceStatusBright(LED_AUTO_OFF);
	}
}
