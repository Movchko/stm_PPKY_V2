/*
 * fire.h
 *
 * Логика пожара/тушения в ППКУ: FSM, индикация, команды вниз.
 */

#ifndef INC_FIRE_H_
#define INC_FIRE_H_

#include <stdint.h>
#include <stdbool.h>
#include "rs_panel_protocol_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Инициализация FSM пожара (вызывается из AppInit) */
void Fire_Init(void);

/* Обработка 1 мс тиков (вызывать из AppTimer1ms) */
void Fire_Timer1ms(void);

/* Обработка 10 мс тиков (кнопки/бипер/LED) – вызывать из AppTimer10ms */
void Fire_Timer10ms(void);

/* События от протокола backend (вызывать из ListenerCommandCB) */
void Fire_OnStatusFire(uint32_t msg_id, const uint8_t *msg_data);
void Fire_OnReplyStatusFire(uint32_t msg_id);
void Fire_OnStopExtinguishment(uint32_t msg_id);
void Fire_OnBusStartSpButton(uint32_t msg_id);
void Fire_OnStartExtinguishment(uint32_t msg_id, const uint8_t *msg_data);
void Fire_OnReplyStartExtinguishment(uint32_t msg_id);
void Fire_OnReplyStopExtinguishment(uint32_t msg_id);
void Fire_OnPauseExtinguishmentTimer(uint32_t msg_id);
void Fire_OnResumeExtinguishmentTimer(uint32_t msg_id);
void Fire_OnReplyPauseExtinguishmentTimer(uint32_t msg_id);
void Fire_OnReplyResumeExtinguishmentTimer(uint32_t msg_id);

/* Текущий статус сценария пожара (для подавления вторичных индикаций). */
uint8_t Fire_IsActive(void);

/* Есть активный слот с неуспешным тушением (ТУШ.ОШ.) - для LED_ERR. */
uint8_t Fire_HasExtinguishIncomplete(void);

/* Идёт удержание ПУСК ОБЩИЙ (3с) - нужен главный экран со счётчиком. */
uint8_t Fire_IsStartAllHoldActive(void);

/* Активен звук/индикация тушения (ПУСК непрерывно на панели). */
uint8_t Fire_IsExtinguishIndicationActive(void);

/* Режим обобщённого LED_FIRE для панели:
 * 0 = OFF, 1 = ПОЖАР1 (непрерывно), 2 = ПОЖАР2 (мигание), 3 = ВНИМАНИЕ (мигание). */
uint8_t Fire_GetPanelFireLedMode(void);

/* Ручной выбор пожара на главном экране (индекс в текущем UI-списке). */
void Fire_UiSetManualSelection(uint8_t enabled, uint8_t selected_ui_index);
uint8_t Fire_UiGetSelectedIndex(void);

/* Смена zone_fire_mode[] (меню РЕЖИМ ЗОН) - обновить LED_AUTO_OFF. */
void Fire_NotifyZoneModeChanged(void);

/* Сбросить кэш UI пожара; следующий Fire_Timer10ms переотправит на панели. */
void Fire_ForceUiResync(void);

/* События с панели (протокол v3): zone=0 — все зоны. */
void Fire_OnPanelStartAllCommit(void);
void Fire_OnPanelStartSp(uint8_t zone);
void Fire_OnPanelStopLaunch(uint8_t zone);
void Fire_OnPanelFireReset(uint8_t zone);

/* Снимок зон для POLL v3 (ZONES). */
void Fire_FillV3Zones(RsPanelV3Zones *out, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* INC_FIRE_H_ */

