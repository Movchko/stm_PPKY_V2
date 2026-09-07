/*
 * app.hpp
 *
 *  Created on: Nov 20, 2025
 *      Author: 79099
 */

#ifndef INC_APP_HPP_
#define INC_APP_HPP_

#include "main.h"
#include "spif.h"
#include "device_config.h"
#include "boot_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_CFG_START_SECTOR 0
#define FLASH_CFG_START_BLOCK FLASH_CFG_START_SECTOR / (SPIF_BLOCK_SIZE / SPIF_SECTOR_SIZE)

#define FLASH_CFG_SECTORS_USED  24
#define FLASH_CFG_BLOCK_USED  FLASH_CFG_SECTORS_USED / (SPIF_BLOCK_SIZE / SPIF_SECTOR_SIZE)
#define NUM_ACTIVE_DEVICE 32

/* Область SPI Flash под конфигурацию ППКУ (секторы 0 .. FLASH_CFG_SECTORS_USED-1). */
#define FLASH_CFG_BYTES_ALLOCATED   (FLASH_CFG_SECTORS_USED * SPIF_SECTOR_SIZE)
#define FLASH_CFG_RESERVE_PERCENT   10u
#define FLASH_CFG_MAX_USABLE_BYTES  ((FLASH_CFG_BYTES_ALLOCATED * (100u - FLASH_CFG_RESERVE_PERCENT)) / 100u)
#define FLASH_CFG_STORED_BYTES      (sizeof(PPKYConfigHeader) + sizeof(PPKYCfg))

/* Область журналов после конфигурации, factory 0x020000 и update 0x0A0000 (слот 512 КБ, см. boot_layout.h). */
#define FLASH_LOG_TOTAL_SECTORS     (SPI_FLASH_SECTOR_COUNT - FLASH_LOG_START_SECTOR)

#define EVENT_LOG_RECORD_SIZE_BYTES   32u
#define EVENT_LOG_RECORDS_PER_SECTOR  (SPIF_SECTOR_SIZE / EVENT_LOG_RECORD_SIZE_BYTES)

#define FLASH_LOG_CRITICAL_MIN_RECORDS   50000u
#define FLASH_LOG_CRITICAL_SECTORS       400u
#define FLASH_LOG_CRITICAL_RECORDS       (FLASH_LOG_CRITICAL_SECTORS * EVENT_LOG_RECORDS_PER_SECTOR)

#define FLASH_LOG_CRITICAL_START_SECTOR  FLASH_LOG_START_SECTOR
#define FLASH_LOG_CRITICAL_END_SECTOR    (FLASH_LOG_CRITICAL_START_SECTOR + FLASH_LOG_CRITICAL_SECTORS - 1u)

#define FLASH_LOG_GENERAL_SECTORS        (FLASH_LOG_TOTAL_SECTORS - FLASH_LOG_CRITICAL_SECTORS)
#define FLASH_LOG_GENERAL_START_SECTOR   (FLASH_LOG_CRITICAL_END_SECTOR + 1u)
#define FLASH_LOG_GENERAL_END_SECTOR     (FLASH_LOG_GENERAL_START_SECTOR + FLASH_LOG_GENERAL_SECTORS - 1u)
#define FLASH_LOG_GENERAL_RECORDS        (FLASH_LOG_GENERAL_SECTORS * EVENT_LOG_RECORDS_PER_SECTOR)

#define FLASH_LOG_UNUSED_TAIL_SECTORS    0u

#define RTC_PING_PERIOD_S 60000u

void FillConfigTemplate(void);
void ReadSavedConfig(void);

/* Чтение содержимого BKP-регистра RTC с моментом последнего сохранения
 * (месяц/день/часы/минуты). Поля возвращаются в формате RTC (BCD),
 * как в HAL_RTC_GetDate / HAL_RTC_GetTime.
 * Любой из указателей может быть NULL, если часть данных не нужна. */
void PPKY_GetLastPowerOnDate(RTC_DateTypeDef *out_date, RTC_TimeTypeDef *out_time);

#define PPKY_MAX_ACTIVE_VDEVS_PER_MCU 16

typedef struct {
	Device dev;
	uint32_t last_seen_ms;
	uint8_t online;
	uint8_t can_status_mask; /* маска активности CAN (из статуса МКУ cmd=0) */
	uint8_t can_state_mask;  /* bits[1:0]=CAN0 state, bits[3:2]=CAN1 state */
	uint8_t can_status_valid; /* 1 после первого валидного статуса МКУ cmd=0 */
	uint8_t u24_01v;         /* измеренное U24 (1V), из статуса МКУ cmd=0 */
	uint8_t mcu_status_data[8]; /* последний полный статус МКУ (cmd=0), MsgData[0..7] */

	/* Виртуальные устройства, которые находятся "внутри" данного МКУ */
	uint8_t vdev_count;

	struct s_active_vdev {
		uint32_t last_seen_ms;
		uint8_t online;

		uint8_t v_d_type; /* DEVICE_* виртуального устройства */
		uint8_t v_l_adr;  /* виртуальный номер (l_adr) */

		/* raw статус, как приходит в CAN payload */
		uint8_t status_cmd;    /* MsgData[0] */
		uint8_t status_params[7]; /* MsgData[1..7] */

		uint8_t prev_status_cmd;   /* предыдущий статус (до последнего обновления) */
		uint8_t status_changed;    /* 1 если статус изменился с прошлого обновления */

		/* часто используемые декодированные поля (для удобства отладки) */
		uint8_t line_state;     /* для DPT/IGNITER */
		uint16_t resistance_ohm; /* для DPT/Button/LSwitch */
		uint16_t igniter_resistance_ohm; /* для IGNITER */
		int16_t max_temp_c;    /* для DPT/Button/LSwitch (термопара MAX) */
		int16_t max_internal_temp_c; /* для DPT/Button/LSwitch (внутренняя MAX) */
		uint8_t max_fault_mask;     /* для DPT/Button/LSwitch (битовая маска MAX) */
		uint8_t ack_flags;     /* для IGNITER */
	} vdevs[PPKY_MAX_ACTIVE_VDEVS_PER_MCU];
} ActiveDeviceInfo;

void RelayAuto_NotifyStartExtinguish(uint8_t zone_can);

#ifdef __cplusplus
}
#endif

#endif /* INC_APP_HPP_ */
