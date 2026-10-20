/*
 * beeper.h — на ППКУ нет локальной пищалки.
 * Mute (beep-маска / beep_block) уходит на панель через SYS flags POLL.
 * PPKYConfig.beep: бит N ↔ RS-адрес панели N (uint8 → адреса 0..7).
 */

#ifndef INC_BEEPER_H_
#define INC_BEEPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void Beeper_Init(void);
/* Глобально: on → маска 0xFF, off → 0. */
void Beeper_SoundOnOff(bool soundOn);
/* Бит панели по RS-адресу (SOUND_SET с панели). */
void Beeper_SetPanelSound(uint8_t addr, uint8_t on);
uint8_t Beeper_IsPanelSoundOn(uint8_t addr);
/* GOST: unmute при новом извещении → маска 0xFF (даже если снят один бит панели). */
void Beeper_ResumeSoundOnNewEvent(void);
/* 1 если маска ≠ 0. */
uint8_t Beeper_IsSoundEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_BEEPER_H_ */
