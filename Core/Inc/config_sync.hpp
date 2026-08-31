#ifndef INC_CONFIG_SYNC_HPP_
#define INC_CONFIG_SYNC_HPP_

#include "app.hpp"
#include "device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void ConfigSync_Init(PPKYCfg *cfg,
                     ActiveDeviceInfo *active_devices,
                     uint8_t *active_devices_count,
                     void (*save_config_cb)(void),
                     void (*apply_success_cb)(void),
                     uint8_t *mismatch_flag_ptr);

void ConfigSync_Process1ms(uint32_t now_ms);
void ConfigSync_OnListenerMessage(uint32_t msg_id, const uint8_t *msg_data);

void ConfigSync_StartReadAllAndSave(void);
void ConfigSync_StartVerify(void);
void ConfigSync_StartApply(void);
void ConfigSync_StartPeriodicVerifySlot(uint8_t slot);
void ConfigSync_StartReadMcuUid(const Device *dev);
void ConfigSync_StartIgnBlockSync(void);

uint8_t ConfigSync_IsBusy(void);
/** Прогресс APPLY ко всем МКУ, 0..100. Вне APPLY — 0. */
uint8_t ConfigSync_GetApplyPercent(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_CONFIG_SYNC_HPP_ */

