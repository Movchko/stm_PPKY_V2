/*
 * fire.h
 *
 * Логика пожара/тушения в ППКУ: FSM, индикация, команды вниз.
 */

#ifndef INC_FIRE_H_
#define INC_FIRE_H_

#include <stdint.h>
#include <stdbool.h>

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
void Fire_OnStatusFire(uint32_t msg_id);
void Fire_OnReplyStatusFire(uint32_t msg_id);
void Fire_OnStopExtinguishment(uint32_t msg_id);
void Fire_OnReplyStartExtinguishment(uint32_t msg_id);
void Fire_OnReplyStopExtinguishment(uint32_t msg_id);
void Fire_OnPauseExtinguishmentTimer(uint32_t msg_id);
void Fire_OnResumeExtinguishmentTimer(uint32_t msg_id);
void Fire_OnReplyPauseExtinguishmentTimer(uint32_t msg_id);
void Fire_OnReplyResumeExtinguishmentTimer(uint32_t msg_id);

/* Текущий статус сценария пожара (для подавления вторичных индикаций). */
uint8_t Fire_IsActive(void);

/* Ручной выбор пожара на главном экране (индекс в текущем UI-списке). */
void Fire_UiSetManualSelection(uint8_t enabled, uint8_t selected_ui_index);

#ifdef __cplusplus
}
#endif

#endif /* INC_FIRE_H_ */

