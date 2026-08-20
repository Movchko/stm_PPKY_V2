/*
 * event_logger.cpp
 *
 * Циклический буфер одного уровня логера во внешней SPI Flash.
 */

#include "event_logger.h"
#include <stddef.h>
#include <string.h>

static_assert(sizeof(EventLogRecord_t) == EVENT_LOG_RECORD_SIZE,
              "EventLogRecord_t must match flash record size");
static_assert(offsetof(EventLogRecord_t, checksum) == 30u,
              "EventLogRecord_t checksum offset mismatch");

uint16_t EventLogTier_CalculateChecksum(const EventLogRecord_t *record)
{
	uint16_t crc = 0xFFFFu;
	const uint8_t *data = (const uint8_t *)record;
	const uint32_t size = EVENT_LOG_RECORD_SIZE - sizeof(uint16_t);

	for (uint32_t i = 0; i < size; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (uint8_t j = 0; j < 8u; j++) {
			if ((crc & 0x8000u) != 0u) {
				crc = (uint16_t)((crc << 1) ^ 0x1021u);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

bool EventLogTier_VerifyChecksum(const EventLogRecord_t *record)
{
	return EventLogTier_CalculateChecksum(record) == record->checksum;
}

static bool EventLogTier_IsRecordEmpty(const EventLogRecord_t *record)
{
	const uint8_t *data = (const uint8_t *)record;
	for (uint32_t i = 0; i < sizeof(EventLogRecord_t); i++) {
		if (data[i] != 0xFFu) {
			return false;
		}
	}
	return true;
}

static uint32_t EventLogTier_GetAddressByIndex(const EventLogTier_t *tier, uint32_t index)
{
	const uint32_t total_sectors = tier->end_sector - tier->start_sector + 1u;
	const uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
	const uint32_t sector_index = (index % total_capacity) / EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
	const uint32_t offset_in_sector = (index % total_capacity) % EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
	const uint32_t sector = tier->start_sector + sector_index;

	return SPIF_SectorToAddress(sector) + (offset_in_sector * EVENT_LOG_RECORD_SIZE);
}

uint32_t EventLogTier_GetRecordsLostOnSectorWrap(void)
{
	return EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
}

uint32_t EventLogTier_GetCapacityRecords(const EventLogTier_t *tier)
{
	if (tier == NULL) {
		return 0u;
	}
	const uint32_t total_sectors = tier->end_sector - tier->start_sector + 1u;
	return total_sectors * EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
}

static bool EventLogTier_SectorIsEmpty(EventLogTier_t *tier, uint32_t sector)
{
	EventLogRecord_t record;
	const uint32_t sector_address = SPIF_SectorToAddress(sector);

	for (uint32_t record_offset = 0u; record_offset < EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
	     record_offset++) {
		const uint32_t record_address = sector_address + (record_offset * EVENT_LOG_RECORD_SIZE);

		if (SPIF_ReadAddress(tier->spif_handle, record_address, (uint8_t *)&record,
		                     EVENT_LOG_RECORD_SIZE) != true) {
			return false;
		}
		if (!EventLogTier_IsRecordEmpty(&record)) {
			return false;
		}
	}
	return true;
}

static uint32_t EventLogTier_GetWriteIndex(const EventLogTier_t *tier, uint32_t total_capacity)
{
	if (tier->total_records >= total_capacity) {
		return (tier->last_index + 1u) % total_capacity;
	}
	if (tier->total_records > 0u) {
		return (tier->last_index + 1u) % total_capacity;
	}

	const uint32_t sector_index = tier->current_sector - tier->start_sector;
	return sector_index * EVENT_LOG_RECORDS_PER_SECTOR_FLASH + tier->current_offset;
}

bool EventLogTier_Init(EventLogTier_t *tier,
                       SPIF_HandleTypeDef *spif_handle,
                       uint32_t start_sector,
                       uint32_t end_sector)
{
	bool retVal = false;

	do {
		if (tier == NULL || spif_handle == NULL) {
			break;
		}
		if (spif_handle->Inited == 0u) {
			break;
		}
		if (start_sector > end_sector) {
			break;
		}
		if (end_sector >= spif_handle->SectorCnt) {
			break;
		}

		memset(tier, 0, sizeof(*tier));
		tier->spif_handle = spif_handle;
		tier->start_sector = start_sector;
		tier->end_sector = end_sector;
		tier->current_sector = start_sector;

		EventLogRecord_t record;
		uint32_t last_valid_index = 0u;
		const uint32_t total_sectors = end_sector - start_sector + 1u;
		const uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		uint32_t first_empty_index = total_capacity;

		for (uint32_t sector = start_sector; sector <= end_sector; sector++) {
			const uint32_t sector_address = SPIF_SectorToAddress(sector);
			const uint32_t sector_base_index = (sector - start_sector) * EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
			const uint32_t first_record_address = sector_address;

			if (SPIF_ReadAddress(spif_handle, first_record_address, (uint8_t *)&record,
			                     EVENT_LOG_RECORD_SIZE) != true) {
				if (first_empty_index == total_capacity) {
					first_empty_index = sector_base_index;
				}
				continue;
			}
			if (EventLogTier_IsRecordEmpty(&record)) {
				continue;
			}

			for (uint32_t record_offset = 0; record_offset < EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
			     record_offset++) {
				const uint32_t record_address = sector_address + (record_offset * EVENT_LOG_RECORD_SIZE);
				const uint32_t current_index = sector_base_index + record_offset;

				if (SPIF_ReadAddress(spif_handle, record_address, (uint8_t *)&record,
				                     EVENT_LOG_RECORD_SIZE) != true) {
					if (first_empty_index == total_capacity) {
						first_empty_index = current_index;
					}
					break;
				}

				if (EventLogTier_IsRecordEmpty(&record)) {
					if (first_empty_index == total_capacity) {
						first_empty_index = current_index;
					}
					break;
				}

				if (!EventLogTier_VerifyChecksum(&record)) {
					if (tier->total_records == 0u) {
						continue;
					}
					if (first_empty_index == total_capacity) {
						first_empty_index = current_index;
					}
					break;
				}

				tier->total_records++;
				last_valid_index = current_index;
			}
		}

		if (tier->total_records > 0u) {
			tier->last_index = last_valid_index;
		} else {
			tier->last_index = 0u;
		}

		uint32_t next_index;
		if (first_empty_index < total_capacity) {
			next_index = first_empty_index;
		} else if (tier->total_records > 0u) {
			next_index = (tier->last_index + 1u) % total_capacity;
		} else {
			next_index = 0u;
		}

		const uint32_t sector_index = next_index / EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		const uint32_t offset_in_sector = next_index % EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		tier->current_sector = start_sector + sector_index;
		tier->current_offset = offset_in_sector;
		tier->initialized = true;
		retVal = true;
	} while (0);

	return retVal;
}

bool EventLogTier_Write(EventLogTier_t *tier, EventLogRecord_t *record)
{
	bool retVal = false;

	do {
		if (tier == NULL || record == NULL || tier->initialized == false) {
			break;
		}

		record->checksum = EventLogTier_CalculateChecksum(record);

		const uint32_t total_sectors = tier->end_sector - tier->start_sector + 1u;
		const uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		const uint32_t write_index = EventLogTier_GetWriteIndex(tier, total_capacity);

		const uint32_t sector_index = write_index / EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		const uint32_t offset_in_sector = write_index % EVENT_LOG_RECORDS_PER_SECTOR_FLASH;
		const uint32_t target_sector = tier->start_sector + sector_index;
		const uint32_t record_address = SPIF_SectorToAddress(target_sector)
			+ (offset_in_sector * EVENT_LOG_RECORD_SIZE);

		if (offset_in_sector == 0u &&
		    (tier->total_records >= total_capacity ||
		     EventLogTier_SectorIsEmpty(tier, target_sector))) {
			if (SPIF_EraseSector(tier->spif_handle, target_sector) == false) {
				break;
			}
		}

		if (SPIF_WriteAddress(tier->spif_handle, record_address, (uint8_t *)record, EVENT_LOG_RECORD_SIZE) == false) {
			break;
		}

		tier->last_index = write_index;
		tier->current_sector = target_sector;
		tier->current_offset = offset_in_sector;
		tier->total_records++;
		if (tier->total_records > total_capacity) {
			tier->total_records = total_capacity;
		}

		retVal = true;
	} while (0);

	return retVal;
}

bool EventLogTier_Read(EventLogTier_t *tier, uint32_t index, EventLogRecord_t *record)
{
	bool retVal = false;

	do {
		if (tier == NULL || record == NULL || tier->initialized == false) {
			break;
		}

		const uint32_t total_capacity = EventLogTier_GetCapacityRecords(tier);
		if (index >= total_capacity) {
			break;
		}

		const uint32_t record_address = EventLogTier_GetAddressByIndex(tier, index);
		if (SPIF_ReadAddress(tier->spif_handle, record_address, (uint8_t *)record, EVENT_LOG_RECORD_SIZE) == false) {
			break;
		}

		retVal = true;
	} while (0);

	return retVal;
}
