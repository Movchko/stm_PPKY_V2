/*
 * event_log.h
 *
 * Высокоуровневый API логера событий ППКУ.
 */

#ifndef INC_EVENT_LOG_H_
#define INC_EVENT_LOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "event_logger.h"
#include "event_log_catalog.h"
#include "spif.h"
#include "backend.h"

typedef struct {
	uint32_t log_start_sector;
	uint32_t log_total_sectors;
	uint32_t critical_sectors;
	uint32_t general_sectors;
	uint32_t unused_tail_sectors;
	uint32_t critical_start_sector;
	uint32_t critical_end_sector;
	uint32_t general_start_sector;
	uint32_t general_end_sector;
	uint32_t critical_record_capacity;
	uint32_t general_record_capacity;
	uint32_t records_lost_on_sector_wrap;
} EventLogCapacityInfo_t;

typedef struct {
	uint8_t  master_wagon_num;
	uint32_t can_header;
	uint8_t  can_data[8];
	uint8_t  additional[8];
} EventLogPayload_t;

bool EventLog_Init(SPIF_HandleTypeDef *spif_handle);
bool EventLog_IsInitialized(void);

void EventLog_SetDebug(uint8_t enabled);
uint8_t EventLog_GetDebug(void);

const EventLogCapacityInfo_t *EventLog_GetCapacityInfo(void);

bool EventLog_Post(uint16_t code, const EventLogPayload_t *payload);
bool EventLog_PostAt(const uint8_t time_bcd[6], uint16_t code, const EventLogPayload_t *payload);

void EventLog_LogMasterBoot(void);
void EventLog_LogCanTelemetry(uint32_t can_id, const uint8_t *data);
void EventLog_LogHostLink(uint8_t media);
void EventLog_HostLinkSessionReset(uint8_t media);
void EventLog_LogConfigApplyOk(uint8_t mcu_ok_count, uint8_t mcu_total);
void EventLog_LogMcuSaved(const Device *dev, const UniqId *uid);
void EventLog_LogAllCfgMcusSaved(void);
void EventLog_LogConfigApplyFail(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone,
                                 uint8_t slot, uint8_t reason);
void EventLog_LogSoundToggle(uint8_t enabled, uint8_t source);
void EventLog_LogFireModeChange(uint8_t mode, uint8_t source);

#ifndef EVENT_LOG_TELEMETRY_SAMPLE_PERIOD_MS
#define EVENT_LOG_TELEMETRY_SAMPLE_PERIOD_MS  (10u * 60u * 1000u)
#endif

#ifndef EVENT_LOG_TELEMETRY_SAMPLE_BUDGET
#define EVENT_LOG_TELEMETRY_SAMPLE_BUDGET     4u
#endif

void EventLog_ProcessTelemetrySample(uint32_t now_ms);

EventLogTier_t *EventLog_GetCriticalTier(void);
EventLogTier_t *EventLog_GetGeneralTier(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOG_H_ */
