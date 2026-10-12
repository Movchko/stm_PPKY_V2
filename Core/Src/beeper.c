/*
 * beeper.c — mute-слой для SYS POLL (локального HW нет).
 */

#include "beeper.h"
#include "device_config.h"
#include "event_log.h"
#include "gost_mode.h"

extern PPKYCfg PPKYConfig;

static uint8_t beep_sound = 1u;

void Beeper_Init(void)
{
	beep_sound = (PPKYConfig.beep != 0u) ? 1u : 0u;
}

void Beeper_SoundOnOff(bool soundOn)
{
	beep_sound = soundOn ? 1u : 0u;
	PPKYConfig.beep = beep_sound;
}

uint8_t Beeper_IsSoundEnabled(void)
{
	return beep_sound;
}

void Beeper_ResumeSoundOnNewEvent(void)
{
#if GOST_MODE
	if (beep_sound != 0u) {
		return;
	}
	if (PPKYConfig.beep_block != 0u) {
		return;
	}
	beep_sound = 1u;
	PPKYConfig.beep = 1u;
	EventLog_LogSoundToggle(1u, 1u); /* source: auto (новое событие) */
#else
	(void)0;
#endif
}
