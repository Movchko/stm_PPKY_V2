#ifndef FW_UPDATE_H_
#define FW_UPDATE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FW_UPDATE_LOG_MUTE_MS
#define FW_UPDATE_LOG_MUTE_MS  10000u
#endif

void Boot_WriteProgramWatchDog(void);

uint8_t SetUpdateWord(uint32_t num, uint32_t word);
uint8_t GetUpdateWord(uint32_t num, uint32_t *word);
uint8_t FinishUpdateTransmit(void);

/** 1 — идёт запись UPDATE во внешний SPI Flash: EventLog не пишет (общая шина/чип).
 *  Сбрасывается, если пакетов SetUpdateWord не было FW_UPDATE_LOG_MUTE_MS. */
uint8_t FwUpdate_IsEventLogMuted(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_UPDATE_H_ */
