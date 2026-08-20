#include "rtc_cache.h"
#include "stm32h5xx_hal_rtc.h"

RtcCache_t g_rtc_cache;

static uint8_t RtcCache_Bcd2Bin(uint8_t bcd)
{
	return (uint8_t)(((bcd >> 4) * 10u) + (bcd & 0x0Fu));
}

static void RtcCache_FillBin(void)
{
	g_rtc_cache.time_bin.Hours = RtcCache_Bcd2Bin(g_rtc_cache.time_bcd.Hours);
	g_rtc_cache.time_bin.Minutes = RtcCache_Bcd2Bin(g_rtc_cache.time_bcd.Minutes);
	g_rtc_cache.time_bin.Seconds = RtcCache_Bcd2Bin(g_rtc_cache.time_bcd.Seconds);
	g_rtc_cache.time_bin.SubSeconds = g_rtc_cache.time_bcd.SubSeconds;
	g_rtc_cache.time_bin.DayLightSaving = g_rtc_cache.time_bcd.DayLightSaving;
	g_rtc_cache.time_bin.StoreOperation = g_rtc_cache.time_bcd.StoreOperation;
	g_rtc_cache.date_bin.Year = RtcCache_Bcd2Bin(g_rtc_cache.date_bcd.Year);
	g_rtc_cache.date_bin.Month = RtcCache_Bcd2Bin(g_rtc_cache.date_bcd.Month);
	g_rtc_cache.date_bin.Date = RtcCache_Bcd2Bin(g_rtc_cache.date_bcd.Date);
	g_rtc_cache.date_bin.WeekDay = g_rtc_cache.date_bcd.WeekDay;
}

void RtcCache_Refresh(void)
{
	if (HAL_RTC_GetTime(&hrtc, &g_rtc_cache.time_bcd, RTC_FORMAT_BCD) != HAL_OK) {
		g_rtc_cache.valid = 0u;
		return;
	}
	if (HAL_RTC_GetDate(&hrtc, &g_rtc_cache.date_bcd, RTC_FORMAT_BCD) != HAL_OK) {
		g_rtc_cache.valid = 0u;
		return;
	}

	RtcCache_FillBin();
	g_rtc_cache.valid = 1u;
}

void RtcCache_Tick1s(void)
{
	RtcCache_Refresh();
}

bool RtcCache_IsValid(void)
{
	return g_rtc_cache.valid != 0u;
}

const RtcCache_t *RtcCache_Get(void)
{
	return &g_rtc_cache;
}

bool RtcCache_GetBcd(RTC_TimeTypeDef *time, RTC_DateTypeDef *date)
{
	if (!RtcCache_IsValid()) {
		return false;
	}
	if (time != NULL) {
		*time = g_rtc_cache.time_bcd;
	}
	if (date != NULL) {
		*date = g_rtc_cache.date_bcd;
	}
	return true;
}

bool RtcCache_GetBin(RTC_TimeTypeDef *time, RTC_DateTypeDef *date)
{
	if (!RtcCache_IsValid()) {
		return false;
	}
	if (time != NULL) {
		*time = g_rtc_cache.time_bin;
	}
	if (date != NULL) {
		*date = g_rtc_cache.date_bin;
	}
	return true;
}
