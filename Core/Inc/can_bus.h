/**
  ******************************************************************************
  * @file    can_bus.h
  * @brief   Модуль приёма CAN по двум закольцованным шинам: кольцевой буфер,
  *          дедупликация и проверка дубликатов по каждому устройству (до 128).
  ******************************************************************************
  */

#ifndef CAN_BUS_H
#define CAN_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Максимальное число устройств на шине (по ним ведётся учёт дубликатов и ошибок) */
#define CAN_MAX_DEVICES  128

/** Номер шины: 1 = FDCAN1, 2 = FDCAN2 */
#define CAN_BUS_1  1
#define CAN_BUS_2  2

/**
  * Вычисление индекса устройства 0..(CAN_MAX_DEVICES-1) по идентификатору CAN.
  * При необходимости переопределите в проекте перед включением can_bus.h.
  */
#ifndef CAN_DEVICE_INDEX
#define CAN_DEVICE_INDEX(id)  ((uint8_t)((id) & 0x7F))
#endif

/**
  * Инициализация состояния модуля (последний пакет по устройствам). Можно вызвать явно при старте или положиться на первый CanProcess().
  */
void CanInit(void);

/**
  * Добавить принятый пакет в буфер (вызывать из прерывания HAL_FDCAN_RxFifo*Callback).
  * @param id    идентификатор CAN
  * @param data  указатель на 8 байт данных
  * @param can_bus  CAN_BUS_1 или CAN_BUS_2
  */
void CanRxPush(uint32_t id, const uint8_t *data, uint8_t can_bus);

/**
  * Обработка приёма: проверка таймаутов, разбор буфера, дедупликация по устройствам,
  * один вызов разбора протокола на уникальный пакет. Вызывать из основного цикла (например раз в 1 мс).
  */
void CanProcess(void);

/**
  * Обработка очередей отправки CAN (по одной независимой очереди на каждую шину).
  * Вызывать из основного цикла.
  */
void App_CanTxProcess(void);

/**
  * Точка отправки для backend (Buf: id + 8 data + bus mask).
  */
void CANSendData(uint8_t *Buf);

/** Глобальные флаги ошибки шин: бит 0 = CAN1 (нет приёма), бит 1 = CAN2 */
extern uint8_t can_bus_error_flags;

/**
  * Ошибки по устройствам: для каждого устройства байт — бит 0 = ожидаемый дубликат с CAN1 не пришёл,
  * бит 1 = ожидаемый дубликат с CAN2 не пришёл. Индекс = CAN_DEVICE_INDEX(can_id).
  */
extern uint8_t device_can_error[CAN_MAX_DEVICES];

#ifdef __cplusplus
}
#endif

#endif /* CAN_BUS_H */
