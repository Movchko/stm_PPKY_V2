#ifndef INC_CONFIG_ZONE_BLOCK_H_
#define INC_CONFIG_ZONE_BLOCK_H_

#include <stdint.h>
#include "device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zone_fire_mode:
 * 0 авто, 1 автономный, 2 ручной, 3 блок пуска, 4 блок пожара */
uint8_t PPKY_ZoneFireModeGet(uint8_t zone_idx);
void PPKY_ZoneFireModeSet(uint8_t zone_idx, uint8_t mode);

/* Эффективный режим зоны: всегда zone_fire_mode[]. zone_can==0 → 0 (авто). */
uint8_t PPKY_ZoneEffectiveMode(uint8_t zone_can);

/* Пуск запрещён (mode 3 или 4). zone_can==0 → нет. */
uint8_t PPKY_ZoneLaunchBlockedByCanZone(uint8_t zone_can);

/* Пожар игнорируется (mode 4): лог+ACK, без сценария. */
uint8_t PPKY_ZoneFireBlockedByCanZone(uint8_t zone_can);

/* Ручной режим зоны (effective == 2). */
uint8_t PPKY_ZoneIsManualByCanZone(uint8_t zone_can);

/* Именованная зона с mode 2/3/4 — LED_AUTO_OFF / баннер РЕЖИМ. */
uint8_t PPKY_AnyZoneManualOrBlocked(void);

/* Миграция Flash: заполнить zone_fire_mode[] из legacy fire_mode. */
void PPKY_ZoneFireModeInitFromGlobal(void);

/* CAN cmd13 / концевик: только 0/1/2 во все именованные зоны (не 3/4). */
void PPKY_ZoneFireModeSetAllNamed(uint8_t mode);

/* UI: новое событие списка РЕЖИМ после смены в меню. */
void PPKY_ZoneModeUiNotify(uint8_t zone_idx);
uint8_t PPKY_ZoneModeUiConsumeNew(uint8_t *zone_idx_out);

#ifdef __cplusplus
}
#endif

#endif /* INC_CONFIG_ZONE_BLOCK_H_ */
