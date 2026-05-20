/*
 * event_logger.c
 *
 *  Created on: 2025
 *      Author: 79099
 */

#include "event_logger.h"
#include <string.h>

/***********************************************************************************************************/
/* Вспомогательные функции */
/***********************************************************************************************************/

/**
 * @brief Вычисление контрольной суммы для записи события (CRC16-CCITT упрощенный)
 * @param record Указатель на структуру события
 * @return Контрольная сумма (16-битное значение)
 */
uint16_t EventLogger_CalculateChecksum(EventLogRecord_t *record)
{
	uint16_t crc = 0xFFFF;
	uint8_t *data = (uint8_t *)record;
	uint32_t size = sizeof(EventLogRecord_t) - sizeof(record->checksum); // Все данные кроме контрольной суммы
	
	for (uint32_t i = 0; i < size; i++)
	{
		crc ^= (uint16_t)data[i] << 8;
		for (uint8_t j = 0; j < 8; j++)
		{
			if (crc & 0x8000)
			{
				crc = (crc << 1) ^ 0x1021;
			}
			else
			{
				crc = crc << 1;
			}
		}
	}
	return crc;
}

/**
 * @brief Проверка контрольной суммы записи события
 * @param record Указатель на структуру события
 * @return true если контрольная сумма правильная, false если нет
 */
bool EventLogger_VerifyChecksum(EventLogRecord_t *record)
{
	uint16_t calculated_checksum = EventLogger_CalculateChecksum(record);
	return (calculated_checksum == record->checksum);
}

/**
 * @brief Проверка, является ли запись пустой (все байты равны 0xFF)
 * @param record Указатель на структуру события
 * @return true если запись пустая, false если нет
 */
static bool EventLogger_IsRecordEmpty(EventLogRecord_t *record)
{
	uint8_t *data = (uint8_t *)record;
	for (uint32_t i = 0; i < sizeof(EventLogRecord_t); i++)
	{
		if (data[i] != 0xFF)
		{
			return false;
		}
	}
	return true;
}

/**
 * @brief Получение адреса события по индексу
 * @param logger Указатель на структуру логера
 * @param index Индекс события (относительно начала диапазона)
 * @return Адрес в Flash памяти
 */
static uint32_t EventLogger_GetAddressByIndex(EventLogger_t *logger, uint32_t index)
{
	// Вычисляем, в каком секторе находится событие
	uint32_t total_sectors = logger->end_sector - logger->start_sector + 1;
	uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR;
	uint32_t sector_index = (index % total_capacity) / EVENT_LOG_RECORDS_PER_SECTOR;
	uint32_t offset_in_sector = (index % total_capacity) % EVENT_LOG_RECORDS_PER_SECTOR;
	uint32_t sector = logger->start_sector + sector_index;
	
	return SPIF_SectorToAddress(sector) + (offset_in_sector * EVENT_LOG_RECORD_SIZE);
}

/***********************************************************************************************************/
/* Основные функции */
/***********************************************************************************************************/

/**
 * @brief Инициализация логера событий
 * @param logger Указатель на структуру логера
 * @param spif_handle Указатель на структуру Flash памяти
 * @param start_sector Начальный сектор для записи событий
 * @param end_sector Конечный сектор для записи событий (включительно)
 * @return true при успешной инициализации, false при ошибке
 */
bool EventLogger_Init(EventLogger_t *logger, SPIF_HandleTypeDef *spif_handle, uint32_t start_sector, uint32_t end_sector)
{
	bool retVal = false;
	
	do
	{
		// Проверка параметров
		if (logger == NULL || spif_handle == NULL)
		{
			break;
		}
		
		if (spif_handle->Inited == 0)
		{
			break; // Flash память не инициализирована
		}
		
		if (start_sector > end_sector)
		{
			break; // Неверный диапазон секторов
		}
		
		if (end_sector >= spif_handle->SectorCnt)
		{
			break; // Выход за границы памяти
		}
		
		// Инициализация структуры логера
		memset(logger, 0, sizeof(EventLogger_t));
		logger->spif_handle = spif_handle;
		logger->start_sector = start_sector;
		logger->end_sector = end_sector;
		logger->total_records = 0;
		logger->last_index = 0;
		logger->current_sector = start_sector;
		logger->current_offset = 0;
		
		// Сканирование всех секторов для подсчета событий
		EventLogRecord_t record;
		uint32_t last_valid_index = 0;
		uint32_t total_sectors = end_sector - start_sector + 1;
		uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR;
		uint32_t first_empty_index = total_capacity; // Индекс первого пустого места
		
		// Проходим по всем секторам
		for (uint32_t sector = start_sector; sector <= end_sector; sector++)
		{
			uint32_t sector_address = SPIF_SectorToAddress(sector);
			uint32_t sector_base_index = (sector - start_sector) * EVENT_LOG_RECORDS_PER_SECTOR;
			
			// Оптимизация: проверяем последнее событие в секторе
			uint32_t last_record_address = sector_address + ((EVENT_LOG_RECORDS_PER_SECTOR - 1) * EVENT_LOG_RECORD_SIZE);
			
			if (SPIF_ReadAddress(spif_handle, last_record_address, (uint8_t *)&record, EVENT_LOG_RECORD_SIZE) == true)
			{
				// Если последнее событие в секторе не пустое, значит весь сектор заполнен
				if (!EventLogger_IsRecordEmpty(&record))
				{
					// Весь сектор заполнен
					logger->total_records += EVENT_LOG_RECORDS_PER_SECTOR;
					last_valid_index = sector_base_index + EVENT_LOG_RECORDS_PER_SECTOR - 1;
					continue; // Переходим к следующему сектору
				}
			}
			
			// Последнее событие пустое - ищем первое пустое место в секторе
			for (uint32_t record_offset = 0; record_offset < EVENT_LOG_RECORDS_PER_SECTOR; record_offset++)
			{
				uint32_t record_address = sector_address + (record_offset * EVENT_LOG_RECORD_SIZE);
				uint32_t current_index = sector_base_index + record_offset;
				
				// Чтение записи из Flash
				if (SPIF_ReadAddress(spif_handle, record_address, (uint8_t *)&record, EVENT_LOG_RECORD_SIZE) == false)
				{
					// Ошибка чтения - считаем запись пустой
					if (first_empty_index == total_capacity)
					{
						first_empty_index = current_index;
					}
					break;
				}
				
				// Проверка, является ли запись пустой
				if (EventLogger_IsRecordEmpty(&record))
				{
					// Найдено первое пустое место
					if (first_empty_index == total_capacity)
					{
						first_empty_index = current_index;
					}
					break; // Выходим из цикла по сектору
				}
				
				// Запись не пустая - считаем её
				logger->total_records++;
				last_valid_index = current_index;
			}
		}
		
		// Установка позиции для следующей записи
		if (first_empty_index < total_capacity)
		{
			// Найдено пустое место - следующая запись туда
			logger->last_index = (first_empty_index > 0) ? (first_empty_index - 1) : 0;
		}
		else if (logger->total_records > 0)
		{
			// Все сектора заполнены - следующая запись после последней (циклический буфер)
			logger->last_index = last_valid_index;
		}
		else
		{
			// Нет записей - начинаем с начала
			logger->last_index = 0;
		}
		
		// Вычисляем позицию для следующей записи
		uint32_t next_index;
		if (first_empty_index < total_capacity)
		{
			// Найдено пустое место - записываем туда
			next_index = first_empty_index;
		}
		else if (logger->total_records > 0)
		{
			// Все заполнено - циклический буфер, следующая запись после последней
			next_index = (logger->last_index + 1) % total_capacity;
		}
		else
		{
			// Нет записей - начинаем с начала
			next_index = 0;
		}
		
		uint32_t sector_index = next_index / EVENT_LOG_RECORDS_PER_SECTOR;
		uint32_t offset_in_sector = next_index % EVENT_LOG_RECORDS_PER_SECTOR;
		logger->current_sector = start_sector + sector_index;
		logger->current_offset = offset_in_sector;
		
		logger->initialized = true;
		retVal = true;
		
	} while (0);
	
	return retVal;
}

/**
 * @brief Запись события в логер
 * @param logger Указатель на структуру логера
 * @param record Указатель на структуру события для записи
 * @return true при успешной записи, false при ошибке
 */
bool EventLogger_WriteEvent(EventLogger_t *logger, EventLogRecord_t *record)
{
	bool retVal = false;
	
	do
	{
		// Проверка параметров
		if (logger == NULL || record == NULL)
		{
			break;
		}
		
		if (logger->initialized == false)
		{
			break; // Логер не инициализирован
		}
		
		// Вычисление контрольной суммы
		record->checksum = EventLogger_CalculateChecksum(record);
		
		// Вычисление адреса для записи
		uint32_t total_sectors = logger->end_sector - logger->start_sector + 1;
		uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR;
		
		// Определяем индекс следующей записи
		uint32_t write_index;
		if (logger->total_records == 0)
		{
			// Еще нет записей - начинаем с индекса 0
			write_index = 0;
		}
		else
		{
			// Следующая запись после последней
			write_index = (logger->last_index + 1) % total_capacity;
		}
		
		uint32_t sector_index = write_index / EVENT_LOG_RECORDS_PER_SECTOR;
		uint32_t offset_in_sector = write_index % EVENT_LOG_RECORDS_PER_SECTOR;
		uint32_t target_sector = logger->start_sector + sector_index;
		uint32_t record_address = SPIF_SectorToAddress(target_sector) + (offset_in_sector * EVENT_LOG_RECORD_SIZE);
		
		// Проверка, нужно ли очистить сектор (если начинаем новый сектор)
		if (offset_in_sector == 0)
		{
			// Очищаем сектор перед записью
			if (SPIF_EraseSector(logger->spif_handle, target_sector) == false)
			{
				break; // Ошибка очистки сектора
			}
		}
		
		// Запись события в Flash
		if (SPIF_WriteAddress(logger->spif_handle, record_address, (uint8_t *)record, EVENT_LOG_RECORD_SIZE) == false)
		{
			break; // Ошибка записи
		}
		
		// Обновление состояния логера
		logger->last_index = write_index;
		logger->current_sector = target_sector;
		logger->current_offset = offset_in_sector;
		logger->total_records++;
		
		// Ограничение общего количества записей (для предотвращения переполнения)
		if (logger->total_records > total_capacity)
		{
			logger->total_records = total_capacity;
		}
		
		retVal = true;
		
	} while (0);
	
	return retVal;
}

/**
 * @brief Чтение события из логера по индексу
 * @param logger Указатель на структуру логера
 * @param index Индекс события (0 - первое событие)
 * @param record Указатель на буфер для чтения события
 * @return true при успешном чтении, false при ошибке
 */
bool EventLogger_ReadEvent(EventLogger_t *logger, uint32_t index, EventLogRecord_t *record)
{
	bool retVal = false;
	
	do
	{
		// Проверка параметров
		if (logger == NULL || record == NULL)
		{
			break;
		}
		
		if (logger->initialized == false)
		{
			break; // Логер не инициализирован
		}
		
		uint32_t total_sectors = logger->end_sector - logger->start_sector + 1;
		uint32_t total_capacity = total_sectors * EVENT_LOG_RECORDS_PER_SECTOR;
		
		if (index >= total_capacity)
		{
			break; // Выход за границы
		}
		
		// Вычисление адреса события
		uint32_t record_address = EventLogger_GetAddressByIndex(logger, index);
		
		// Чтение события из Flash
		if (SPIF_ReadAddress(logger->spif_handle, record_address, (uint8_t *)record, EVENT_LOG_RECORD_SIZE) == false)
		{
			break; // Ошибка чтения
		}
		
		retVal = true;
		
	} while (0);
	
	return retVal;
}
