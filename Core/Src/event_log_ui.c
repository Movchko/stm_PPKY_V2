/*
 * event_log_ui.c — форматирование записей журнала для передачи на панель.
 * Основано на логике из `stm_PPKY/Core/Src/event_log_ui.c`, но сведено
 * к используемому сейчас набору полей title/detail/header.
 */

#include "event_log_ui.h"

#include "backend.h"
#include "config_monitor.h"
#include "device_config.h"
#include "event_log_catalog.h"

#include <stdio.h>
#include <string.h>

extern PPKYCfg PPKYConfig;

enum {
	ELUI_FC_LINE_BREAK = 0u,
	ELUI_FC_LINE_SHORT = 1u,
	ELUI_FC_PROTOCOL   = 2u,
	ELUI_FC_CAN        = 3u,
	ELUI_FC_POWER      = 4u,
	ELUI_FC_OTHER      = 5u,
	ELUI_FC_POSITION   = 6u
};

static uint8_t BcdToBin(uint8_t bcd)
{
	return (uint8_t)(((bcd >> 4) & 0x0Fu) * 10u + (bcd & 0x0Fu));
}

static uint8_t IsMcuDType(uint8_t d_type)
{
	return (d_type == DEVICE_MCU_IGN_TYPE ||
	        d_type == DEVICE_MCU_TC_TYPE ||
	        d_type == DEVICE_MCU_K1 ||
	        d_type == DEVICE_MCU_K2 ||
	        d_type == DEVICE_MCU_K3 ||
	        d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

static uint8_t IsVdevDType(uint8_t d_type)
{
	return (d_type == DEVICE_DPT_TYPE ||
	        d_type == DEVICE_IGNITER_TYPE ||
	        d_type == DEVICE_BUTTON_TYPE ||
	        d_type == DEVICE_LSWITCH_TYPE) ? 1u : 0u;
}

static const char *ChannelTypeShort(uint8_t v_d_type)
{
	switch (v_d_type) {
	case DEVICE_DPT_TYPE:     return "ДПТ";
	case DEVICE_IGNITER_TYPE: return "СП";
	case DEVICE_BUTTON_TYPE:  return "КН";
	case DEVICE_LSWITCH_TYPE: return "КОН";
	default:                  return "???";
	}
}

static const char *McuTypeName(uint8_t d_type)
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

static void ParseCanHeader(uint32_t can_header,
			   uint8_t *zone, uint8_t *h_adr, uint8_t *l_adr, uint8_t *d_type)
{
	can_ext_id_t id;
	id.ID = can_header;
	*zone = (uint8_t)(id.field.zone & 0x7Fu);
	*h_adr = (uint8_t)id.field.h_adr;
	*l_adr = (uint8_t)(id.field.l_adr & 0x3Fu);
	*d_type = (uint8_t)(id.field.d_type & 0x7Fu);
}

static void GetZoneName(uint8_t zone, char *out, size_t out_sz)
{
	if (out == NULL || out_sz == 0u) {
		return;
	}
	out[0] = '\0';
	if (zone > 0u && zone <= ZONE_NUMBER) {
		strncpy(out, (const char *)PPKYConfig.zone_name[zone - 1u], ZONE_NAME_SIZE);
		out[ZONE_NAME_SIZE] = '\0';
	}
	if (out[0] == '\0') {
		snprintf(out, out_sz, "ЗОНА %u", (unsigned)zone);
	}
}

static void FormatPpkySerial(char *out, size_t out_sz)
{
	snprintf(out, out_sz, "S/N:%08lX:%08lX:%08lX",
		 (unsigned long)PPKYConfig.UId.UId0,
		 (unsigned long)PPKYConfig.UId.UId1,
		 (unsigned long)PPKYConfig.UId.UId2);
}

static void GetMkuSerial(uint8_t zone, uint8_t h_adr, uint8_t mcu_d_type,
			 char *out, size_t out_sz)
{
	uint8_t i;

	for (i = 0u; i < 32u; i++) {
		const Device *dv = &PPKYConfig.CfgDevices[i].UId.devId;
		if (dv->h_adr == h_adr && dv->d_type == mcu_d_type) {
			snprintf(out, out_sz, "S/N:%08lX:%08lX:%08lX",
				 (unsigned long)PPKYConfig.CfgDevices[i].UId.UId0,
				 (unsigned long)PPKYConfig.CfgDevices[i].UId.UId1,
				 (unsigned long)PPKYConfig.CfgDevices[i].UId.UId2);
			return;
		}
	}

	{
		Device dev;
		uint8_t remote_valid = 0u;
		const uint8_t *remote;

		memset(&dev, 0, sizeof(dev));
		dev.zone = zone;
		dev.h_adr = h_adr;
		dev.d_type = mcu_d_type;

		remote = ConfigMonitor_GetRemoteSerial(&dev, &remote_valid);
		if (remote_valid != 0u && remote != NULL) {
			const uint32_t *uid = (const uint32_t *)remote;
			snprintf(out, out_sz, "S/N:%08lX:%08lX:%08lX",
				 (unsigned long)uid[0],
				 (unsigned long)uid[1],
				 (unsigned long)uid[2]);
			return;
		}
	}

	snprintf(out, out_sz, "S/N:---");
}

static void FormatMkuDetail(char *out, size_t out_sz,
			    uint8_t zone, uint8_t h_adr, uint8_t mcu_d_type)
{
	char serial[32];
	char zone_name[ZONE_NAME_SIZE + 1];
	GetMkuSerial(zone, h_adr, mcu_d_type, serial, sizeof(serial));
	GetZoneName(zone, zone_name, sizeof(zone_name));
	snprintf(out, out_sz, "%s %s %u %s",
		 zone_name, McuTypeName(mcu_d_type), (unsigned)h_adr, serial);
}

static void FormatPpkyDetail(char *out, size_t out_sz)
{
	char serial[32];
	FormatPpkySerial(serial, sizeof(serial));
	snprintf(out, out_sz, "ППКУ %s", serial);
}

static void FormatZoneOnlyDetail(char *out, size_t out_sz, uint8_t zone)
{
	if (zone == 0u) {
		snprintf(out, out_sz, "все зоны");
		return;
	}

	{
		char zone_name[ZONE_NAME_SIZE + 1];
		GetZoneName(zone, zone_name, sizeof(zone_name));
		snprintf(out, out_sz, "%s", zone_name);
	}
}

static uint8_t LookupMcuDType(uint8_t zone, uint8_t h_adr, uint8_t fallback)
{
	uint8_t i;
	for (i = 0u; i < 32u; i++) {
		const Device *dv = &PPKYConfig.CfgDevices[i].UId.devId;
		if (!IsMcuDType(dv->d_type)) {
			continue;
		}
		if (dv->h_adr == h_adr && (zone == 0u || dv->zone == zone)) {
			return dv->d_type;
		}
	}
	return fallback;
}

static void FormatHeaderPos(char *dst, size_t dst_sz, uint32_t index_1based, uint32_t count)
{
	if (count == 0u) {
		dst[0] = '\0';
		return;
	}
	if (count >= 10000u) {
		snprintf(dst, dst_sz, "%lu/%luk",
			 (unsigned long)index_1based,
			 (unsigned long)((count + 999u) / 1000u));
	} else {
		snprintf(dst, dst_sz, "%lu/%lu",
			 (unsigned long)index_1based,
			 (unsigned long)count);
	}
}

static void FormatDeviceFaultTitle(const EventLogRecord_t *rec, char *title, size_t title_sz)
{
	const uint8_t *a = rec->additional;
	uint8_t fc = a[0];
	uint8_t ch = a[1];
	uint8_t zone = 0u;
	uint8_t h_adr = 0u;
	uint8_t l_adr = 0u;
	uint8_t d_type = 0u;

	ParseCanHeader(rec->can_header, &zone, &h_adr, &l_adr, &d_type);

	if (fc == ELUI_FC_POSITION) {
		snprintf(title, title_sz, "ПОЗИЦИЯ");
		return;
	}

	if (fc == ELUI_FC_POWER) {
		if (rec->can_data[0] != 0u) {
			snprintf(title, title_sz, "ПИТАНИЕ %u", (unsigned)(ch != 0u ? ch : 1u));
		} else {
			snprintf(title, title_sz, "ВЫХОД %u", (unsigned)(ch != 0u ? ch : 1u));
		}
		return;
	}

	if (fc == ELUI_FC_CAN) {
		const char *fault = (rec->can_data[0] == 2u) ? "КЗ" : "ОБРЫВ";
		uint8_t can_idx = (ch != 0u) ? ch : rec->can_data[1];
		if (can_idx == 0u) {
			can_idx = 1u;
		}
		snprintf(title, title_sz, "%s CAN%u", fault, (unsigned)can_idx);
		return;
	}

	if (IsVdevDType(d_type)) {
		const char *fault = "ОБРЫВ";
		uint8_t v_l = (l_adr != 0u) ? l_adr : ch;
		if (fc == ELUI_FC_LINE_SHORT) {
			fault = "КЗ";
		} else if (fc == ELUI_FC_PROTOCOL || fc == ELUI_FC_OTHER) {
			fault = "НЕИСП";
		}
		snprintf(title, title_sz, "%s %s%u", fault, ChannelTypeShort(d_type), (unsigned)v_l);
		return;
	}

	snprintf(title, title_sz, "НЕИСПР.");
}

static void FormatDeviceFaultDetail(const EventLogRecord_t *rec, char *detail, size_t detail_sz)
{
	const uint8_t *a = rec->additional;
	uint8_t fc = a[0];
	uint8_t ch = a[1];
	uint8_t zone = 0u;
	uint8_t h_adr = 0u;
	uint8_t l_adr = 0u;
	uint8_t d_type = 0u;

	ParseCanHeader(rec->can_header, &zone, &h_adr, &l_adr, &d_type);

	if (fc == ELUI_FC_POSITION) {
		snprintf(detail, detail_sz, "МКУ %u", (unsigned)(ch != 0u ? ch : rec->can_data[0]));
		return;
	}

	if (fc == ELUI_FC_POWER || (fc == ELUI_FC_CAN && d_type == DEVICE_PPKY_TYPE)) {
		FormatPpkyDetail(detail, detail_sz);
		return;
	}

	if (IsMcuDType(d_type)) {
		FormatMkuDetail(detail, detail_sz, zone, h_adr, d_type);
		return;
	}

	if (IsVdevDType(d_type)) {
		uint8_t mcu_d_type = LookupMcuDType(zone, h_adr, DEVICE_MCU_K1);
		FormatMkuDetail(detail, detail_sz, zone, h_adr, mcu_d_type);
		return;
	}

	FormatPpkyDetail(detail, detail_sz);
}

static void FormatTitle(const EventLogRecord_t *rec, char *title, size_t title_sz)
{
	const uint8_t *a = rec->additional;

	switch (rec->event_code) {
	case EVENT_LOG_DEVICE_FAULT:
		FormatDeviceFaultTitle(rec, title, title_sz);
		break;
	case EVENT_LOG_FIRE_DETECTED:
		snprintf(title, title_sz, "ПОЖАР");
		break;
	case EVENT_LOG_EXTINGUISH_START:
		snprintf(title, title_sz, "ТУШЕНИЕ");
		break;
	case EVENT_LOG_EXTINGUISH_FORCE_STOP:
		snprintf(title, title_sz, "ПОЖАР/ОСТ.");
		break;
	case EVENT_LOG_EXTINGUISH_COMPLETE:
		snprintf(title, title_sz, "ТУШ.ВЫП.");
		break;
	case EVENT_LOG_EXTINGUISH_INCOMPLETE:
		snprintf(title, title_sz, "ТУШ.ОШ.");
		break;
	case EVENT_LOG_PANEL_BUTTON:
		if (a[0] == 0u) {
			snprintf(title, title_sz, "ПУСК ОБЩИЙ");
		} else if (a[0] == 1u) {
			snprintf(title, title_sz, "ПУСК СП");
		} else if (a[0] == 2u) {
			snprintf(title, title_sz, "ОСТАНОВ");
		} else {
			snprintf(title, title_sz, "КНОПКА");
		}
		break;
	case EVENT_LOG_MASTER_START:
		snprintf(title, title_sz, "СТАРТ");
		break;
	case EVENT_LOG_MASTER_STOP:
		snprintf(title, title_sz, "СТОП");
		break;
	case EVENT_LOG_SYSTEM_START_OK:
		snprintf(title, title_sz, "СТАРТ ОК");
		break;
	case EVENT_LOG_HOST_LINK:
		if (a[0] != 0u) {
			snprintf(title, title_sz, "ЖУРНАЛ RS");
		} else {
			snprintf(title, title_sz, "СВЯЗЬ");
		}
		break;
	case EVENT_LOG_CONFIG_APPLY_OK:
		snprintf(title, title_sz, "КОНФ.ОК");
		break;
	case EVENT_LOG_CONFIG_APPLY_FAIL:
		snprintf(title, title_sz, "КОНФ.ОШ");
		break;
	case EVENT_LOG_SOUND_TOGGLE:
		snprintf(title, title_sz, "ЗВУК");
		break;
	case EVENT_LOG_FIRE_MODE_CHANGE:
		snprintf(title, title_sz, "РЕЖИМ");
		break;
	case EVENT_LOG_FIRE_RESET:
		snprintf(title, title_sz, "СБРОС");
		break;
	case EVENT_LOG_MCU_SAVED:
		snprintf(title, title_sz, "СОХР.МКУ");
		break;
	case EVENT_LOG_PANEL_BTN_PRESS:
		if (a[0] == 0u) {
			snprintf(title, title_sz, "НАЖ.ОБЩ");
		} else if (a[0] == 1u) {
			snprintf(title, title_sz, "НАЖ.СП");
		} else if (a[0] == 2u) {
			snprintf(title, title_sz, "НАЖ.ОСТ");
		} else {
			snprintf(title, title_sz, "НАЖАТИЕ");
		}
		break;
	case EVENT_LOG_COUNTDOWN_PAUSE:
		snprintf(title, title_sz, "ПАУЗА");
		break;
	case EVENT_LOG_COUNTDOWN_RESUME:
		snprintf(title, title_sz, "СНЯТ.ПАУЗ");
		break;
	case EVENT_LOG_CONFIG_SAVED:
		snprintf(title, title_sz, "СОХР.КФГ");
		break;
	case EVENT_LOG_ZONE_NAME:
		snprintf(title, title_sz, "ИМЯ ЗОНЫ");
		break;
	default:
		snprintf(title, title_sz, "СОБ.%u", (unsigned)rec->event_code);
		break;
	}
}

static void PrependPhasePrefix(const EventLogRecord_t *rec, char *title, size_t title_sz)
{
	char tmp[EVENT_LOG_UI_TITLE_LEN];

	if (title_sz == 0u || title[0] == '\0') {
		return;
	}
	if (rec->event_code != EVENT_LOG_DEVICE_FAULT) {
		return;
	}

	snprintf(tmp, sizeof(tmp), "%c%s",
		 (rec->additional[2] != 0u) ? '+' : '-',
		 title);
	strncpy(title, tmp, title_sz);
	title[title_sz - 1u] = '\0';
}

static void FormatDetail(const EventLogRecord_t *rec, char *detail, size_t detail_sz)
{
	const uint8_t *a = rec->additional;
	uint8_t zone = 0u;
	uint8_t h_adr = 0u;
	uint8_t l_adr = 0u;
	uint8_t d_type = 0u;

	detail[0] = '\0';
	ParseCanHeader(rec->can_header, &zone, &h_adr, &l_adr, &d_type);

	switch (rec->event_code) {
	case EVENT_LOG_DEVICE_FAULT:
		FormatDeviceFaultDetail(rec, detail, detail_sz);
		break;
	case EVENT_LOG_FIRE_DETECTED:
		if (rec->can_header != 0u && IsMcuDType(d_type)) {
			FormatMkuDetail(detail, detail_sz, zone, h_adr, d_type);
		} else if (rec->can_header != 0u && IsVdevDType(d_type)) {
			uint8_t mcu_d_type = LookupMcuDType(zone, h_adr, DEVICE_MCU_K1);
			FormatMkuDetail(detail, detail_sz, zone, h_adr, mcu_d_type);
		} else {
			FormatZoneOnlyDetail(detail, detail_sz, a[0]);
		}
		break;
	case EVENT_LOG_EXTINGUISH_START:
	case EVENT_LOG_EXTINGUISH_FORCE_STOP:
	case EVENT_LOG_EXTINGUISH_COMPLETE:
	case EVENT_LOG_EXTINGUISH_INCOMPLETE:
	case EVENT_LOG_FIRE_RESET:
		if (rec->event_code == EVENT_LOG_EXTINGUISH_START) {
			FormatZoneOnlyDetail(detail, detail_sz, a[1]);
		} else if (rec->event_code == EVENT_LOG_EXTINGUISH_FORCE_STOP) {
			FormatZoneOnlyDetail(detail, detail_sz, a[1]);
		} else if (rec->event_code == EVENT_LOG_EXTINGUISH_COMPLETE) {
			FormatZoneOnlyDetail(detail, detail_sz, a[3]);
		} else if (rec->event_code == EVENT_LOG_EXTINGUISH_INCOMPLETE) {
			FormatZoneOnlyDetail(detail, detail_sz, a[2]);
		} else {
			FormatZoneOnlyDetail(detail, detail_sz, a[0]);
		}
		break;
	case EVENT_LOG_PANEL_BUTTON:
	case EVENT_LOG_PANEL_BTN_PRESS:
		if (a[1] != 0u) {
			FormatZoneOnlyDetail(detail, detail_sz, a[1]);
		} else {
			FormatPpkyDetail(detail, detail_sz);
		}
		break;
	case EVENT_LOG_COUNTDOWN_PAUSE:
	case EVENT_LOG_COUNTDOWN_RESUME:
		FormatZoneOnlyDetail(detail, detail_sz, a[0]);
		break;
	case EVENT_LOG_MASTER_START:
	case EVENT_LOG_MASTER_STOP:
	case EVENT_LOG_SYSTEM_START_OK:
	case EVENT_LOG_SOUND_TOGGLE:
	case EVENT_LOG_FIRE_MODE_CHANGE:
		FormatPpkyDetail(detail, detail_sz);
		break;
	case EVENT_LOG_HOST_LINK:
		if (a[0] != 0u) {
			snprintf(detail, detail_sz, "ПАНЕЛЬ %u %s",
				 (unsigned)a[0],
				 (a[1] != 0u) ? "ACK OK" : "ACK TIMEOUT");
		} else {
			FormatPpkyDetail(detail, detail_sz);
		}
		break;
	case EVENT_LOG_CONFIG_APPLY_OK:
		snprintf(detail, detail_sz, "ППКУ OK %u/%u",
			 (unsigned)a[0], (unsigned)a[1]);
		break;
	case EVENT_LOG_CONFIG_APPLY_FAIL:
		if (IsMcuDType(d_type)) {
			FormatMkuDetail(detail, detail_sz, zone, h_adr, d_type);
		} else {
			FormatPpkyDetail(detail, detail_sz);
		}
		break;
	case EVENT_LOG_CONFIG_SAVED:
		snprintf(detail, detail_sz, "ППКУ зон %u/%u",
			 (unsigned)a[0], (unsigned)a[1]);
		break;
	case EVENT_LOG_ZONE_NAME: {
		char name[17];
		memcpy(name, rec->can_data, 8u);
		memcpy(name + 8u, rec->additional, 8u);
		name[16] = '\0';
		if (name[0] == '\0') {
			snprintf(detail, detail_sz, "%u", (unsigned)zone);
		} else {
			snprintf(detail, detail_sz, "%u %s", (unsigned)zone, name);
		}
		break;
	}
	default:
		if (IsMcuDType(d_type)) {
			FormatMkuDetail(detail, detail_sz, zone, h_adr, d_type);
		} else if (rec->can_header != 0u) {
			uint8_t mcu_d_type = LookupMcuDType(zone, h_adr, d_type);
			if (IsMcuDType(mcu_d_type)) {
				FormatMkuDetail(detail, detail_sz, zone, h_adr, mcu_d_type);
			} else {
				FormatPpkyDetail(detail, detail_sz);
			}
		} else {
			FormatPpkyDetail(detail, detail_sz);
		}
		break;
	}
}

void EventLogUi_FormatEmpty(EventLogUiLines_t *out)
{
	if (out == NULL) {
		return;
	}
	snprintf(out->header, sizeof(out->header), "ЖУРНАЛ");
	snprintf(out->title, sizeof(out->title), "ПУСТО");
	snprintf(out->detail, sizeof(out->detail), "Нет записей");
}

void EventLogUi_FormatRecord(const EventLogRecord_t *rec,
			     uint32_t display_index_1based,
			     uint32_t count,
			     EventLogUiLines_t *out)
{
	uint8_t yy;
	uint8_t mo;
	uint8_t dd;
	uint8_t hh;
	uint8_t mi;
	char pos[16];

	if (out == NULL) {
		return;
	}

	memset(out, 0, sizeof(*out));
	if (rec == NULL) {
		EventLogUi_FormatEmpty(out);
		return;
	}

	yy = BcdToBin(rec->time[0]);
	mo = BcdToBin(rec->time[1]);
	dd = BcdToBin(rec->time[2]);
	hh = BcdToBin(rec->time[3]);
	mi = BcdToBin(rec->time[4]);

	FormatHeaderPos(pos, sizeof(pos), display_index_1based, count);
	snprintf(out->header, sizeof(out->header),
		 "%s %02u.%02u.%02u %02u:%02u",
		 pos, (unsigned)dd, (unsigned)mo, (unsigned)yy,
		 (unsigned)hh, (unsigned)mi);

	FormatTitle(rec, out->title, sizeof(out->title));
	PrependPhasePrefix(rec, out->title, sizeof(out->title));
	FormatDetail(rec, out->detail, sizeof(out->detail));
}
