#ifndef INC_CONFIG_MONITOR_H_
#define INC_CONFIG_MONITOR_H_

#include <stdint.h>
#include "device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void ConfigMonitor_Init(uint32_t boot_ms);
void ConfigMonitor_Process1ms(uint32_t now_ms);

uint8_t ConfigMonitor_IsMcuMissingLatched(uint8_t cfg_slot);
uint8_t ConfigMonitor_IsCrcFaultLatched(uint8_t cfg_slot);
uint8_t ConfigMonitor_GetFoundLatchedCount(void);
uint8_t ConfigMonitor_GetFoundLatchedKey(uint8_t index, Device *mcu_out,
					 uint8_t *v_l_adr, uint8_t *v_d_type);
const Device *ConfigMonitor_GetCfgDevice(uint8_t cfg_slot);

void ConfigMonitor_OnPeriodicCrcResult(uint8_t slot, uint8_t crc_match, uint8_t comm_failed);
void ConfigMonitor_OnRemoteUidRead(const Device *dev, const uint8_t *uid_bytes, uint8_t ok);
const uint8_t *ConfigMonitor_GetRemoteSerial(const Device *dev, uint8_t *out_valid);

#ifdef __cplusplus
}
#endif

#endif /* INC_CONFIG_MONITOR_H_ */
