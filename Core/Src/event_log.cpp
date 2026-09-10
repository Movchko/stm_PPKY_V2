#include "event_log.h"
#include "app.hpp"
#include "rtc_cache.h"
#include "device_config.h"
#include "menu_ui.h"
#include "backend.h"
#include "stm32h5xx_hal.h"

#include <string.h>

extern struct PPKYCfg PPKYConfig;
extern RTC_HandleTypeDef hrtc;
extern ActiveDeviceInfo g_active_devices[];
extern uint8_t g_active_devices_count;

static EventLogTier_t g_critical_tier;
static EventLogTier_t g_general_tier;
static EventLogCapacityInfo_t g_capacity;
static uint8_t g_debug_enabled = 0u;
static bool g_initialized = false;
static uint8_t s_host_link_logged[2] = {0u, 0u};

static_assert(FLASH_LOG_GENERAL_END_SECTOR < SPI_FLASH_SECTOR_COUNT, "Event log region exceeds SPI flash size");
static_assert(FLASH_LOG_CRITICAL_SECTORS > 0u && FLASH_LOG_GENERAL_SECTORS > 0u, "No SPI flash sectors left for event log after config region");
static_assert(FLASH_LOG_CRITICAL_RECORDS >= FLASH_LOG_CRITICAL_MIN_RECORDS, "Critical event log capacity below minimum");
static_assert(FLASH_LOG_GENERAL_END_SECTOR == (FLASH_LOG_START_SECTOR + FLASH_LOG_TOTAL_SECTORS - 1u), "Event log regions must cover all log sectors without gaps");

static void EventLog_FillCapacityInfo(void)
{
	g_capacity.log_start_sector = FLASH_LOG_START_SECTOR;
	g_capacity.log_total_sectors = FLASH_LOG_TOTAL_SECTORS;
	g_capacity.critical_sectors = FLASH_LOG_CRITICAL_SECTORS;
	g_capacity.general_sectors = FLASH_LOG_GENERAL_SECTORS;
	g_capacity.unused_tail_sectors = FLASH_LOG_UNUSED_TAIL_SECTORS;
	g_capacity.critical_start_sector = FLASH_LOG_CRITICAL_START_SECTOR;
	g_capacity.critical_end_sector = FLASH_LOG_CRITICAL_END_SECTOR;
	g_capacity.general_start_sector = FLASH_LOG_GENERAL_START_SECTOR;
	g_capacity.general_end_sector = FLASH_LOG_GENERAL_END_SECTOR;
	g_capacity.critical_record_capacity = FLASH_LOG_CRITICAL_RECORDS;
	g_capacity.general_record_capacity = FLASH_LOG_GENERAL_RECORDS;
	g_capacity.records_lost_on_sector_wrap = EVENT_LOG_RECORDS_PER_SECTOR;
}

static void EventLog_FillTimeBcd(uint8_t time[6])
{
	RTC_TimeTypeDef time_bcd;
	RTC_DateTypeDef date_bcd;
	if (time == NULL) return;
	if (!RtcCache_GetBcd(&time_bcd, &date_bcd)) { memset(time, 0, 6); return; }
	time[0] = date_bcd.Year; time[1] = date_bcd.Month; time[2] = date_bcd.Date;
	time[3] = time_bcd.Hours; time[4] = time_bcd.Minutes; time[5] = time_bcd.Seconds;
}

static bool EventLog_WriteToTier(EventLogTier_t *tier, const uint8_t *time_bcd, uint16_t code, const EventLogPayload_t *payload)
{
	EventLogRecord_t record;
	memset(&record, 0, sizeof(record));
	if (time_bcd != NULL) memcpy(record.time, time_bcd, sizeof(record.time)); else EventLog_FillTimeBcd(record.time);
	record.event_code = code;
	if (payload != NULL) {
		record.master_wagon_num = payload->master_wagon_num;
		record.can_header = payload->can_header;
		memcpy(record.can_data, payload->can_data, sizeof(record.can_data));
		memcpy(record.additional, payload->additional, sizeof(record.additional));
	}
	return EventLogTier_Write(tier, &record);
}

static bool EventLog_BcdFieldValid(uint8_t bcd, uint8_t min_val, uint8_t max_val)
{
	const uint8_t tens = (uint8_t)((bcd >> 4) & 0x0Fu);
	const uint8_t ones = (uint8_t)(bcd & 0x0Fu);
	const uint8_t val = (uint8_t)(tens * 10u + ones);
	if (tens > 9u || ones > 9u) return false;
	return (val >= min_val) && (val <= max_val);
}

static uint32_t EventLog_PackMmDdHhMm(uint8_t month, uint8_t day, uint8_t hours, uint8_t minutes)
{
	return ((uint32_t)month << 24) | ((uint32_t)day << 16) | ((uint32_t)hours << 8) | (uint32_t)minutes;
}

static uint8_t EventLog_DecYearBcd(uint8_t year_bcd)
{
	uint8_t val = (uint8_t)(((year_bcd >> 4) & 0x0Fu) * 10u + (year_bcd & 0x0Fu));
	val = (val > 0u) ? (uint8_t)(val - 1u) : 99u;
	return (uint8_t)(((val / 10u) << 4) | (val % 10u));
}

static bool EventLog_ReadLastRunTimeBcd(uint8_t time_bcd[6])
{
	RTC_DateTypeDef bkp_date = {};
	RTC_TimeTypeDef bkp_time = {};
	RTC_DateTypeDef cur_date = {};
	RTC_TimeTypeDef cur_time = {};
	const uint32_t bkp_raw = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);
	if (time_bcd == NULL || bkp_raw == 0u) return false;
	PPKY_GetLastPowerOnDate(&bkp_date, &bkp_time);
	if (!EventLog_BcdFieldValid(bkp_date.Month, 1u, 12u) || !EventLog_BcdFieldValid(bkp_date.Date, 1u, 31u) || !EventLog_BcdFieldValid(bkp_time.Hours, 0u, 23u) || !EventLog_BcdFieldValid(bkp_time.Minutes, 0u, 59u)) return false;
	if (!RtcCache_GetBcd(&cur_time, &cur_date)) time_bcd[0] = 0u;
	else {
		time_bcd[0] = cur_date.Year;
		if (EventLog_PackMmDdHhMm(cur_date.Month, cur_date.Date, cur_time.Hours, cur_time.Minutes) < EventLog_PackMmDdHhMm(bkp_date.Month, bkp_date.Date, bkp_time.Hours, bkp_time.Minutes)) time_bcd[0] = EventLog_DecYearBcd(cur_date.Year);
	}
	time_bcd[1] = bkp_date.Month; time_bcd[2] = bkp_date.Date; time_bcd[3] = bkp_time.Hours; time_bcd[4] = bkp_time.Minutes; time_bcd[5] = 0u;
	return true;
}

static bool EventLog_PostInternal(const uint8_t *time_bcd, uint16_t code, const EventLogPayload_t *payload)
{
	const EventLogDescriptor_t *desc;
	EventLogTier_t *tier;
	if (!g_initialized) return false;
	desc = EventLogCatalog_Find(code);
	if (desc == NULL) return false;
	if (desc->debug_only != 0u && g_debug_enabled == 0u) return true;
	tier = (desc->level == EVENT_LOG_LEVEL_CRITICAL) ? &g_critical_tier : &g_general_tier;
	return EventLog_WriteToTier(tier, time_bcd, code, payload);
}

bool EventLog_Init(SPIF_HandleTypeDef *spif_handle)
{
	uint32_t general_end_sector = FLASH_LOG_GENERAL_END_SECTOR;
	EventLog_FillCapacityInfo();
	if (spif_handle != NULL && spif_handle->SectorCnt > 0u && general_end_sector >= spif_handle->SectorCnt) general_end_sector = spif_handle->SectorCnt - 1u;
	const bool ok_critical = EventLogTier_Init(&g_critical_tier, spif_handle, FLASH_LOG_CRITICAL_START_SECTOR, FLASH_LOG_CRITICAL_END_SECTOR);
	if (general_end_sector >= FLASH_LOG_GENERAL_START_SECTOR) (void)EventLogTier_Init(&g_general_tier, spif_handle, FLASH_LOG_GENERAL_START_SECTOR, general_end_sector);
	g_initialized = ok_critical;
	return g_initialized;
}

bool EventLog_IsInitialized(void) { return g_initialized; }
void EventLog_SetDebug(uint8_t enabled) { g_debug_enabled = (enabled != 0u) ? 1u : 0u; }
uint8_t EventLog_GetDebug(void) { return g_debug_enabled; }
const EventLogCapacityInfo_t *EventLog_GetCapacityInfo(void) { return &g_capacity; }
bool EventLog_Post(uint16_t code, const EventLogPayload_t *payload) { return EventLog_PostInternal(NULL, code, payload); }
bool EventLog_PostAt(const uint8_t time_bcd[6], uint16_t code, const EventLogPayload_t *payload) { return (time_bcd == NULL) ? false : EventLog_PostInternal(time_bcd, code, payload); }

void EventLog_LogMasterBoot(void)
{
	EventLogPayload_t payload;
	uint8_t stop_time_bcd[6];
	if (!g_initialized) return;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	if (EventLog_ReadLastRunTimeBcd(stop_time_bcd)) { payload.additional[0] = 1u; (void)EventLog_PostAt(stop_time_bcd, EVENT_LOG_MASTER_STOP, &payload); }
	memset(payload.additional, 0, sizeof(payload.additional)); payload.additional[0] = 0u;
	(void)EventLog_Post(EVENT_LOG_MASTER_START, &payload);
}

void EventLog_LogCanTelemetry(uint32_t can_id, const uint8_t *data)
{
	EventLogPayload_t payload;
	if (!g_initialized || data == NULL) return;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.can_header = can_id & 0x1FFFFFFFu;
	memcpy(payload.can_data, data, 8u); payload.additional[0] = 0u;
	(void)EventLog_Post(EVENT_LOG_TELEMETRY, &payload);
}

void EventLog_LogHostLink(uint8_t media)
{
	EventLogPayload_t payload;
	uint8_t idx = (media != 0u) ? 1u : 0u;
	if (!g_initialized || s_host_link_logged[idx] != 0u) return;
	s_host_link_logged[idx] = 1u;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.additional[0] = idx;
	(void)EventLog_Post(EVENT_LOG_HOST_LINK, &payload);
}

void EventLog_HostLinkSessionReset(uint8_t media) { s_host_link_logged[(media != 0u) ? 1u : 0u] = 0u; }

void EventLog_LogConfigApplyOk(uint8_t mcu_ok_count, uint8_t mcu_total)
{
	EventLogPayload_t payload;
	if (!g_initialized) return;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.additional[0] = mcu_ok_count; payload.additional[1] = mcu_total;
	(void)EventLog_Post(EVENT_LOG_CONFIG_APPLY_OK, &payload);
}

#define EVENT_LOG_ZONE_NAME_PACKED_BYTES  16u /* can_data[8] + additional[8] */

static uint8_t EventLog_CountNamedZones(void)
{
	uint8_t count = 0u;
	for (uint16_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (PPKYConfig.zone_name[zi][0] != 0) {
			count++;
		}
	}
	return count;
}

static void EventLog_PackZoneName(const int8_t *src, uint8_t can_data[8], uint8_t additional[8])
{
	uint8_t packed[EVENT_LOG_ZONE_NAME_PACKED_BYTES];
	uint8_t i;

	memset(packed, 0, sizeof(packed));
	if (src == nullptr) {
		memcpy(can_data, packed, 8u);
		memcpy(additional, packed + 8u, 8u);
		return;
	}
	for (i = 0u; i < EVENT_LOG_ZONE_NAME_PACKED_BYTES && i < ZONE_NAME_SIZE; i++) {
		if (src[i] == 0) {
			break;
		}
		packed[i] = (uint8_t)src[i];
	}
	memcpy(can_data, packed, 8u);
	memcpy(additional, packed + 8u, 8u);
}

static void EventLog_LogZoneName(uint8_t zone_1based, const int8_t *name)
{
	EventLogPayload_t payload;
	can_ext_id_t id;

	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;

	id.ID = 0u;
	id.field.zone = zone_1based & 0x7Fu;
	id.field.d_type = DEVICE_PPKY_TYPE;
	id.field.h_adr = PPKYConfig.UId.devId.h_adr;
	id.field.dir = 1u;
	payload.can_header = id.ID & 0x1FFFFFFFu;

	EventLog_PackZoneName(name, payload.can_data, payload.additional);
	(void)EventLog_Post(EVENT_LOG_ZONE_NAME, &payload);
}

void EventLog_LogConfigSaved(void)
{
	EventLogPayload_t payload;
	uint8_t named_count;

	if (!g_initialized) {
		return;
	}

	named_count = EventLog_CountNamedZones();
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.additional[0] = named_count;
	payload.additional[1] = (uint8_t)ZONE_NUMBER;
	if (!EventLog_Post(EVENT_LOG_CONFIG_SAVED, &payload)) {
		return;
	}

	for (uint16_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (PPKYConfig.zone_name[zi][0] == 0) {
			continue;
		}
		EventLog_LogZoneName((uint8_t)(zi + 1u), PPKYConfig.zone_name[zi]);
	}
}

static uint8_t EventLog_IsMcuType(uint8_t d_type)
{
	return (d_type == DEVICE_MCU_IGN_TYPE || d_type == DEVICE_MCU_TC_TYPE || d_type == DEVICE_MCU_K1 || d_type == DEVICE_MCU_K2 || d_type == DEVICE_MCU_K3 || d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

void EventLog_LogConfigApplyFail(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone, uint8_t slot, uint8_t reason)
{
	EventLogPayload_t payload; can_ext_id_t id;
	if (!g_initialized) return;
	memset(&payload, 0, sizeof(payload)); payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	id.ID = 0u; id.field.zone = zone & 0x7Fu; id.field.l_adr = l_adr & 0x3Fu; id.field.h_adr = h_adr; id.field.d_type = d_type & 0x7Fu; id.field.dir = 1u;
	payload.can_header = id.ID & 0x1FFFFFFFu;
	payload.additional[0] = reason; payload.additional[1] = slot; payload.additional[2] = h_adr; payload.additional[3] = zone & 0x7Fu; payload.additional[4] = d_type & 0x7Fu;
	(void)EventLog_Post(EVENT_LOG_CONFIG_APPLY_FAIL, &payload);
}

static void EventLog_PackMcuSerial(const UniqId *uid, uint8_t can_data[8], uint8_t additional[8])
{
	if (uid == nullptr) return;
	memcpy(can_data, &uid->UId0, 4u); memcpy(can_data + 4u, &uid->UId1, 4u); memcpy(additional, &uid->UId2, 4u);
}

void EventLog_LogMcuSaved(const Device *dev, const UniqId *uid)
{
	EventLogPayload_t payload; can_ext_id_t id;
	if (!g_initialized || dev == nullptr || uid == nullptr || EventLog_IsMcuType(dev->d_type) == 0u) return;
	memset(&payload, 0, sizeof(payload)); payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	id.ID = 0u; id.field.zone = dev->zone & 0x7Fu; id.field.l_adr = dev->l_adr & 0x3Fu; id.field.h_adr = dev->h_adr; id.field.d_type = dev->d_type & 0x7Fu; id.field.dir = 1u;
	payload.can_header = id.ID & 0x1FFFFFFFu;
	EventLog_PackMcuSerial(uid, payload.can_data, payload.additional);
	(void)EventLog_Post(EVENT_LOG_MCU_SAVED, &payload);
}

void EventLog_LogAllCfgMcusSaved(void)
{
	if (!g_initialized) return;
	for (uint8_t i = 0u; i < MAX_MCU_IN_BUS; i++) {
		const MKUCfg *mcu = &PPKYConfig.CfgDevices[i];
		const Device *dev = &mcu->UId.devId;
		if (EventLog_IsMcuType(dev->d_type) == 0u) continue;
		EventLog_LogMcuSaved(dev, &mcu->UId);
	}
}

void EventLog_LogSoundToggle(uint8_t enabled, uint8_t source)
{
	EventLogPayload_t payload;
	if (!g_initialized) return;
	memset(&payload, 0, sizeof(payload)); payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.additional[0] = enabled ? 1u : 0u; payload.additional[1] = source;
	(void)EventLog_Post(EVENT_LOG_SOUND_TOGGLE, &payload);
}

void EventLog_LogFireModeChange(uint8_t mode, uint8_t source)
{
	EventLogPayload_t payload;
	if (!g_initialized) return;
	memset(&payload, 0, sizeof(payload)); payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.additional[0] = mode; payload.additional[1] = source;
	(void)EventLog_Post(EVENT_LOG_FIRE_MODE_CHANGE, &payload);
}

static uint32_t BuildSampleCanHeader(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone)
{
	can_ext_id_t id; id.ID = 0u; id.field.zone = zone & 0x7Fu; id.field.l_adr = l_adr & 0x3Fu; id.field.h_adr = h_adr; id.field.d_type = d_type & 0x7Fu; id.field.dir = 1u; return id.ID & 0x1FFFFFFFu;
}

static void EventLog_PostTelemetrySample(uint32_t can_header, const uint8_t *can_data, uint8_t kind)
{
	EventLogPayload_t payload;
	memset(&payload, 0, sizeof(payload)); payload.master_wagon_num = PPKYConfig.UId.devId.h_adr; payload.can_header = can_header;
	if (can_data != NULL) memcpy(payload.can_data, can_data, 8u);
	payload.additional[0] = kind;
	(void)EventLog_Post(EVENT_LOG_TELEMETRY_SAMPLE, &payload);
}

void EventLog_ProcessTelemetrySample(uint32_t now_ms)
{
	static uint32_t s_next_ms = 0u;
	static uint8_t s_started = 0u, s_dumping = 0u, s_mcu_i = 0u;
	static int8_t s_vdev_i = -1;
	uint8_t budget;
	if (!g_initialized) return;
	if (s_started == 0u) { s_started = 1u; s_next_ms = now_ms + EVENT_LOG_TELEMETRY_SAMPLE_PERIOD_MS; return; }
	if (s_dumping == 0u) {
		if ((int32_t)(now_ms - s_next_ms) < 0) return;
		s_dumping = 1u; s_mcu_i = 0u; s_vdev_i = -1; s_next_ms = now_ms + EVENT_LOG_TELEMETRY_SAMPLE_PERIOD_MS;
	}
	budget = EVENT_LOG_TELEMETRY_SAMPLE_BUDGET;
	while (budget != 0u && s_dumping != 0u) {
		ActiveDeviceInfo *m;
		if (s_mcu_i >= g_active_devices_count) { s_dumping = 0u; break; }
		m = &g_active_devices[s_mcu_i];
		if (!m->online) { s_mcu_i++; s_vdev_i = -1; continue; }
		if (s_vdev_i < 0) {
			if (m->can_status_valid != 0u) {
				uint32_t hdr = BuildSampleCanHeader(m->dev.d_type, m->dev.h_adr, m->dev.l_adr, m->dev.zone);
				EventLog_PostTelemetrySample(hdr, m->mcu_status_data, 0u);
				budget--;
			}
			s_vdev_i = 0;
			continue;
		}
		if ((uint8_t)s_vdev_i >= m->vdev_count) { s_mcu_i++; s_vdev_i = -1; continue; }
		auto *v = &m->vdevs[(uint8_t)s_vdev_i];
		s_vdev_i++;
		if (!v->online) continue;
		uint8_t data[8];
		uint32_t hdr = BuildSampleCanHeader(v->v_d_type, m->dev.h_adr, v->v_l_adr, m->dev.zone);
		data[0] = v->status_cmd;
		memcpy(&data[1], v->status_params, 7u);
		EventLog_PostTelemetrySample(hdr, data, 1u);
		budget--;
	}
}

extern "C" void App_OnHostConfigCommand(uint8_t bus, uint8_t command)
{
	(void)bus;
	if (command == ServiceCmd_SetConfigWord || command == ServiceCmd_StartSetConfig) {
		if (!MenuUi_IsConfigSessionActive()) {
			MenuUi_SetConfigSession(1u);
			MenuConfig_Reset();
		}
	}
	if ((bus & BUS_UART1) != 0u) {
		EventLog_LogHostLink(0u); /* WiFi / ESP32 UART2 */
	}
}

EventLogTier_t *EventLog_GetCriticalTier(void) { return &g_critical_tier; }
EventLogTier_t *EventLog_GetGeneralTier(void) { return &g_general_tier; }
