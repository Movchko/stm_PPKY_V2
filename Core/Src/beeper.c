/*
 * beeper.c — mute-слой для SYS POLL (локального HW нет).
 * PPKYConfig.beep — маска: бит N = панель с RS-адресом N (N = 0..7 в uint8_t).
 */

#include "beeper.h"
#include "device_config.h"
#include "event_log.h"
#include "gost_mode.h"

extern PPKYCfg PPKYConfig;

/* 1 = хотя бы один бит маски включён (для Resume / legacy API). */
static uint8_t beep_sound = 1u;

static uint8_t Beeper_MaskBit(uint8_t addr)
{
	/* Маска uint8_t: адреса 0..7. Адрес ≥8 — бит (addr & 7). */
	return (uint8_t)(1u << (addr & 7u));
}

void Beeper_Init(void)
{
	beep_sound = (PPKYConfig.beep != 0u) ? 1u : 0u;
}

void Beeper_SoundOnOff(bool soundOn)
{
	/* Глобально: все панели on → 0xFF, off → 0. */
	PPKYConfig.beep = soundOn ? 0xFFu : 0u;
	beep_sound = soundOn ? 1u : 0u;
}

void Beeper_SetPanelSound(uint8_t addr, uint8_t on)
{
	const uint8_t bit = Beeper_MaskBit(addr);
	if (on != 0u) {
		PPKYConfig.beep = (uint8_t)(PPKYConfig.beep | bit);
	} else {
		PPKYConfig.beep = (uint8_t)(PPKYConfig.beep & (uint8_t)(~bit));
	}
	beep_sound = (PPKYConfig.beep != 0u) ? 1u : 0u;
}

uint8_t Beeper_IsPanelSoundOn(uint8_t addr)
{
	return ((PPKYConfig.beep & Beeper_MaskBit(addr)) != 0u) ? 1u : 0u;
}

uint8_t Beeper_IsSoundEnabled(void)
{
	return beep_sound;
}

void Beeper_ResumeSoundOnNewEvent(void)
{
#if GOST_MODE
	if (PPKYConfig.beep_block != 0u) {
		return;
	}
	/* Mute панели — бит в маске, не глобальный beep_sound.
	 * Если снят только один бит, beep_sound может остаться 1 — всё равно
	 * поднять маску до 0xFF, иначе SYS_SOUND_ON для этой панели не появится. */
	if (PPKYConfig.beep == 0xFFu) {
		beep_sound = 1u;
		return;
	}
	PPKYConfig.beep = 0xFFu;
	beep_sound = 1u;
	EventLog_LogSoundToggle(1u, 1u); /* source: auto (новое событие) */
#else
	(void)0;
#endif
}
