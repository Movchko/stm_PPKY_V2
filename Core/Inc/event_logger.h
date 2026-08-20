/*
 * event_logger.h
 *
 * Низкоуровневый циклический логер одного уровня (кольцевой буфер в SPI Flash).
 */

#ifndef INC_EVENT_LOGGER_H_
#define INC_EVENT_LOGGER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "spif.h"
#include <stdbool.h>
#include <stdint.h>

#define EVENT_LOG_RECORD_SIZE      32u
#define EVENT_LOG_SECTOR_SIZE      SPIF_SECTOR_SIZE
#define EVENT_LOG_RECORDS_PER_SECTOR_FLASH  (EVENT_LOG_SECTOR_SIZE / EVENT_LOG_RECORD_SIZE)

#if defined(__GNUC__)
#pragma pack(push, 1)
#endif

typedef struct {
	uint8_t  time[6];
	uint8_t  master_wagon_num;
	uint8_t  reserved;
	uint16_t event_code;
	uint32_t can_header;
	uint8_t  can_data[8];
	uint8_t  additional[8];
	uint16_t checksum;
} EventLogRecord_t;

#if defined(__GNUC__)
#pragma pack(pop)
#endif

typedef struct {
	SPIF_HandleTypeDef *spif_handle;
	uint32_t            start_sector;
	uint32_t            end_sector;
	uint32_t            total_records;
	uint32_t            last_index;
	uint32_t            current_sector;
	uint32_t            current_offset;
	bool                initialized;
} EventLogTier_t;

uint16_t EventLogTier_CalculateChecksum(const EventLogRecord_t *record);
bool EventLogTier_VerifyChecksum(const EventLogRecord_t *record);

bool EventLogTier_Init(EventLogTier_t *tier,
                       SPIF_HandleTypeDef *spif_handle,
                       uint32_t start_sector,
                       uint32_t end_sector);

bool EventLogTier_Write(EventLogTier_t *tier, EventLogRecord_t *record);
bool EventLogTier_Read(EventLogTier_t *tier, uint32_t index, EventLogRecord_t *record);

uint32_t EventLogTier_GetCapacityRecords(const EventLogTier_t *tier);
uint32_t EventLogTier_GetRecordsLostOnSectorWrap(void);

typedef EventLogTier_t EventLogger_t;
#define EventLogger_CalculateChecksum  EventLogTier_CalculateChecksum
#define EventLogger_VerifyChecksum     EventLogTier_VerifyChecksum
#define EventLogger_Init               EventLogTier_Init
#define EventLogger_WriteEvent         EventLogTier_Write
#define EventLogger_ReadEvent          EventLogTier_Read

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOGGER_H_ */
