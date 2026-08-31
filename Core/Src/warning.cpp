#include "config_monitor.h"

#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "backend.h"
#include "event_log.h"
#include "fire.h"
#include "gost_mode.h"
#include "led.h"
#include "beeper.h"
#include "can_bus.h"
#include "device_config.h"
#include "device_dpt.hpp"
#include "sound_profiles.h"
#include "menu_ui.h"

#define WARN_TITLE_LEN 24

extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;
extern PPKYCfg PPKYConfig;

extern "C" void Warning_UiUpdate(uint8_t active, uint8_t n_items,
				 char (*big_titles)[WARN_TITLE_LEN],
				 char (*details)[ZONE_NAME_SIZE + 1]);

enum FaultSoundPhase : uint8_t {
	FAULT_SOUND_IDLE = 0u,
	FAULT_SOUND_WAIT_PERIODIC,
	FAULT_SOUND_PERIODIC
};
enum AttentionSoundPhase : uint8_t {
	ATTN_SOUND_IDLE = 0u,
	ATTN_SOUND_WAIT_PERIODIC,
	ATTN_SOUND_PERIODIC
};
static uint8_t g_power_fault_mask = 0u;
static uint8_t g_ppku_input_fault_mask = 0u;
static uint8_t g_raw_power_fault_mask = 0u;
static uint8_t g_raw_ppku_input_fault_mask = 0u;
static uint32_t g_power_confirm_since[4];
static uint8_t g_power_confirm_pending[4];
static uint8_t g_prev_power_fault_mask = 0u;
static uint8_t g_prev_ppku_input_fault_mask = 0u;
static FaultSoundPhase g_fault_sound_phase = FAULT_SOUND_IDLE;
static uint32_t g_fault_sound_deadline_ms = 0u;
static AttentionSoundPhase g_attention_sound_phase = ATTN_SOUND_IDLE;
static uint32_t g_attention_sound_deadline_ms = 0u;

extern "C" uint8_t Warning_HasActiveFault(void);

namespace {

constexpr uint8_t WARN_MAX_ITEMS = 16u;
constexpr uint32_t WARNING_SHOW_HOLD_MS = 10000u; /* Показывать предупреждение ещё 10 с после исчезновения */
constexpr uint32_t WARN_CONFIRM_MS = 3000u;       /* Подтверждение неисправности перед реакцией/логом */
constexpr uint8_t WARN_KIND_VDEV_FAULT = 0u;
constexpr uint8_t WARN_KIND_MCU_CAN_FAULT = 1u;
constexpr uint8_t WARN_KIND_PPKU_CAN_FAULT = 2u;
constexpr uint8_t WARN_KIND_LSWITCH_OPEN_ATTN = 3u;
constexpr uint8_t WARN_KIND_DPT_WARNING_ATTN = 4u;
constexpr uint8_t WARN_KIND_MCU_POSITION_FAULT = 5u;
constexpr uint8_t WARN_KIND_DEVICE_MISSING = 6u;
constexpr uint8_t WARN_KIND_DEVICE_FOUND = 7u;
constexpr uint8_t WARN_KIND_CONFIG_MISMATCH = 8u;
constexpr uint8_t WARN_TITLE_MARK_ATTN = 0x01u;

struct WarningItem {
	uint8_t used;
	uint8_t kind; /* 0/1/2=fault, 3/4=attention */
	uint8_t zone;
	uint8_t h_adr;
	uint8_t v_l_adr;
	uint8_t mcu_d_type;
	uint8_t v_d_type;
	uint8_t line_state; /* 1=обрыв, 2=КЗ */
	uint8_t can_idx; /* 1 или 2 для CAN-предупреждений */
	int16_t extra; /* произвольный параметр (например температура DPT) */
	uint8_t fault_now;
	uint8_t confirm_pending;
	uint32_t confirm_since_ms;
	uint32_t show_until_ms;
	uint32_t appeared_ms; /* момент появления (новое сверху в UI) */
};

static WarningItem g_items[WARN_MAX_ITEMS];

static uint8_t g_last_active = 0xFFu;
static uint8_t g_last_count = 0xFFu;
static char g_last_big[WARN_MAX_ITEMS][WARN_TITLE_LEN];
static char g_last_details[WARN_MAX_ITEMS][ZONE_NAME_SIZE + 1];
static uint8_t g_led_err_on = 0u;
static uint8_t g_prev_active_fault_count = 0u;
static uint8_t g_prev_sound_fault_count = 0u;
static uint8_t g_prev_sound_attention_count = 0u;
static uint32_t g_position_fault_mask = 0u;

/* Текстовое имя типа МКУ для отображения в UI предупреждений. */
static const char* McuTypeName(uint8_t d_type)
{
	switch (d_type) {
	case DEVICE_MCU_IGN_TYPE: return "MKU IGN";
	case DEVICE_MCU_TC_TYPE:  return "MKU TC";
	case DEVICE_MCU_K1:       return "MKU K1";
	case DEVICE_MCU_K2:       return "MKU K2";
	case DEVICE_MCU_K3:       return "MKU K3";
	case DEVICE_MCU_KR:       return "MKU KR";
	default:                  return "MKU";
	}
}

static const char* Warning_McuTypeSerialToken(uint8_t d_type)
{
	switch (d_type & 0x7Fu) {
	case DEVICE_MCU_IGN_TYPE: return "IGN";
	case DEVICE_MCU_TC_TYPE:  return "TC";
	case DEVICE_MCU_K1:       return "K1";
	case DEVICE_MCU_K2:       return "K2";
	case DEVICE_MCU_K3:       return "K3";
	case DEVICE_MCU_KR:       return "KR";
	default:                  return "MKU";
	}
}

static const char* Warning_ChannelTypeShort(uint8_t v_d_type)
{
	switch (v_d_type) {
	case DEVICE_DPT_TYPE: return "ДПТ";
	case DEVICE_IGNITER_TYPE: return "СП";
	case DEVICE_BUTTON_TYPE: return "КН";
	case DEVICE_LSWITCH_TYPE: return "КОН";
	/* Отдельного DT-типа в текущих статусах нет, оставляем задел под расширение. */
	default: return "???";
	}
}

static void Warning_GetZoneName(const WarningItem& it, char *out, size_t out_sz)
{
	if (out_sz == 0u) {
		return;
	}
	out[0] = '\0';
	if (it.zone > 0u && it.zone <= ZONE_NUMBER) {
		strncpy(out, reinterpret_cast<const char*>(PPKYConfig.zone_name[it.zone - 1u]), ZONE_NAME_SIZE);
		out[(out_sz - 1u) < ZONE_NAME_SIZE ? (out_sz - 1u) : ZONE_NAME_SIZE] = '\0';
		/* Убрать хвостовые пробелы из фиксированного поля имени зоны. */
		size_t n = strlen(out);
		while (n > 0u && out[n - 1u] == ' ') {
			out[--n] = '\0';
		}
	}
	if (out[0] == '\0') {
		snprintf(out, out_sz, "ЗОНА %u", (unsigned)it.zone);
	}
}


static void Warning_GetSerialPlaceholder(const WarningItem& it, char *out, size_t out_sz)
{
	for (uint8_t i = 0; i < 32; i++) {
		if ((it.h_adr == PPKYConfig.CfgDevices[i].UId.devId.h_adr) &&
		    (it.mcu_d_type == PPKYConfig.CfgDevices[i].UId.devId.d_type)) {
			snprintf(out, out_sz, "S/N:%08lX:%08lX:%08lX",
				 PPKYConfig.CfgDevices[i].UId.UId0,
				 PPKYConfig.CfgDevices[i].UId.UId1,
				 PPKYConfig.CfgDevices[i].UId.UId2);
			return;
		}
	}

	Device dev = {};
	dev.zone = it.zone;
	dev.h_adr = it.h_adr;
	dev.l_adr = it.v_l_adr != 0u ? 0u : 0u;
	dev.d_type = it.mcu_d_type;
	uint8_t remote_valid = 0u;
	const uint8_t *remote = ConfigMonitor_GetRemoteSerial(&dev, &remote_valid);
	if (remote_valid != 0u && remote != nullptr) {
		const uint32_t *uid = (const uint32_t *)remote;
		snprintf(out, out_sz, "S/N:%08lX:%08lX:%08lX",
			 (unsigned long)uid[0], (unsigned long)uid[1], (unsigned long)uid[2]);
		return;
	}

	snprintf(out, out_sz, "S/N:---");
}

static void Warning_FormatMkuAndSerial(char *out, size_t out_sz, const WarningItem& it)
{
	char serial[24];
	char zone_name[ZONE_NAME_SIZE + 1];
	Warning_GetSerialPlaceholder(it, serial, sizeof(serial));
	Warning_GetZoneName(it, zone_name, sizeof(zone_name));
	snprintf(out, out_sz, "%s %s %u %s",
		 zone_name, McuTypeName(it.mcu_d_type), (unsigned)it.h_adr, serial);
}

/* Фильтр типов виртуальных устройств, участвующих в модуле неисправностей. */
static uint8_t IsTrackedVdevType(uint8_t v_d_type)
{
	return (v_d_type == DEVICE_DPT_TYPE ||
		v_d_type == DEVICE_IGNITER_TYPE ||
		v_d_type == DEVICE_LSWITCH_TYPE) ? 1u : 0u;
}

/* Проверяет, что состояние линии относится к неисправности.
 * line_state=5 (Fault) — протокол для DPT/BUTTON/LSWITCH.
 */
static uint8_t IsFaultLineStateForVdev(uint8_t v_d_type, uint8_t line_state)
{
	(void)v_d_type;
	if (line_state == 1u || line_state == 2u || line_state == 5u) {
		return 1u; /* 1=Обрыв, 2=КЗ, 5=Неисправность */
	}
	return 0u;
}

/* fault_class для EVENT_LOG_DEVICE_FAULT (catalog additional[0]). */
enum : uint8_t {
	FAULT_CLASS_LINE_BREAK = 0u,
	FAULT_CLASS_LINE_SHORT = 1u,
	FAULT_CLASS_PROTOCOL   = 2u,
	FAULT_CLASS_CAN        = 3u,
	FAULT_CLASS_POWER      = 4u,
	FAULT_CLASS_OTHER      = 5u,
	FAULT_CLASS_POSITION   = 6u
};

static uint8_t IsAttentionKind(uint8_t kind)
{
	return (kind == WARN_KIND_LSWITCH_OPEN_ATTN || kind == WARN_KIND_DPT_WARNING_ATTN) ? 1u : 0u;
}

/* В ГОСТ режим ВНИМАНИЕ (концевик/ДПТ Warning) не используем: ни список, ни звук, ни LED. */
static uint8_t AttentionEventsEnabled(void)
{
#if GOST_MODE
	return 0u;
#else
	return 1u;
#endif
}

static uint8_t IsFaultKind(uint8_t kind)
{
	return (kind == WARN_KIND_VDEV_FAULT ||
		kind == WARN_KIND_MCU_CAN_FAULT ||
		kind == WARN_KIND_PPKU_CAN_FAULT ||
		kind == WARN_KIND_MCU_POSITION_FAULT ||
		kind == WARN_KIND_DEVICE_MISSING ||
		kind == WARN_KIND_DEVICE_FOUND ||
		kind == WARN_KIND_CONFIG_MISMATCH) ? 1u : 0u;
}

static uint32_t BuildFaultCanHeader(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone)
{
	can_ext_id_t id;

	id.ID = 0u;
	id.field.zone = zone & 0x7Fu;
	id.field.l_adr = l_adr & 0x3Fu;
	id.field.h_adr = h_adr;
	id.field.d_type = d_type & 0x7Fu;
	id.field.dir = 1u; /* статус / ответ устройства */
	return id.ID & 0x1FFFFFFFu;
}

static uint8_t FaultClassFromWarningItem(const WarningItem& it)
{
	if (it.kind == WARN_KIND_MCU_CAN_FAULT || it.kind == WARN_KIND_PPKU_CAN_FAULT) {
		return FAULT_CLASS_CAN;
	}
	if (it.kind == WARN_KIND_MCU_POSITION_FAULT) {
		return FAULT_CLASS_POSITION;
	}
	if (it.line_state == 1u) {
		return FAULT_CLASS_LINE_BREAK;
	}
	if (it.line_state == 2u) {
		return FAULT_CLASS_LINE_SHORT;
	}
	if (it.line_state == 5u) {
		return FAULT_CLASS_PROTOCOL;
	}
	return FAULT_CLASS_OTHER;
}

static void FillVdevStatusCanData(const WarningItem& it, uint8_t *can_data)
{
	memset(can_data, 0, 8u);
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo *m = &g_active_devices[mi];
		if (!m->online) {
			continue;
		}
		if (m->dev.zone != it.zone || m->dev.h_adr != it.h_adr || m->dev.d_type != it.mcu_d_type) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			auto *v = &m->vdevs[vi];
			if (!v->online || v->v_l_adr != it.v_l_adr || v->v_d_type != it.v_d_type) {
				continue;
			}
			can_data[0] = v->status_cmd;
			memcpy(&can_data[1], v->status_params, 7u);
			return;
		}
	}
}

static void EventLog_PostDeviceFaultItem(const WarningItem& it, uint8_t cleared)
{
	EventLogPayload_t payload;

	if (!IsFaultKind(it.kind)) {
		return;
	}

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.additional[0] = FaultClassFromWarningItem(it);
	payload.additional[2] = cleared ? 1u : 0u; /* 0=появилась, 1=устранена */

	if (it.kind == WARN_KIND_VDEV_FAULT) {
		payload.can_header = BuildFaultCanHeader(it.v_d_type, it.h_adr, it.v_l_adr, it.zone);
		FillVdevStatusCanData(it, payload.can_data);
		payload.additional[1] = it.v_l_adr;
	} else if (it.kind == WARN_KIND_MCU_CAN_FAULT) {
		payload.can_header = BuildFaultCanHeader(it.mcu_d_type, it.h_adr, 0u, it.zone);
		payload.can_data[0] = it.line_state;
		payload.can_data[1] = it.can_idx;
		payload.additional[1] = it.can_idx;
	} else if (it.kind == WARN_KIND_PPKU_CAN_FAULT) {
		payload.can_header = BuildFaultCanHeader(DEVICE_PPKY_TYPE,
							 PPKYConfig.UId.devId.h_adr, 0u, 0u);
		payload.can_data[0] = it.line_state;
		payload.can_data[1] = it.can_idx;
		payload.additional[1] = it.can_idx;
	} else if (it.kind == WARN_KIND_MCU_POSITION_FAULT) {
		payload.can_header = BuildFaultCanHeader(DEVICE_PPKY_TYPE,
							 PPKYConfig.UId.devId.h_adr, 0u, 0u);
		payload.can_data[0] = it.h_adr;
		payload.additional[1] = it.h_adr;
	}

	(void)EventLog_Post(EVENT_LOG_DEVICE_FAULT, &payload);
}

static void EventLog_PostPowerFault(uint8_t channel, uint8_t is_ppku_input, uint8_t cleared)
{
	EventLogPayload_t payload;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.can_header = BuildFaultCanHeader(DEVICE_PPKY_TYPE,
						 PPKYConfig.UId.devId.h_adr, 0u, 0u);
	payload.can_data[0] = is_ppku_input ? 1u : 0u; /* 0=выход power, 1=вход ППКУ */
	payload.can_data[1] = channel;
	payload.additional[0] = FAULT_CLASS_POWER;
	payload.additional[1] = channel;
	payload.additional[2] = cleared ? 1u : 0u;
	(void)EventLog_Post(EVENT_LOG_DEVICE_FAULT, &payload);
}

static void SyncPowerFaultEventLog(void)
{
	uint8_t power_new = (uint8_t)(g_power_fault_mask & (uint8_t)(~g_prev_power_fault_mask));
	uint8_t input_new = (uint8_t)(g_ppku_input_fault_mask & (uint8_t)(~g_prev_ppku_input_fault_mask));
	uint8_t power_clr = (uint8_t)(g_prev_power_fault_mask & (uint8_t)(~g_power_fault_mask));
	uint8_t input_clr = (uint8_t)(g_prev_ppku_input_fault_mask & (uint8_t)(~g_ppku_input_fault_mask));

	for (uint8_t ch = 0u; ch < 2u; ch++) {
		if ((power_new & (1u << ch)) != 0u) {
			EventLog_PostPowerFault((uint8_t)(ch + 1u), 0u, 0u);
		}
		if ((input_new & (1u << ch)) != 0u) {
			EventLog_PostPowerFault((uint8_t)(ch + 1u), 1u, 0u);
		}
		if ((power_clr & (1u << ch)) != 0u) {
			EventLog_PostPowerFault((uint8_t)(ch + 1u), 0u, 1u);
		}
		if ((input_clr & (1u << ch)) != 0u) {
			EventLog_PostPowerFault((uint8_t)(ch + 1u), 1u, 1u);
		}
	}

	g_prev_power_fault_mask = g_power_fault_mask;
	g_prev_ppku_input_fault_mask = g_ppku_input_fault_mask;
}

/* Поиск записи неисправности по ключу устройства/канала. */
static int FindItem(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
		    uint8_t mcu_d_type, uint8_t v_d_type, uint8_t can_idx)
{
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used) {
			continue;
		}
		if (g_items[i].kind == kind &&
		    g_items[i].zone == zone &&
		    g_items[i].h_adr == h_adr &&
		    g_items[i].v_l_adr == v_l_adr &&
		    g_items[i].mcu_d_type == mcu_d_type &&
		    g_items[i].v_d_type == v_d_type &&
		    g_items[i].can_idx == can_idx) {
			return (int)i;
		}
	}
	return -1;
}

/* Безопасное сравнение таймера с учётом переполнения HAL_GetTick(). */
static uint8_t TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
	return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1u : 0u;
}

static uint8_t IsConfigFaultKind(uint8_t kind)
{
	return (kind == WARN_KIND_DEVICE_MISSING ||
		kind == WARN_KIND_DEVICE_FOUND ||
		kind == WARN_KIND_CONFIG_MISMATCH) ? 1u : 0u;
}

static uint8_t ConfirmElapsed(const WarningItem& it, uint32_t now_ms)
{
	return (it.confirm_pending != 0u &&
		TimeReached(now_ms, it.confirm_since_ms + WARN_CONFIRM_MS)) ? 1u : 0u;
}

/* Удаляет запись без записи в журнал (кратковременное/неподтверждённое событие). */
static void ClearItemAt(uint8_t idx)
{
	if (idx >= WARN_MAX_ITEMS || !g_items[idx].used) {
		return;
	}
	memset(&g_items[idx], 0, sizeof(g_items[idx]));
}

static void BeginConfirm(WarningItem& it, uint32_t now_ms)
{
	if (it.confirm_pending == 0u) {
		it.confirm_pending = 1u;
		it.confirm_since_ms = now_ms;
	}
}

static uint8_t PromoteToActiveFault(uint8_t idx, uint32_t now_ms)
{
	if (idx >= WARN_MAX_ITEMS || !g_items[idx].used || g_items[idx].fault_now != 0u) {
		return 0u;
	}
	g_items[idx].fault_now = 1u;
	g_items[idx].confirm_pending = 0u;
	g_items[idx].appeared_ms = now_ms;
	g_items[idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
	if (IsFaultKind(g_items[idx].kind) && !IsConfigFaultKind(g_items[idx].kind)) {
		EventLog_PostDeviceFaultItem(g_items[idx], 0u);
	}
	return 1u;
}

/* Удаляет запись неисправности по индексу.
 * Если запись была активной неисправностью — пишет событие устранения. */
static void RemoveItemAt(uint8_t idx)
{
	if (idx >= WARN_MAX_ITEMS || !g_items[idx].used) {
		return;
	}
	if (IsFaultKind(g_items[idx].kind) && g_items[idx].fault_now != 0u) {
		EventLog_PostDeviceFaultItem(g_items[idx], 1u);
	}
	memset(&g_items[idx], 0, sizeof(g_items[idx]));
}

/* Добавляет/обновляет запись неисправности и продлевает окно отображения.
 * Возвращает 1 при появлении новой активной неисправности (фронт). */
static uint8_t UpsertItem(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
		       uint8_t mcu_d_type, uint8_t v_d_type, uint8_t line_state,
		       uint8_t can_idx, int16_t extra, uint32_t now_ms)
{
	int idx = FindItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, can_idx);
	if (idx >= 0) {
		uint8_t became_active = 0u;
		g_items[(uint8_t)idx].line_state = line_state;
		g_items[(uint8_t)idx].can_idx = can_idx;
		g_items[(uint8_t)idx].extra = extra;
		if (g_items[(uint8_t)idx].fault_now != 0u) {
			g_items[(uint8_t)idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
		} else {
			BeginConfirm(g_items[(uint8_t)idx], now_ms);
			if (ConfirmElapsed(g_items[(uint8_t)idx], now_ms)) {
				became_active = PromoteToActiveFault((uint8_t)idx, now_ms);
			}
		}
		return became_active;
	}
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (g_items[i].used) {
			continue;
		}
		g_items[i].used = 1u;
		g_items[i].kind = kind;
		g_items[i].zone = zone;
		g_items[i].h_adr = h_adr;
		g_items[i].v_l_adr = v_l_adr;
		g_items[i].mcu_d_type = mcu_d_type;
		g_items[i].v_d_type = v_d_type;
		g_items[i].line_state = line_state;
		g_items[i].can_idx = can_idx;
		g_items[i].extra = extra;
		g_items[i].fault_now = 0u;
		g_items[i].confirm_pending = 1u;
		g_items[i].confirm_since_ms = now_ms;
		g_items[i].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
		g_items[i].appeared_ms = now_ms;
		return 0u;
	}
	return 0u;
}

/* Помечает неисправность как восстановленную (с хвостом отображения). */
static void MarkRecovered(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
			  uint8_t mcu_d_type, uint8_t v_d_type, uint8_t can_idx, uint32_t now_ms)
{
	int idx = FindItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, can_idx);
	if (idx >= 0) {
		if (g_items[(uint8_t)idx].fault_now == 0u && g_items[(uint8_t)idx].confirm_pending != 0u) {
			ClearItemAt((uint8_t)idx);
			return;
		}
		if (IsFaultKind(kind) && g_items[(uint8_t)idx].fault_now != 0u) {
			EventLog_PostDeviceFaultItem(g_items[(uint8_t)idx], 1u);
		}
		g_items[(uint8_t)idx].fault_now = 0u;
		g_items[(uint8_t)idx].confirm_pending = 0u;
		g_items[(uint8_t)idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
	}
}

static void ProcessPendingConfirmations(uint32_t now_ms);
static uint8_t IsItemStillFaulty(const WarningItem& it);

static uint8_t IsConfigItemStillActive(const WarningItem& it)
{
	for (uint8_t slot = 0u; slot < 32u; slot++) {
		const Device *dev = ConfigMonitor_GetCfgDevice(slot);
		if (dev == nullptr || dev->d_type == 0u) {
			continue;
		}
		if (dev->zone != it.zone || dev->h_adr != it.h_adr || dev->d_type != it.mcu_d_type) {
			continue;
		}
		if (it.kind == WARN_KIND_DEVICE_MISSING) {
			return ConfigMonitor_IsMcuMissingLatched(slot) ? 1u : 0u;
		}
		if (it.kind == WARN_KIND_CONFIG_MISMATCH) {
			return ConfigMonitor_IsCrcFaultLatched(slot) ? 1u : 0u;
		}
	}
	if (it.kind == WARN_KIND_DEVICE_FOUND) {
		uint8_t found_n = ConfigMonitor_GetFoundLatchedCount();
		for (uint8_t fi = 0u; fi < found_n; fi++) {
			Device mcu = {};
			uint8_t v_l_adr = 0u;
			uint8_t v_d_type = 0u;
			if (!ConfigMonitor_GetFoundLatchedKey(fi, &mcu, &v_l_adr, &v_d_type)) {
				continue;
			}
			if (it.zone == mcu.zone && it.h_adr == mcu.h_adr && it.mcu_d_type == mcu.d_type &&
			    it.v_l_adr == v_l_adr && it.v_d_type == v_d_type) {
				return 1u;
			}
		}
	}
	return 0u;
}

static void ProcessPendingConfirmations(uint32_t now_ms)
{
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used || g_items[i].fault_now != 0u || g_items[i].confirm_pending == 0u) {
			continue;
		}
		uint8_t still = IsConfigFaultKind(g_items[i].kind) ?
				IsConfigItemStillActive(g_items[i]) :
				IsItemStillFaulty(g_items[i]);
		if (still == 0u) {
			ClearItemAt(i);
			continue;
		}
		if (ConfirmElapsed(g_items[i], now_ms)) {
			(void)PromoteToActiveFault(i, now_ms);
		}
	}
}

static void UpdateDebouncedPowerFaults(uint32_t now_ms)
{
	uint8_t raw[4];
	raw[0] = (uint8_t)(g_raw_power_fault_mask & 0x01u);
	raw[1] = (uint8_t)((g_raw_power_fault_mask >> 1) & 0x01u);
	raw[2] = (uint8_t)(g_raw_ppku_input_fault_mask & 0x01u);
	raw[3] = (uint8_t)((g_raw_ppku_input_fault_mask >> 1) & 0x01u);

	uint8_t out_power = 0u;
	uint8_t out_input = 0u;

	for (uint8_t ch = 0u; ch < 4u; ch++) {
		uint8_t confirmed = (ch < 2u) ?
				    (uint8_t)((g_power_fault_mask >> ch) & 0x01u) :
				    (uint8_t)((g_ppku_input_fault_mask >> (ch - 2u)) & 0x01u);
		if (raw[ch] != 0u) {
			if (confirmed != 0u) {
				if (ch < 2u) {
					out_power |= (uint8_t)(1u << ch);
				} else {
					out_input |= (uint8_t)(1u << (ch - 2u));
				}
				g_power_confirm_pending[ch] = 0u;
				continue;
			}
			if (g_power_confirm_pending[ch] == 0u) {
				g_power_confirm_pending[ch] = 1u;
				g_power_confirm_since[ch] = now_ms;
			} else if (TimeReached(now_ms, g_power_confirm_since[ch] + WARN_CONFIRM_MS)) {
				if (ch < 2u) {
					out_power |= (uint8_t)(1u << ch);
				} else {
					out_input |= (uint8_t)(1u << (ch - 2u));
				}
			}
		} else {
			g_power_confirm_pending[ch] = 0u;
		}
	}

	g_power_fault_mask = out_power;
	g_ppku_input_fault_mask = out_input;
}

static uint8_t IsItemStillFaulty(const WarningItem& it)
{
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo* m = &g_active_devices[mi];
		if (!m->online) {
			continue;
		}
		if (m->dev.zone != it.zone || m->dev.h_adr != it.h_adr || m->dev.d_type != it.mcu_d_type) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			auto* v = &m->vdevs[vi];
			if (!v->online) {
				continue;
			}
			if (it.kind == WARN_KIND_VDEV_FAULT && v->v_l_adr == it.v_l_adr && v->v_d_type == it.v_d_type) {
				return (IsTrackedVdevType(v->v_d_type) &&
					IsFaultLineStateForVdev(v->v_d_type, v->line_state)) ? 1u : 0u;
			}
			if (it.kind == WARN_KIND_LSWITCH_OPEN_ATTN && v->v_l_adr == it.v_l_adr &&
			    v->v_d_type == DEVICE_LSWITCH_TYPE) {
				return (v->line_state == 4u) ? 1u : 0u;
			}
			if (it.kind == WARN_KIND_DPT_WARNING_ATTN && v->v_l_adr == it.v_l_adr &&
			    v->v_d_type == DEVICE_DPT_TYPE) {
				return (v->status_cmd == (uint8_t)DeviceDPTStatus_Warning) ? 1u : 0u;
			}
		}
	}
	if (it.kind == WARN_KIND_MCU_CAN_FAULT) {
		for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
			ActiveDeviceInfo* m = &g_active_devices[mi];
			if (!m->online) {
				continue;
			}
			if (m->dev.zone == it.zone && m->dev.h_adr == it.h_adr && m->dev.d_type == it.mcu_d_type) {
				if (!m->can_status_valid) {
					return 0u;
				}
				uint8_t shift = (uint8_t)((it.can_idx - 1u) * 2u);
				uint8_t can_state = (uint8_t)((m->can_state_mask >> shift) & 0x3u);
				return (can_state == 1u || can_state == 2u) ? 1u : 0u;
			}
		}
	}
	if (it.kind == WARN_KIND_PPKU_CAN_FAULT) {
		return ((can_bus_error_flags & (1u << (it.can_idx - 1u))) != 0u) ? 1u : 0u;
	}
	if (it.kind == WARN_KIND_MCU_POSITION_FAULT) {
		uint8_t ha = it.h_adr;
		if (ha == 0u || ha > 32u) {
			return 0u;
		}
		return ((g_position_fault_mask & (1u << (ha - 1u))) != 0u) ? 1u : 0u;
	}
	if (it.kind == WARN_KIND_DEVICE_MISSING ||
	    it.kind == WARN_KIND_DEVICE_FOUND ||
	    it.kind == WARN_KIND_CONFIG_MISMATCH) {
		return IsConfigItemStillActive(it);
	}
	return 0u;
}

/* Обрабатывает только vdev со status_changed и обновляет список предупреждений. */
static void ConsumeChangedStatuses(uint32_t now_ms)
{
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo* m = &g_active_devices[mi];
		if (!m->online) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			auto* v = &m->vdevs[vi];
			if (!v->online) {
				continue;
			}
			if (!v->status_changed) {
				continue;
			}
			if (!IsTrackedVdevType(v->v_d_type)) {
				v->status_changed = 0u;
				continue;
			}

			if (IsFaultLineStateForVdev(v->v_d_type, v->line_state)) {
				UpsertItem(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type,
					   v->line_state, 0u, 0u, now_ms);
			} else {
				MarkRecovered(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type, 0u, now_ms);
			}

			if (AttentionEventsEnabled() != 0u) {
				if (v->v_d_type == DEVICE_LSWITCH_TYPE) {
					if (v->line_state == 4u) {
						UpsertItem(WARN_KIND_LSWITCH_OPEN_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							   v->v_d_type, v->line_state, 0u, 0u, now_ms);
					} else {
						MarkRecovered(WARN_KIND_LSWITCH_OPEN_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							      v->v_d_type, 0u, now_ms);
					}
				}
				if (v->v_d_type == DEVICE_DPT_TYPE) {
					if (v->status_cmd == (uint8_t)DeviceDPTStatus_Warning) {
						UpsertItem(WARN_KIND_DPT_WARNING_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							   v->v_d_type, 0u, 0u, v->max_temp_c, now_ms);
					} else {
						MarkRecovered(WARN_KIND_DPT_WARNING_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							      v->v_d_type, 0u, now_ms);
					}
				}
			}
			v->status_changed = 0u;
		}
	}
}

/* Снимает/удаляет устаревшие записи, управляет 10-секундным хвостом показа. */
static void PruneInactiveItems(uint32_t now_ms)
{
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used) {
			continue;
		}
		/* ГОСТ: ВНИМАНИЕ не ведём — сразу вычищаем старые attention-записи. */
		if (AttentionEventsEnabled() == 0u && IsAttentionKind(g_items[i].kind)) {
			RemoveItemAt(i);
			continue;
		}

		if (IsItemStillFaulty(g_items[i])) {
			if (g_items[i].fault_now == 0u) {
				BeginConfirm(g_items[i], now_ms);
				if (ConfirmElapsed(g_items[i], now_ms)) {
					(void)PromoteToActiveFault(i, now_ms);
				}
			}
			if (g_items[i].fault_now != 0u) {
				g_items[i].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
			}
			continue;
		}

		if (g_items[i].fault_now) {
			if (IsFaultKind(g_items[i].kind) && !IsConfigFaultKind(g_items[i].kind)) {
				EventLog_PostDeviceFaultItem(g_items[i], 1u);
			}
			g_items[i].fault_now = 0u;
			if (TimeReached(now_ms, g_items[i].show_until_ms)) {
				g_items[i].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
			}
		}

		if (TimeReached(now_ms, g_items[i].show_until_ms)) {
			RemoveItemAt(i);
		}
	}
}

/* Подхватывает "старые" активные неисправности без status_changed на старте. */
static void SyncMissingFaultItems(uint32_t now_ms)
{
	/* Подстраховка: если устройство уже пришло в "плохом" состоянии как первое сообщение
	 * (status_changed == 0), всё равно добавляем его в список предупреждений. */
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo* m = &g_active_devices[mi];
		if (!m->online) {
			continue;
		}
		for (uint8_t vi = 0u; vi < m->vdev_count; vi++) {
			auto* v = &m->vdevs[vi];
			if (!v->online || !IsTrackedVdevType(v->v_d_type)) {
				continue;
			}
			if (!IsFaultLineStateForVdev(v->v_d_type, v->line_state)) {
				MarkRecovered(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type, 0u, now_ms);
			} else {
				UpsertItem(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type,
					   v->line_state, 0u, 0u, now_ms);
			}
			if (AttentionEventsEnabled() != 0u) {
				if (v->v_d_type == DEVICE_LSWITCH_TYPE) {
					if (v->line_state == 4u) {
						UpsertItem(WARN_KIND_LSWITCH_OPEN_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							   v->v_d_type, v->line_state, 0u, 0u, now_ms);
					} else {
						MarkRecovered(WARN_KIND_LSWITCH_OPEN_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							      v->v_d_type, 0u, now_ms);
					}
				}
				if (v->v_d_type == DEVICE_DPT_TYPE) {
					if (v->status_cmd == (uint8_t)DeviceDPTStatus_Warning) {
						UpsertItem(WARN_KIND_DPT_WARNING_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							   v->v_d_type, 0u, 0u, v->max_temp_c, now_ms);
					} else {
						MarkRecovered(WARN_KIND_DPT_WARNING_ATTN, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type,
							      v->v_d_type, 0u, now_ms);
					}
				}
			}
		}
	}
}

/* Добавляет/снимает предупреждения о КЗ/обрыве CAN у МКУ по can_state_mask. */
static void SyncMkuCanFaultItems(uint32_t now_ms)
{
	for (uint8_t mi = 0u; mi < g_active_devices_count; mi++) {
		ActiveDeviceInfo* m = &g_active_devices[mi];
		if (!m->online || !m->can_status_valid) {
			continue;
		}
		for (uint8_t can_idx = 1u; can_idx <= 2u; can_idx++) {
			uint8_t shift = (uint8_t)((can_idx - 1u) * 2u);
			uint8_t can_state = (uint8_t)((m->can_state_mask >> shift) & 0x3u);
			if (can_state == 1u || can_state == 2u) {
				/* В WarningItem line_state: 1=обрыв, 2=КЗ. В heartbeat: 1=КЗ, 2=обрыв. */
				uint8_t line_state = (can_state == 1u) ? 2u : 1u;
				UpsertItem(WARN_KIND_MCU_CAN_FAULT, m->dev.zone, m->dev.h_adr, 0u, m->dev.d_type, 0u, line_state, can_idx, 0u, now_ms);
			} else {
				/* Для CAN-линий МКУ снимаем предупреждение сразу при восстановлении. */
				int idx = FindItem(WARN_KIND_MCU_CAN_FAULT, m->dev.zone, m->dev.h_adr, 0u, m->dev.d_type, 0u, can_idx);
				if (idx >= 0) {
					RemoveItemAt((uint8_t)idx);
				}
			}
		}
	}
}

/* Добавляет/снимает предупреждения об отсутствии активности CAN у ППКУ. */
static void SyncPpkuCanFaultItems(uint32_t now_ms)
{
	(void)now_ms;
	for (uint8_t can_idx = 1u; can_idx <= 2u; can_idx++) {
		uint8_t bit = (uint8_t)(1u << (can_idx - 1u));
		if ((can_bus_error_flags & bit) != 0u) {
			UpsertItem(WARN_KIND_PPKU_CAN_FAULT, 0u, 0u, 0u, DEVICE_PPKY_TYPE, 0u, 1u, can_idx, 0u, now_ms);
		} else {
			/* Для локальной линии ППКУ снимаем предупреждение сразу при восстановлении. */
			int idx = FindItem(WARN_KIND_PPKU_CAN_FAULT, 0u, 0u, 0u, DEVICE_PPKY_TYPE, 0u, can_idx);
			if (idx >= 0) {
				RemoveItemAt((uint8_t)idx);
			}
		}
	}
}

static uint8_t UpsertConfigUiItem(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
				  uint8_t mcu_d_type, uint8_t v_d_type, uint32_t now_ms)
{
	return UpsertItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, 0u, 0u, 0u, now_ms);
}

static void MarkConfigUiRecovered(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
				  uint8_t mcu_d_type, uint8_t v_d_type, uint32_t now_ms)
{
	int idx = FindItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, 0u);
	if (idx >= 0) {
		g_items[(uint8_t)idx].fault_now = 0u;
		g_items[(uint8_t)idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
	}
}

static void SyncMkuPositionFaultItems(uint32_t now_ms)
{
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used || g_items[i].kind != WARN_KIND_MCU_POSITION_FAULT) {
			continue;
		}
		uint8_t ha = g_items[i].h_adr;
		if (ha == 0u || ha > 32u || ((g_position_fault_mask & (1u << (ha - 1u))) == 0u)) {
			RemoveItemAt(i);
		}
	}

	for (uint8_t ha = 1u; ha <= 32u; ha++) {
		if ((g_position_fault_mask & (1u << (ha - 1u))) == 0u) {
			continue;
		}
		UpsertItem(WARN_KIND_MCU_POSITION_FAULT, 0u, ha, 0u,
			   DEVICE_PPKY_TYPE, 0u, 0u, 0u, 0u, now_ms);
	}
}

static void SyncConfigMonitorItems(uint32_t now_ms)
{
	for (uint8_t slot = 0u; slot < 32u; slot++) {
		const Device *dev = ConfigMonitor_GetCfgDevice(slot);
		if (dev == nullptr || dev->d_type == 0u) {
			continue;
		}
		if (ConfigMonitor_IsMcuMissingLatched(slot)) {
			UpsertConfigUiItem(WARN_KIND_DEVICE_MISSING, dev->zone, dev->h_adr, 0u,
					   dev->d_type, 0u, now_ms);
		} else if (FindItem(WARN_KIND_DEVICE_MISSING, dev->zone, dev->h_adr, 0u,
				    dev->d_type, 0u, 0u) >= 0) {
			MarkConfigUiRecovered(WARN_KIND_DEVICE_MISSING, dev->zone, dev->h_adr, 0u,
					      dev->d_type, 0u, now_ms);
		}
		if (ConfigMonitor_IsCrcFaultLatched(slot)) {
			UpsertConfigUiItem(WARN_KIND_CONFIG_MISMATCH, dev->zone, dev->h_adr, 0u,
					   dev->d_type, 0u, now_ms);
		} else if (FindItem(WARN_KIND_CONFIG_MISMATCH, dev->zone, dev->h_adr, 0u,
				    dev->d_type, 0u, 0u) >= 0) {
			MarkConfigUiRecovered(WARN_KIND_CONFIG_MISMATCH, dev->zone, dev->h_adr, 0u,
					      dev->d_type, 0u, now_ms);
		}
	}

	uint8_t found_n = ConfigMonitor_GetFoundLatchedCount();
	for (uint8_t fi = 0u; fi < found_n; fi++) {
		Device mcu = {};
		uint8_t v_l_adr = 0u;
		uint8_t v_d_type = 0u;
		if (!ConfigMonitor_GetFoundLatchedKey(fi, &mcu, &v_l_adr, &v_d_type)) {
			continue;
		}
		UpsertConfigUiItem(WARN_KIND_DEVICE_FOUND, mcu.zone, mcu.h_adr, v_l_adr,
				   mcu.d_type, v_d_type, now_ms);
	}
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used || g_items[i].kind != WARN_KIND_DEVICE_FOUND || !g_items[i].fault_now) {
			continue;
		}
		uint8_t still = 0u;
		for (uint8_t fi = 0u; fi < found_n; fi++) {
			Device mcu = {};
			uint8_t v_l_adr = 0u;
			uint8_t v_d_type = 0u;
			if (!ConfigMonitor_GetFoundLatchedKey(fi, &mcu, &v_l_adr, &v_d_type)) {
				continue;
			}
			if (g_items[i].zone == mcu.zone &&
			    g_items[i].h_adr == mcu.h_adr &&
			    g_items[i].mcu_d_type == mcu.d_type &&
			    g_items[i].v_l_adr == v_l_adr &&
			    g_items[i].v_d_type == v_d_type) {
				still = 1u;
				break;
			}
		}
		if (!still) {
			MarkConfigUiRecovered(WARN_KIND_DEVICE_FOUND, g_items[i].zone, g_items[i].h_adr,
					      g_items[i].v_l_adr, g_items[i].mcu_d_type, g_items[i].v_d_type,
					      now_ms);
		}
	}
}

/* Формирует отсортированный набор строк для UI (большое/малое поле). */
static uint8_t BuildUiPayload(char (*big_titles)[WARN_TITLE_LEN], char (*details)[ZONE_NAME_SIZE + 1])
{
	uint8_t count = 0u;
	uint8_t order[WARN_MAX_ITEMS];
	uint8_t on = 0u;
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (!g_items[i].used) {
			continue;
		}
		if (!g_items[i].fault_now) {
			continue;
		}
		order[on++] = i;
	}
	/* Новее сверху: АВАРИЯ 1 = последнее событие. */
	for (uint8_t a = 1u; a < on; a++) {
		uint8_t key = order[a];
		uint8_t b = a;
		while (b > 0u &&
		       g_items[order[b - 1u]].appeared_ms < g_items[key].appeared_ms) {
			order[b] = order[b - 1u];
			b--;
		}
		order[b] = key;
	}

	/* Сначала ВНИМАНИЕ, затем НЕИСПРАВНОСТИ (без перемешивания). */
	if (AttentionEventsEnabled() != 0u) {
		for (uint8_t i = 0u; i < on && count < WARN_MAX_ITEMS; i++) {
			const WarningItem& it = g_items[order[i]];
			if (!IsAttentionKind(it.kind)) {
				continue;
			}
			if (it.kind == WARN_KIND_LSWITCH_OPEN_ATTN) {
				snprintf(big_titles[count], WARN_TITLE_LEN, "%cОТКРЫТИЕ", (char)WARN_TITLE_MARK_ATTN);
				Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
			} else if (it.kind == WARN_KIND_DPT_WARNING_ATTN) {
				int temp_c = (int)it.extra;
				snprintf(big_titles[count], WARN_TITLE_LEN, "%cТЕМП. %d", (char)WARN_TITLE_MARK_ATTN, temp_c);
				Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
			}
			count++;
		}
	}

	/* Далее системные неисправности питания ППКУ. */
	for (uint8_t ch = 0u; ch < 2u && count < WARN_MAX_ITEMS; ch++) {
		if ((g_power_fault_mask & (1u << ch)) == 0u) {
			continue;
		}
		snprintf(big_titles[count], WARN_TITLE_LEN, "ВЫХОД %u", (unsigned)(ch + 1u));
		snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N:%08lX:%08lX:%08lX", PPKYConfig.UId.UId0, PPKYConfig.UId.UId1, PPKYConfig.UId.UId2);
		count++;
	}
	for (uint8_t ch = 0u; ch < 2u && count < WARN_MAX_ITEMS; ch++) {
		if ((g_ppku_input_fault_mask & (1u << ch)) == 0u) {
			continue;
		}
		snprintf(big_titles[count], WARN_TITLE_LEN, "ПИТАНИЕ %u", (unsigned)(ch + 1u));
		snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N:%08lX:%08lX:%08lX", PPKYConfig.UId.UId0, PPKYConfig.UId.UId1, PPKYConfig.UId.UId2);
		count++;
	}

	/* Затем обычные неисправности устройств/CAN. */
	for (uint8_t i = 0u; i < on && count < WARN_MAX_ITEMS; i++) {
		const WarningItem& it = g_items[order[i]];
		if (!IsFaultKind(it.kind)) {
			continue;
		}

		if (it.kind == WARN_KIND_VDEV_FAULT) {
			const char* fault = "ОБРЫВ";
			if (it.line_state == 5u) {
				fault = "НЕИСП";
			} else if (it.line_state == 2u) {
				fault = "КЗ";
			}
			snprintf(big_titles[count], WARN_TITLE_LEN, "%s %s%u", fault,
				 Warning_ChannelTypeShort(it.v_d_type), (unsigned)it.v_l_adr);
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else if (it.kind == WARN_KIND_MCU_CAN_FAULT) {
			const char* fault = (it.line_state == 2u) ? "КЗ" : "ОБРЫВ";
			snprintf(big_titles[count], WARN_TITLE_LEN, "%s CAN%u", fault, (unsigned)it.can_idx);
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else if (it.kind == WARN_KIND_MCU_POSITION_FAULT) {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ПОЗИЦИЯ");
			snprintf(details[count], ZONE_NAME_SIZE + 1, "МКУ %u", (unsigned)it.h_adr);
		} else if (it.kind == WARN_KIND_DEVICE_MISSING) {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ОТСУСТВ.");
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else if (it.kind == WARN_KIND_DEVICE_FOUND) {
			snprintf(big_titles[count], WARN_TITLE_LEN, "НОВОЕ");
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else if (it.kind == WARN_KIND_CONFIG_MISMATCH) {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ОШ. КОНФ.");
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ОБРЫВ CAN%u", (unsigned)it.can_idx);

			//Warning_GetSerialPlaceholder(it, serial, sizeof(serial));
			snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N:%08lX:%08lX:%08lX", PPKYConfig.UId.UId0, PPKYConfig.UId.UId1, PPKYConfig.UId.UId2);
		}
		count++;
	}
	return count;
}

/* Есть ли сейчас хотя бы одна активная (не восстановленная) неисправность. */
static uint8_t HasActiveFaultNow(void)
{
	if (g_power_fault_mask != 0u || g_ppku_input_fault_mask != 0u) {
		return 1u;
	}
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (g_items[i].used && g_items[i].fault_now && IsFaultKind(g_items[i].kind)) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t CountActiveFaultNow(void)
{
	uint8_t count = 0u;
	if (g_power_fault_mask != 0u) {
		for (uint8_t i = 0u; i < 2u; i++) {
			if ((g_power_fault_mask & (1u << i)) != 0u) {
				count++;
			}
		}
	}
	if (g_ppku_input_fault_mask != 0u) {
		for (uint8_t i = 0u; i < 2u; i++) {
			if ((g_ppku_input_fault_mask & (1u << i)) != 0u) {
				count++;
			}
		}
	}
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (g_items[i].used && g_items[i].fault_now && IsFaultKind(g_items[i].kind) && count < 0xFFu) {
			count++;
		}
	}
	return count;
}

static uint8_t CountActiveAttentionNow(void)
{
	if (AttentionEventsEnabled() == 0u) {
		return 0u;
	}
	uint8_t count = 0u;
	for (uint8_t i = 0u; i < WARN_MAX_ITEMS; i++) {
		if (g_items[i].used && g_items[i].fault_now && IsAttentionKind(g_items[i].kind) && count < 0xFFu) {
			count++;
		}
	}
	return count;
}

static void UpdateFaultSound(uint32_t now_ms)
{
	uint8_t attention_count = CountActiveAttentionNow();
	uint8_t fault_count = CountActiveFaultNow();
	if (Fire_IsActive()) {
		g_fault_sound_phase = FAULT_SOUND_IDLE;
		g_fault_sound_deadline_ms = 0u;
		g_attention_sound_phase = ATTN_SOUND_IDLE;
		g_attention_sound_deadline_ms = 0u;
		g_prev_sound_fault_count = fault_count;
		g_prev_sound_attention_count = attention_count;
		return;
	}

	if (attention_count > 0u) {
		/* ВНИМАНИЕ имеет приоритет над НЕИСПРАВНОСТЬЮ. */
		if (g_fault_sound_phase == FAULT_SOUND_PERIODIC || g_fault_sound_phase == FAULT_SOUND_WAIT_PERIODIC) {
			Beeper_StopPattern();
			g_fault_sound_phase = FAULT_SOUND_IDLE;
			g_fault_sound_deadline_ms = 0u;
		}

		if (attention_count > g_prev_sound_attention_count) {
			Beeper_ResumeSoundOnNewEvent();
		}
		if (attention_count > g_prev_sound_attention_count || g_attention_sound_phase == ATTN_SOUND_IDLE) {
			Beeper_StopPattern();
			Beeper_StartPulseTrain(SOUND_ATTN_SIGNAL_ON_MS, SOUND_ATTN_SIGNAL_OFF_MS,
					       SOUND_ATTN_SIGNAL_PULSES, 0u);
			g_attention_sound_phase = ATTN_SOUND_WAIT_PERIODIC;
			g_attention_sound_deadline_ms =
				now_ms + ((SOUND_ATTN_SIGNAL_ON_MS + SOUND_ATTN_SIGNAL_OFF_MS) * (uint32_t)SOUND_ATTN_SIGNAL_PULSES);
		} else if (g_attention_sound_phase == ATTN_SOUND_WAIT_PERIODIC &&
			   TimeReached(now_ms, g_attention_sound_deadline_ms)) {
			Beeper_StartPulseTrain(SOUND_ATTN_DUTY_ON_MS, SOUND_ATTN_DUTY_OFF_MS,
					       SOUND_ATTN_DUTY_PULSES, SOUND_ATTN_DUTY_REPEAT_MS);
			g_attention_sound_phase = ATTN_SOUND_PERIODIC;
		}

		g_prev_sound_attention_count = attention_count;
		g_prev_sound_fault_count = fault_count;
		return;
	}

	if (fault_count == 0u) {
		if (g_fault_sound_phase == FAULT_SOUND_PERIODIC ||
		    g_fault_sound_phase == FAULT_SOUND_WAIT_PERIODIC) {
			Beeper_StopPattern();
		}
		if (g_attention_sound_phase == ATTN_SOUND_PERIODIC ||
		    g_attention_sound_phase == ATTN_SOUND_WAIT_PERIODIC) {
			Beeper_StopPattern();
		}
		g_fault_sound_phase = FAULT_SOUND_IDLE;
		g_fault_sound_deadline_ms = 0u;
		g_attention_sound_phase = ATTN_SOUND_IDLE;
		g_attention_sound_deadline_ms = 0u;
		g_prev_sound_fault_count = fault_count;
		g_prev_sound_attention_count = attention_count;
		return;
	}

	/* Любая новая неисправность должна заново выдать 1.3с сигнальный звук. */
	if (fault_count > g_prev_sound_fault_count) {
		Beeper_ResumeSoundOnNewEvent();
	}
	if (fault_count > g_prev_sound_fault_count || g_fault_sound_phase == FAULT_SOUND_IDLE) {
		Beeper_StopPattern();
		Beeper_StartPulseTrain(SOUND_FAULT_SIGNAL_ON_MS, SOUND_FAULT_SIGNAL_OFF_MS,
				       SOUND_FAULT_SIGNAL_PULSES, 0u);
		g_fault_sound_phase = FAULT_SOUND_WAIT_PERIODIC;
		g_fault_sound_deadline_ms =
			now_ms + ((SOUND_FAULT_SIGNAL_ON_MS + SOUND_FAULT_SIGNAL_OFF_MS) * (uint32_t)SOUND_FAULT_SIGNAL_PULSES);
		g_prev_sound_fault_count = fault_count;
		g_prev_sound_attention_count = attention_count;
		return;
	}
	g_prev_sound_fault_count = fault_count;
	g_prev_sound_attention_count = attention_count;
	if (g_fault_sound_phase == FAULT_SOUND_WAIT_PERIODIC &&
	    TimeReached(now_ms, g_fault_sound_deadline_ms)) {
		Beeper_StartPulseTrain(SOUND_FAULT_DUTY_ON_MS, SOUND_FAULT_DUTY_OFF_MS,
				       SOUND_FAULT_DUTY_PULSES, SOUND_FAULT_DUTY_REPEAT_MS);
		g_fault_sound_phase = FAULT_SOUND_PERIODIC;
	}
}

/* Управляет LED_ERR: только неисправность (непрерывно). ВНИМАНИЕ — на LED_FIRE. */
static void UpdateErrorLed(uint32_t now_ms)
{
	uint8_t fault_count = CountActiveFaultNow();
	uint8_t attention_count = CountActiveAttentionNow();
	uint8_t active_count = (uint8_t)(fault_count + attention_count);

	if (active_count > g_prev_active_fault_count && fault_count > 0u) {
		Led_ForceStatusBright(LED_ERR);
	}
	g_prev_active_fault_count = active_count;
	(void)now_ms;

	if (fault_count > 0u || Fire_HasExtinguishIncomplete()) {
		Led_Set(LED_ERR, 1u);
		g_led_err_on = 1u;
		return;
	}

	if (g_led_err_on) {
		Led_Set(LED_ERR, 0u);
		g_led_err_on = 0u;
	}
}

/* Пушит данные в TouchGFX только при реальном изменении (анти-спам). */
static void PushUiIfChanged(uint8_t active, uint8_t count,
			    char (*big_titles)[WARN_TITLE_LEN], char (*details)[ZONE_NAME_SIZE + 1])
{
	uint8_t same = (g_last_active == active && g_last_count == count) ? 1u : 0u;
	if (same && active) {
		if (memcmp(g_last_big, big_titles, sizeof(g_last_big)) != 0 ||
		    memcmp(g_last_details, details, sizeof(g_last_details)) != 0) {
			same = 0u;
		}
	}
	if (same) {
		return;
	}

	g_last_active = active;
	g_last_count = count;
	memset(g_last_big, 0, sizeof(g_last_big));
	memset(g_last_details, 0, sizeof(g_last_details));
	if (active) {
		memcpy(g_last_big, big_titles, sizeof(g_last_big));
		memcpy(g_last_details, details, sizeof(g_last_details));
	}
	Warning_UiUpdate(active, count, big_titles, details);
}

} // namespace

/* Главный 1мс-тик модуля: сбор, фильтрация, LED и публикация предупреждений. */
extern "C" void WarningProcess1ms(void)
{
	if (MenuUi_IsConfigSessionActive()) {
		return;
	}

	uint32_t now_ms = HAL_GetTick();
	char big_titles[WARN_MAX_ITEMS][WARN_TITLE_LEN] = {{0}};
	char details[WARN_MAX_ITEMS][ZONE_NAME_SIZE + 1] = {{0}};

	ConsumeChangedStatuses(now_ms);
	SyncMissingFaultItems(now_ms);
	SyncMkuCanFaultItems(now_ms);
	SyncPpkuCanFaultItems(now_ms);
	SyncMkuPositionFaultItems(now_ms);
	SyncConfigMonitorItems(now_ms);
	UpdateDebouncedPowerFaults(now_ms);
	ProcessPendingConfirmations(now_ms);
	SyncPowerFaultEventLog();
	PruneInactiveItems(now_ms);
	UpdateErrorLed(now_ms);
	UpdateFaultSound(now_ms);

	/* Список поддерживаем всегда (в т.ч. во время пожара) — приоритет показа на UI. */
	uint8_t count = BuildUiPayload(big_titles, details);
	PushUiIfChanged((count > 0u) ? 1u : 0u, count, big_titles, details);
}

extern "C" void Warning_SetPowerFaultMask(uint8_t mask)
{
	g_raw_power_fault_mask = (uint8_t)(mask & 0x03u);
}

extern "C" void Warning_SetPpkuInputFaultMask(uint8_t mask)
{
	g_raw_ppku_input_fault_mask = (uint8_t)(mask & 0x03u);
}

extern "C" void Warning_SetMkuPositionFaultMask(uint32_t mask)
{
	g_position_fault_mask = mask & 0xFFFFFFFFu;
}

extern "C" uint8_t Warning_HasActiveFault(void)
{
	return (HasActiveFaultNow() || Fire_HasExtinguishIncomplete()) ? 1u : 0u;
}

extern "C" uint8_t Warning_HasActiveAttention(void)
{
	return CountActiveAttentionNow() > 0u ? 1u : 0u;
}
