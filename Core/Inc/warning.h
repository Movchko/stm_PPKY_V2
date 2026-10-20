#ifndef INC_WARNING_H_
#define INC_WARNING_H_

#include <stdint.h>
#include "rs_panel_protocol_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Обработка предупреждений/неисправностей виртуальных устройств. */
void WarningProcess1ms(void);
/* Ошибки выходов модуля power: bits0..1 => ВЫХОД1/2 (МКУ), bit2 => панель */
void Warning_SetPowerFaultMask(uint8_t mask);
/* Тип неисправности входа питания ППКУ (для ПИТ1/ПИТ2). */
#define PPKU_INPUT_FAULT_OK    0u /* норма */
#define PPKU_INPUT_FAULT_OPEN  1u /* ОБРЫВ — питания нет */
#define PPKU_INPUT_FAULT_LOW   2u /* НИЖЕ нормы */
#define PPKU_INPUT_FAULT_HIGH  3u /* ВЫШЕ нормы */
/* kinds: ch1/ch2 = PPKU_INPUT_FAULT_*; 0 = норма (бита маски нет).
 * mv_ch* / required_mv — мВ для записи в журнал (факт / номинал). */
void Warning_SetPpkuInputFaultKinds(uint8_t kind_ch1, uint8_t kind_ch2,
				    uint16_t mv_ch1, uint16_t mv_ch2,
				    uint16_t required_mv);
/* Текущая (после debounce) маска входов питания — для SYS POLL v3 / LED_POWER. */
uint8_t Warning_GetPpkuInputFaultMask(void);
/* Текущий тип неисправности входа (ch 0..1), 0 если нет ошибки. */
uint8_t Warning_GetPpkuInputFaultKind(uint8_t ch);
/* Ошибки несоответствия физической позиции МКУ (бит h_adr-1 => МКУ с адресом h_adr). */
void Warning_SetMkuPositionFaultMask(uint32_t mask);
/* Ошибка доставки журнала на панели (бит panel_addr-1 => панель addr 1..8). */
void Warning_SetPanelJournalFaultMask(uint8_t mask);
uint8_t Warning_HasActiveFault(void);
uint8_t Warning_HasActiveAttention(void);
/* Принудительно переотправить текущий список неисправностей на панели (после READY). */
void Warning_RepublishUiNow(void);
/* Сбросить кэш PushUiIfChanged (панель переподключилась / не приняла снимок). */
void Warning_ResetPanelUiCache(void);
/* Последний count из BuildUiPayload (Live Watch на ППКУ). */
uint8_t Warning_GetLastUiBuildCount(void);
/* Стартовые паузы (wall-time, HAL_GetTick):
 * — process ~10 с: затем WarningProcess/Commit (каталог FAULT можно слать);
 * — SYS_READY дольше: сначала неисправности, потом снятие «ПРОВЕРКА». */
void Warning_ArmProcessDelay(uint32_t now_ms);
/* 1, пока WarningProcess ещё не крутится. */
uint8_t Warning_IsProcessDelayActive(void);
/* 1, пока рано ставить SYS_READY на панель (дольше process). */
uint8_t Warning_IsSysReadyDelayActive(void);
/* 1 после первого полного WarningProcess после process-паузы (снимок Commit). */
uint8_t Warning_IsFaultPipelinePrimed(void);
/* Снимок активных неисправностей v3 (коды + ключи) для RsPanelV3Master_SetFaultSnapshot. */
uint16_t Warning_BuildV3FaultSnapshot(RsPanelV3FaultEvtItem *out, uint16_t max_out);

#ifdef __cplusplus
}
#endif

#endif /* INC_WARNING_H_ */

