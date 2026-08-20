#ifndef ESP_MANAGER_H
#define ESP_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void EspManager_Init(void);
void EspManager_Process(uint32_t now_ms);

void EspManager_OnEspPoweredOn(void);
void EspManager_OnEspPoweredOff(void);
void EspManager_OnActivity(const uint8_t *payload, uint16_t len);
void EspManager_RequestWifiEnable(void);

uint8_t EspManager_IsOnline(void);
uint8_t EspManager_IsWifiEnabled(void);
uint8_t EspManager_IsHostConnected(void);
uint8_t EspManager_IsLinkActive(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_MANAGER_H */
