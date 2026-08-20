/*
 * event_log_reader.h
 */

#ifndef INC_EVENT_LOG_READER_H_
#define INC_EVENT_LOG_READER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "event_logger.h"

typedef enum {
	EVENT_LOG_REC_VALID   = 0,
	EVENT_LOG_REC_EMPTY   = 1,
	EVENT_LOG_REC_INVALID = 2
} EventLogRecStatus_t;

typedef struct {
	uint32_t capacity;
	uint32_t count;
	uint32_t write_head;
} EventLogTierInfo_t;

bool EventLogReader_GetTierInfo(uint8_t tier, EventLogTierInfo_t *info);
bool EventLogReader_ReadLogical(uint8_t tier,
                                uint32_t logical_index,
                                EventLogRecStatus_t *status,
                                EventLogRecord_t *record);
uint32_t EventLogReader_GetCatalogCrc32(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOG_READER_H_ */
