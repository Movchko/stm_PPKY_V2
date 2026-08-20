#include "event_log_reader.h"
#include "event_log.h"
#include "event_log_catalog.h"
#include "service.h"

#include <string.h>

static bool EventLogReader_GetTier(uint8_t tier, EventLogTier_t **out)
{
	if (out == NULL || !EventLog_IsInitialized()) {
		return false;
	}
	if (tier == 0u) {
		*out = EventLog_GetCriticalTier();
	} else if (tier == 1u) {
		*out = EventLog_GetGeneralTier();
	} else {
		return false;
	}
	if ((*out)->initialized == false) {
		return false;
	}
	return true;
}

static bool EventLogReader_IsRecordEmpty(const EventLogRecord_t *record)
{
	const uint8_t *data = (const uint8_t *)record;
	for (uint32_t i = 0; i < sizeof(EventLogRecord_t); i++) {
		if (data[i] != 0xFFu) {
			return false;
		}
	}
	return true;
}

static uint32_t EventLogReader_OldestPhysical(const EventLogTier_t *tier)
{
	const uint32_t capacity = EventLogTier_GetCapacityRecords(tier);
	const uint32_t count = tier->total_records;
	if (count == 0u) {
		return 0u;
	}
	if (count < capacity) {
		return 0u;
	}
	return (tier->last_index + 1u) % capacity;
}

bool EventLogReader_GetTierInfo(uint8_t tier, EventLogTierInfo_t *info)
{
	EventLogTier_t *tier_ptr;

	if (info == NULL || !EventLog_IsInitialized()) {
		return false;
	}

	memset(info, 0, sizeof(*info));
	if (!EventLogReader_GetTier(tier, &tier_ptr)) {
		return true;
	}

	info->capacity = EventLogTier_GetCapacityRecords(tier_ptr);
	info->count = tier_ptr->total_records;
	info->write_head = tier_ptr->last_index;
	return true;
}

bool EventLogReader_ReadLogical(uint8_t tier,
                                uint32_t logical_index,
                                EventLogRecStatus_t *status,
                                EventLogRecord_t *record)
{
	EventLogTier_t *tier_ptr;
	uint32_t physical;
	uint32_t count;

	if (status == NULL || record == NULL) {
		return false;
	}
	if (!EventLogReader_GetTier(tier, &tier_ptr)) {
		return false;
	}

	count = tier_ptr->total_records;
	if (logical_index >= count) {
		return false;
	}

	physical = (EventLogReader_OldestPhysical(tier_ptr) + logical_index)
	           % EventLogTier_GetCapacityRecords(tier_ptr);
	if (!EventLogTier_Read(tier_ptr, physical, record)) {
		return false;
	}

	if (EventLogReader_IsRecordEmpty(record)) {
		*status = EVENT_LOG_REC_EMPTY;
	} else if (!EventLogTier_VerifyChecksum(record)) {
		*status = EVENT_LOG_REC_INVALID;
	} else {
		*status = EVENT_LOG_REC_VALID;
	}
	return true;
}

uint32_t EventLogReader_GetCatalogCrc32(void)
{
	return crc32(POLYNOM,
	             g_event_log_catalog,
	             (uint32_t)(g_event_log_catalog_count * sizeof(g_event_log_catalog[0])));
}
