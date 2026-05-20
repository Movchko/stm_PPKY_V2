/*
 * event_logger.h
 *
 *  Created on: 2025
 *      Author: 79099
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

/***********************************************************************************************************/
/* Константы */
/***********************************************************************************************************/

#define EVENT_LOG_RECORD_SIZE      32      // Размер записи события в байтах
#define EVENT_LOG_SECTOR_SIZE      SPIF_SECTOR_SIZE  // Размер сектора Flash
#define EVENT_LOG_RECORDS_PER_SECTOR  (EVENT_LOG_SECTOR_SIZE / EVENT_LOG_RECORD_SIZE)  // Количество событий в секторе (128)

/***********************************************************************************************************/
/* Структура записи события (32 байта) */
/***********************************************************************************************************/

typedef struct
{
	uint8_t  time[6];          // Время события в формате BCD (6 байт)
	uint8_t  master_wagon_num; // Номер мастера (шины) или номер вагона в составе (1 байт)
	uint8_t  reserved;         // Резерв (1 байт)
	uint16_t event_code;       // Код события (2 байта)
	uint32_t can_header;       // CAN заголовок (4 байта)
	uint8_t  can_data[8];      // Данные из CAN сообщения (8 байт)
	uint8_t  additional[8];    // Дополнительная информация к событию (8 байт)
	uint16_t checksum;         // Контрольная сумма (2 байта)
} EventLogRecord_t;

/***********************************************************************************************************/
/* Структура логера событий */
/***********************************************************************************************************/

typedef struct
{
	SPIF_HandleTypeDef *spif_handle;  // Указатель на структуру Flash памяти
	uint32_t            start_sector;  // Начальный сектор для записи событий
	uint32_t            end_sector;    // Конечный сектор для записи событий
	uint32_t            total_records; // Общее количество записанных событий
	uint32_t            last_index;    // Индекс последнего записанного события (относительно начала диапазона)
	uint32_t            current_sector;// Текущий сектор для записи
	uint32_t            current_offset;// Смещение в текущем секторе (в записях)
	bool                initialized;   // Флаг инициализации
} EventLogger_t;

/***********************************************************************************************************/
/* Прототипы функций */
/***********************************************************************************************************/

/**
 * @brief Инициализация логера событий
 * @param logger Указатель на структуру логера
 * @param spif_handle Указатель на структуру Flash памяти
 * @param start_sector Начальный сектор для записи событий
 * @param end_sector Конечный сектор для записи событий (включительно)
 * @return true при успешной инициализации, false при ошибке
 */
bool EventLogger_Init(EventLogger_t *logger, SPIF_HandleTypeDef *spif_handle, uint32_t start_sector, uint32_t end_sector);

/**
 * @brief Запись события в логер
 * @param logger Указатель на структуру логера
 * @param record Указатель на структуру события для записи
 * @return true при успешной записи, false при ошибке
 */
bool EventLogger_WriteEvent(EventLogger_t *logger, EventLogRecord_t *record);

/**
 * @brief Чтение события из логера по индексу
 * @param logger Указатель на структуру логера
 * @param index Индекс события (0 - первое событие)
 * @param record Указатель на буфер для чтения события
 * @return true при успешном чтении, false при ошибке
 */
bool EventLogger_ReadEvent(EventLogger_t *logger, uint32_t index, EventLogRecord_t *record);

/**
 * @brief Вычисление контрольной суммы для записи события
 * @param record Указатель на структуру события
 * @return Контрольная сумма (16-битное значение)
 */
uint16_t EventLogger_CalculateChecksum(EventLogRecord_t *record);

/**
 * @brief Проверка контрольной суммы записи события
 * @param record Указатель на структуру события
 * @return true если контрольная сумма правильная, false если нет
 */
bool EventLogger_VerifyChecksum(EventLogRecord_t *record);

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOGGER_H_ */
