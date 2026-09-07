#ifndef FW_UPDATE_H_
#define FW_UPDATE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Boot_WriteProgramWatchDog(void);

uint8_t SetUpdateWord(uint32_t num, uint32_t word);
uint8_t GetUpdateWord(uint32_t num, uint32_t *word);
uint8_t FinishUpdateTransmit(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPDATE_H_ */
