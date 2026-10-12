/*
 * beeper.h — на ППКУ нет локальной пищалки.
 * Mute (beep / beep_block) уходит на панель через SYS flags POLL.
 */

#ifndef INC_BEEPER_H_
#define INC_BEEPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void Beeper_Init(void);
void Beeper_SoundOnOff(bool soundOn);
/* GOST: unmute при новом извещении → PPKYConfig.beep → SYS_SOUND_ON. */
void Beeper_ResumeSoundOnNewEvent(void);
uint8_t Beeper_IsSoundEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_BEEPER_H_ */
