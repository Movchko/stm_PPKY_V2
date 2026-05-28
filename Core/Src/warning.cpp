#include "warning.h"

#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "fire.h"
#include "led.h"
#include "beeper.h"
#include "can_bus.h"
#include "device_config.h"
#include "device_dpt.hpp"
#include "sound_profiles.h"

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
static FaultSoundPhase g_fault_sound_phase = FAULT_SOUND_IDLE;
static uint32_t g_fault_sound_deadline_ms = 0u;
static AttentionSoundPhase g_attention_sound_phase = ATTN_SOUND_IDLE;
static uint32_t g_attention_sound_deadline_ms = 0u;

extern "C" uint8_t Warning_HasActiveFault(void);

namespace {

constexpr uint8_t WARN_MAX_ITEMS = 16u;
constexpr uint32_t WARNING_SHOW_HOLD_MS = 10000u; /* Показывать предупреждение ещё 10 с после исчезновения */
constexpr uint8_t WARN_KIND_VDEV_FAULT = 0u;
constexpr uint8_t WARN_KIND_MCU_CAN_FAULT = 1u;
constexpr uint8_t WARN_KIND_PPKU_CAN_FAULT = 2u;
constexpr uint8_t WARN_KIND_LSWITCH_OPEN_ATTN = 3u;
constexpr uint8_t WARN_KIND_DPT_WARNING_ATTN = 4u;
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
	uint32_t show_until_ms;
};

static WarningItem g_items[WARN_MAX_ITEMS];

static uint8_t g_last_active = 0xFFu;
static uint8_t g_last_count = 0xFFu;
static char g_last_big[WARN_MAX_ITEMS][WARN_TITLE_LEN];
static char g_last_details[WARN_MAX_ITEMS][ZONE_NAME_SIZE + 1];
static uint8_t g_led_err_on = 0u;
static uint8_t g_led_err_blink_phase = 0u;
static uint32_t g_led_err_blink_toggle_ms = 0u;
static uint8_t g_prev_active_fault_count = 0u;
static uint8_t g_prev_sound_fault_count = 0u;
static uint8_t g_prev_sound_attention_count = 0u;

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
		out[ZONE_NAME_SIZE] = '\0';
	}
	if (out[0] == '\0') {
		snprintf(out, out_sz, "ЗОНА %u", (unsigned)it.zone);
	}
}

/* Заглушка под будущий реальный S/N: *ТИП**H_ADR**2026* */
static void Warning_GetSerialPlaceholder(const WarningItem& it, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "S/N:%s%u2026",
		 Warning_McuTypeSerialToken(it.mcu_d_type), (unsigned)it.h_adr);
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

/* Проверяет, что состояние линии относится к неисправности (обрыв/КЗ). */
static uint8_t IsFaultLineState(uint8_t line_state)
{
	return (line_state == 1u || line_state == 2u) ? 1u : 0u; /* 1=Обрыв, 2=КЗ */
}

static uint8_t IsAttentionKind(uint8_t kind)
{
	return (kind == WARN_KIND_LSWITCH_OPEN_ATTN || kind == WARN_KIND_DPT_WARNING_ATTN) ? 1u : 0u;
}

static uint8_t IsFaultKind(uint8_t kind)
{
	return (kind == WARN_KIND_VDEV_FAULT ||
		kind == WARN_KIND_MCU_CAN_FAULT ||
		kind == WARN_KIND_PPKU_CAN_FAULT) ? 1u : 0u;
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

/* Удаляет запись неисправности по индексу. */
static void RemoveItemAt(uint8_t idx)
{
	if (idx < WARN_MAX_ITEMS) {
		memset(&g_items[idx], 0, sizeof(g_items[idx]));
	}
}

/* Безопасное сравнение таймера с учётом переполнения HAL_GetTick(). */
static uint8_t TimeReached(uint32_t now_ms, uint32_t deadline_ms)
{
	return ((int32_t)(now_ms - deadline_ms) >= 0) ? 1u : 0u;
}

/* Добавляет/обновляет запись неисправности и продлевает окно отображения. */
static void UpsertItem(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
		       uint8_t mcu_d_type, uint8_t v_d_type, uint8_t line_state,
		       uint8_t can_idx, int16_t extra, uint32_t now_ms)
{
	int idx = FindItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, can_idx);
	if (idx >= 0) {
		g_items[(uint8_t)idx].line_state = line_state;
		g_items[(uint8_t)idx].can_idx = can_idx;
		g_items[(uint8_t)idx].extra = extra;
		g_items[(uint8_t)idx].fault_now = 1u;
		g_items[(uint8_t)idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
		return;
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
		g_items[i].fault_now = 1u;
		g_items[i].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
		return;
	}
}

/* Помечает неисправность как восстановленную (с хвостом отображения). */
static void MarkRecovered(uint8_t kind, uint8_t zone, uint8_t h_adr, uint8_t v_l_adr,
			  uint8_t mcu_d_type, uint8_t v_d_type, uint8_t can_idx, uint32_t now_ms)
{
	int idx = FindItem(kind, zone, h_adr, v_l_adr, mcu_d_type, v_d_type, can_idx);
	if (idx >= 0) {
		g_items[(uint8_t)idx].fault_now = 0u;
		g_items[(uint8_t)idx].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
	}
}

/* Проверяет, что запись всё ещё реально неисправна по текущим данным шины. */
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
				return (IsTrackedVdevType(v->v_d_type) && IsFaultLineState(v->line_state)) ? 1u : 0u;
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

			if (IsFaultLineState(v->line_state)) {
				UpsertItem(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type,
					   v->line_state, 0u, 0u, now_ms);
			} else {
				MarkRecovered(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type, 0u, now_ms);
			}

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

		if (IsItemStillFaulty(g_items[i])) {
			g_items[i].fault_now = 1u;
			g_items[i].show_until_ms = now_ms + WARNING_SHOW_HOLD_MS;
			continue;
		}

		if (g_items[i].fault_now) {
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
			if (!IsFaultLineState(v->line_state)) {
				MarkRecovered(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type, 0u, now_ms);
			} else {
				UpsertItem(WARN_KIND_VDEV_FAULT, m->dev.zone, m->dev.h_adr, v->v_l_adr, m->dev.d_type, v->v_d_type,
					   v->line_state, 0u, 0u, now_ms);
			}
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
	/* Стабильный порядок для UI: zone -> h_adr -> l_adr -> type.
	 * Это убирает "скакание" строк при нескольких неисправностях. */
	for (uint8_t a = 1u; a < on; a++) {
		uint8_t key = order[a];
		uint8_t b = a;
		while (b > 0u) {
			const WarningItem& l = g_items[order[b - 1u]];
			const WarningItem& r = g_items[key];
			uint8_t greater = 0u;
			if (l.zone > r.zone) greater = 1u;
			else if (l.zone == r.zone && l.h_adr > r.h_adr) greater = 1u;
			else if (l.zone == r.zone && l.h_adr == r.h_adr && l.v_l_adr > r.v_l_adr) greater = 1u;
			else if (l.zone == r.zone && l.h_adr == r.h_adr && l.v_l_adr == r.v_l_adr && l.kind > r.kind) greater = 1u;
			else if (l.zone == r.zone && l.h_adr == r.h_adr && l.v_l_adr == r.v_l_adr && l.kind == r.kind && l.can_idx > r.can_idx) greater = 1u;
			else if (l.zone == r.zone && l.h_adr == r.h_adr && l.v_l_adr == r.v_l_adr && l.kind == r.kind && l.can_idx == r.can_idx && l.v_d_type > r.v_d_type) greater = 1u;
			if (!greater) {
				break;
			}
			order[b] = order[b - 1u];
			b--;
		}
		order[b] = key;
	}

	/* Сначала ВНИМАНИЕ, затем НЕИСПРАВНОСТИ (без перемешивания). */
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

	/* Далее системные неисправности питания ППКУ. */
	for (uint8_t ch = 0u; ch < 3u && count < WARN_MAX_ITEMS; ch++) {
		if ((g_power_fault_mask & (1u << ch)) == 0u) {
			continue;
		}
		if (ch == 2u) {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ПАНЕЛЬ");
		} else {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ВЫХОД %u", (unsigned)(ch + 1u));
		}
		snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N 123456789");
		count++;
	}
	for (uint8_t ch = 0u; ch < 2u && count < WARN_MAX_ITEMS; ch++) {
		if ((g_ppku_input_fault_mask & (1u << ch)) == 0u) {
			continue;
		}
		snprintf(big_titles[count], WARN_TITLE_LEN, "ПИТАНИЕ %u", (unsigned)(ch + 1u));
		snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N 123456789");
		count++;
	}

	/* Затем обычные неисправности устройств/CAN. */
	for (uint8_t i = 0u; i < on && count < WARN_MAX_ITEMS; i++) {
		const WarningItem& it = g_items[order[i]];
		if (!IsFaultKind(it.kind)) {
			continue;
		}

		if (it.kind == WARN_KIND_VDEV_FAULT) {
			const char* fault = (it.line_state == 2u) ? "КЗ" : "ОБРЫВ";
			snprintf(big_titles[count], WARN_TITLE_LEN, "%s %s%u", fault,
				 Warning_ChannelTypeShort(it.v_d_type), (unsigned)it.v_l_adr);
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else if (it.kind == WARN_KIND_MCU_CAN_FAULT) {
			const char* fault = (it.line_state == 2u) ? "КЗ" : "ОБРЫВ";
			snprintf(big_titles[count], WARN_TITLE_LEN, "%s CAN%u", fault, (unsigned)it.can_idx);
			Warning_FormatMkuAndSerial(details[count], ZONE_NAME_SIZE + 1, it);
		} else {
			snprintf(big_titles[count], WARN_TITLE_LEN, "ОБРЫВ CAN%u", (unsigned)it.can_idx);

			//Warning_GetSerialPlaceholder(it, serial, sizeof(serial));
			snprintf(details[count], ZONE_NAME_SIZE + 1, "ППКУ S/N 123456789");
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
		for (uint8_t i = 0u; i < 3u; i++) {
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

/* Управляет LED_ERR: мгновенное включение и мгновенное отключение. */
static void UpdateErrorLed(uint32_t now_ms)
{
	uint8_t fault_count = CountActiveFaultNow();
	uint8_t attention_count = CountActiveAttentionNow();
	if ((fault_count + attention_count) > g_prev_active_fault_count) {
		Led_ForceStatusBright(LED_ERR);
	}
	g_prev_active_fault_count = (uint8_t)(fault_count + attention_count);

	if (attention_count > 0u) {
		/* При наличии ВНИМАНИЯ индикатор НЕИСПР. должен мигать (0.5с). */
		if ((now_ms - g_led_err_blink_toggle_ms) >= 500u) {
			g_led_err_blink_toggle_ms = now_ms;
			g_led_err_blink_phase = (uint8_t)!g_led_err_blink_phase;
		}
		Led_Set(LED_ERR, g_led_err_blink_phase);
		g_led_err_on = g_led_err_blink_phase;
		return;
	}

	if (fault_count > 0u) {
		Led_Set(LED_ERR, 1u);
		g_led_err_on = 1u;
		return;
	}

	if (g_led_err_on) {
		Led_Set(LED_ERR, 0u);
		g_led_err_on = 0u;
	}
	g_led_err_blink_phase = 0u;
	g_led_err_blink_toggle_ms = now_ms;
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
void WarningProcess1ms(void)
{
	uint32_t now_ms = HAL_GetTick();
	char big_titles[WARN_MAX_ITEMS][WARN_TITLE_LEN] = {{0}};
	char details[WARN_MAX_ITEMS][ZONE_NAME_SIZE + 1] = {{0}};

	ConsumeChangedStatuses(now_ms);
	SyncMissingFaultItems(now_ms);
	SyncMkuCanFaultItems(now_ms);
	SyncPpkuCanFaultItems(now_ms);
	PruneInactiveItems(now_ms);
	UpdateErrorLed(now_ms);
	UpdateFaultSound(now_ms);

	/* Во время пожара предупреждения не отображаем (но список поддерживаем актуальным). */
	if (Fire_IsActive()) {
		PushUiIfChanged(0u, 0u, big_titles, details);
		return;
	}

	uint8_t count = BuildUiPayload(big_titles, details);
	PushUiIfChanged((count > 0u) ? 1u : 0u, count, big_titles, details);
}

extern "C" void Warning_SetPowerFaultMask(uint8_t mask)
{
	g_power_fault_mask = (uint8_t)(mask & 0x07u);
}

extern "C" void Warning_SetPpkuInputFaultMask(uint8_t mask)
{
	g_ppku_input_fault_mask = (uint8_t)(mask & 0x03u);
}

extern "C" uint8_t Warning_HasActiveFault(void)
{
	return HasActiveFaultNow();
}
