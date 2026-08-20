#ifndef INC_CONFIG_ZONE_BLOCK_H_
#define INC_CONFIG_ZONE_BLOCK_H_

#include <stdint.h>
#include "device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* zone_fire_mode: 0 авто, 1 автономный, 2 ручной, 3 заблокировано */
uint8_t PPKY_ZoneFireModeGet(uint8_t zone_idx);
void PPKY_ZoneFireModeSet(uint8_t zone_idx, uint8_t mode);

/* Эффективный режим зоны: GOST -> zone_fire_mode[]; иначе -> глобальный fire_mode. */
uint8_t PPKY_ZoneEffectiveMode(uint8_t zone_can);

/* Пуск заблокирован (effective == 3). zone_can: 0 -> служебная. */
uint8_t PPKY_ZoneLaunchBlockedByCanZone(uint8_t zone_can);

/* Ручной режим зоны (effective == 2). */
uint8_t PPKY_ZoneIsManualByCanZone(uint8_t zone_can);

/* Есть ли зона с ручным (2) или блокировкой (3) - для LED_AUTO_OFF в GOST. */
uint8_t PPKY_AnyZoneManualOrBlocked(void);

/* Заполнить zone_fire_mode[] значением fire_mode (DefaultConfig / миграция). */
void PPKY_ZoneFireModeInitFromGlobal(void);

/* UI: новое событие списка РЕЖИМ после смены в меню. */
void PPKY_ZoneModeUiNotify(uint8_t zone_idx);
uint8_t PPKY_ZoneModeUiConsumeNew(uint8_t *zone_idx_out);

#ifdef __cplusplus
}
#endif

#endif /* INC_CONFIG_ZONE_BLOCK_H_ */
