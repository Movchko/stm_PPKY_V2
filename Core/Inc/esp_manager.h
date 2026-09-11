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
/** Ответ ESP_CMD (тип 3) с UART ESP: сборка версии по 159. */
void EspManager_OnEspCmd(const uint8_t *payload, uint16_t len);
/** Запрос версии ESP (команда 159). Пока без UI — вызывать при необходимости. */
void EspManager_RequestVersion(void);
/** Копия UTF-8 строки версии в out. Возвращает длину без нуля, 0 если ещё нет. */
uint8_t EspManager_GetVersion(char *out, uint8_t out_size);
uint8_t EspManager_IsVersionValid(void);
/** Повторно запросить включение WiFi (меню связи и т.п.). */
void EspManager_RequestWifiEnable(void);
/** Выключить WiFi в ESP (меню «Связь»). */
void EspManager_RequestWifiDisable(void);

uint8_t EspManager_IsOnline(void);
uint8_t EspManager_IsWifiEnabled(void);
uint8_t EspManager_IsHostConnected(void);
uint8_t EspManager_IsLinkActive(void);
/** Активна сессия WiFi (ожидание/подключение). */
uint8_t EspManager_IsUserWifiOn(void);
/** Видимость значка WiFi с учётом мигания (2 Гц) до TCP-подключения. */
uint8_t EspManager_IsWifiIconVisible(uint32_t now_ms);

uint8_t EspManager_IsWifiSessionActive(void);
#ifdef __cplusplus
}
#endif

#endif /* ESP_MANAGER_H */
