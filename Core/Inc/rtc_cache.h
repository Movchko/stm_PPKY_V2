/*
 * rtc_cache.h
 *
 * Кэш даты/времени RTC: HAL_RTC_GetTime/GetDate не чаще 1 раза в секунду.
 */

#ifndef INC_RTC_CACHE_H_
#define INC_RTC_CACHE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	RTC_TimeTypeDef time_bcd;
	RTC_DateTypeDef date_bcd;
	RTC_TimeTypeDef time_bin;
	RTC_DateTypeDef date_bin;
	uint8_t         valid;
} RtcCache_t;

extern RtcCache_t g_rtc_cache;

void RtcCache_Tick1s(void);
void RtcCache_Refresh(void);

bool RtcCache_IsValid(void);
const RtcCache_t *RtcCache_Get(void);

bool RtcCache_GetBcd(RTC_TimeTypeDef *time, RTC_DateTypeDef *date);
bool RtcCache_GetBin(RTC_TimeTypeDef *time, RTC_DateTypeDef *date);

#ifdef __cplusplus
}
#endif

#endif /* INC_RTC_CACHE_H_ */
