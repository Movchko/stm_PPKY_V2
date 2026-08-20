#ifndef INC_CONFIG_IGN_BLOCK_SYNC_H_
#define INC_CONFIG_IGN_BLOCK_SYNC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ConfigIgnBlockSync_Init(void);
void ConfigIgnBlockSync_Process1ms(uint32_t now_ms);
void ConfigIgnBlockSync_Request(void);
/* 1 = пока нельзя латчить CONFIG_MISMATCH (ждём/идёт boot IgnBlockSync). */
uint8_t ConfigIgnBlockSync_ShouldDeferCrc(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_CONFIG_IGN_BLOCK_SYNC_H_ */
