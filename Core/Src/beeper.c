/*
 * beeper.c
 *
 *  Created on: 2025
 *      Author: 79099
 */

#include "beeper.h"
#include "main.h"

/***********************************************************************************************************/
/* Внутренние типы и переменные */
/***********************************************************************************************************/

typedef enum
{
	BEEPER_STATE_IDLE = 0,              // Простой
	BEEPER_STATE_SHORT_BEEP,           // Одно короткое пищание
	BEEPER_STATE_DOUBLE_SHORT_BEEP,    // Два коротких пищания
	BEEPER_STATE_LONG_BEEP,            // Длинное пищание
	BEEPER_STATE_CONTINUOUS,            // Постоянное пищание
	BEEPER_STATE_FIRE_ALARM,            // Пожар: короткие включения с паузами
	BEEPER_STATE_PATTERN
} BeeperState_t;

/* Пожар: ~200 мс звук, ~300 мс тишина (шаг 10 мс в Beeper_Process) */
#define BEEPER_FIRE_ON_TICKS   20u
#define BEEPER_FIRE_OFF_TICKS  30u

static BeeperState_t beeper_state = BEEPER_STATE_IDLE;
static uint16_t beeper_counter = 0;
static uint8_t beep_phase = 0;  // Фаза для двойного пищания (0 - первое, 1 - пауза, 2 - второе)
static uint8_t fire_alarm_sound = 1u; /* 1 = фаза «звук», 0 = пауза */

static uint8_t beep_sound = 1;
static uint16_t pattern_on_ticks = 0;
static uint16_t pattern_off_ticks = 0;
static uint16_t pattern_repeat_ticks = 0;
static uint16_t pattern_counter = 0;
static uint16_t pattern_repeat_counter = 0;
static uint8_t pattern_pulses_total = 0;
static uint8_t pattern_pulses_left = 0;
static uint8_t pattern_sound_phase = 0;

typedef struct
{
	uint8_t valid;
	BeeperState_t state;
	uint16_t on_ticks;
	uint16_t off_ticks;
	uint16_t repeat_ticks;
	uint8_t pulses_total;
} BeeperResumeCtx_t;

static BeeperResumeCtx_t g_resume_ctx = {0};

static uint16_t Beeper_MsToTicks(uint16_t duration_ms)
{
	uint16_t ticks = (uint16_t)((duration_ms + 9u) / 10u);
	return (ticks == 0u) ? 1u : ticks;
}

/***********************************************************************************************************/
/* Внутренние функции */
/***********************************************************************************************************/

/**
 * @brief Включение звука
 */
static void Beeper_On(void)
{
	if(beep_sound == 0)
		return;

}

/**
 * @brief Выключение звука
 */
static void Beeper_Off(void)
{

}

static uint8_t Beeper_IsOneShotState(BeeperState_t st)
{
	return (st == BEEPER_STATE_SHORT_BEEP ||
		st == BEEPER_STATE_DOUBLE_SHORT_BEEP ||
		st == BEEPER_STATE_LONG_BEEP) ? 1u : 0u;
}

static void Beeper_CaptureResumeStateIfNeeded(void)
{
	/* Переснимаем контекст каждый раз: старый может устареть, если между
	 * кнопочным бипом и его завершением режим был переключён извне. */
	g_resume_ctx.valid = 0u;
	if (beeper_state == BEEPER_STATE_CONTINUOUS ||
	    beeper_state == BEEPER_STATE_FIRE_ALARM) {
		g_resume_ctx.valid = 1u;
		g_resume_ctx.state = beeper_state;
		return;
	}
	if (beeper_state == BEEPER_STATE_PATTERN && pattern_repeat_ticks > 0u) {
		g_resume_ctx.valid = 1u;
		g_resume_ctx.state = BEEPER_STATE_PATTERN;
		g_resume_ctx.on_ticks = pattern_on_ticks;
		g_resume_ctx.off_ticks = pattern_off_ticks;
		g_resume_ctx.repeat_ticks = pattern_repeat_ticks;
		g_resume_ctx.pulses_total = pattern_pulses_total;
	}
}

static void Beeper_RestoreAfterOneShot(void)
{
	if (!g_resume_ctx.valid) {
		beeper_state = BEEPER_STATE_IDLE;
		Beeper_Off();
		return;
	}

	if (g_resume_ctx.state == BEEPER_STATE_CONTINUOUS ||
	    g_resume_ctx.state == BEEPER_STATE_FIRE_ALARM) {
		beeper_state = g_resume_ctx.state;
		beeper_counter = 0u;
		beep_phase = 0u;
		Beeper_On();
		g_resume_ctx.valid = 0u;
		return;
	}

	if (g_resume_ctx.state == BEEPER_STATE_PATTERN && g_resume_ctx.pulses_total > 0u) {
		pattern_on_ticks = g_resume_ctx.on_ticks;
		pattern_off_ticks = g_resume_ctx.off_ticks;
		pattern_repeat_ticks = g_resume_ctx.repeat_ticks;
		pattern_pulses_total = g_resume_ctx.pulses_total;
		pattern_pulses_left = g_resume_ctx.pulses_total;
		pattern_sound_phase = 1u;
		pattern_counter = pattern_on_ticks;
		pattern_repeat_counter = 0u;
		beeper_state = BEEPER_STATE_PATTERN;
		Beeper_On();
		g_resume_ctx.valid = 0u;
		return;
	}

	g_resume_ctx.valid = 0u;
	beeper_state = BEEPER_STATE_IDLE;
	Beeper_Off();
}

/***********************************************************************************************************/
/* Публичные функции */
/***********************************************************************************************************/

/**
 * @brief Инициализация пищалки
 */
void Beeper_Init(void)
{
	beeper_state = BEEPER_STATE_IDLE;
	beeper_counter = 0;
	beep_phase = 0;
	Beeper_Off();
}

/**
 * @brief Одно короткое пищание (10мс)
 */
void Beeper_ShortBeep(void)
{
	if (!Beeper_IsOneShotState(beeper_state)) {
		Beeper_CaptureResumeStateIfNeeded();
	}
	beeper_state = BEEPER_STATE_SHORT_BEEP;
	beeper_counter = BEEPER_SHORT_BEEP_DURATION;
	beep_phase = 0;
	Beeper_On();
}

/**
 * @brief Два коротких пищания с паузой между ними
 */
void Beeper_DoubleShortBeep(void)
{
	if (!Beeper_IsOneShotState(beeper_state)) {
		Beeper_CaptureResumeStateIfNeeded();
	}
	beeper_state = BEEPER_STATE_DOUBLE_SHORT_BEEP;
	beeper_counter = BEEPER_SHORT_BEEP_DURATION;
	beep_phase = 0;  // Начинаем с первого пищания
	Beeper_On();
}

/**
 * @brief Длинное пищание (100мс)
 */
void Beeper_LongBeep(void)
{
	if (!Beeper_IsOneShotState(beeper_state)) {
		Beeper_CaptureResumeStateIfNeeded();
	}
	beeper_state = BEEPER_STATE_LONG_BEEP;
	beeper_counter = BEEPER_LONG_BEEP_DURATION;
	beep_phase = 0;
	Beeper_On();
}

void Beeper_LongBeep1300ms(void)
{
	Beeper_LongBeep();
}

/**
 * @brief Включить постоянное пищание
 */
void Beeper_ContinuousOn(void)
{
	g_resume_ctx.valid = 0u;
	beeper_state = BEEPER_STATE_CONTINUOUS;
	beeper_counter = 0;
	beep_phase = 0;
	Beeper_On();
}

void Beeper_FireAlarmOn(void)
{
	Beeper_ContinuousOn();

	//beeper_state = BEEPER_STATE_FIRE_ALARM;
	//fire_alarm_sound = 1u;
	//beeper_counter = BEEPER_FIRE_ON_TICKS;
	//Beeper_On();
}

void Beeper_FireAlarmOff(void)
{
	if (beeper_state == BEEPER_STATE_FIRE_ALARM) {
		beeper_state = BEEPER_STATE_IDLE;
		Beeper_Off();
	}
}

/**
 * @brief Выключить постоянное пищание
 */
void Beeper_ContinuousOff(void)
{
	if (beeper_state == BEEPER_STATE_CONTINUOUS)
	{
		beeper_state = BEEPER_STATE_IDLE;
		g_resume_ctx.valid = 0u;
		Beeper_Off();
	}
}

void Beeper_StopPattern(void)
{
	if (beeper_state == BEEPER_STATE_PATTERN) {
		beeper_state = BEEPER_STATE_IDLE;
		g_resume_ctx.valid = 0u;
		Beeper_Off();
	}
}

/**
 * @brief Переключить состояние постоянного пищания
 */
void Beeper_ContinuousToggle(void)
{
	if (beeper_state == BEEPER_STATE_CONTINUOUS)
	{
		Beeper_ContinuousOff();
	}
	else
	{
		Beeper_ContinuousOn();
	}
}

void Beeper_PlayOneShotMs(uint16_t duration_ms)
{
	beeper_state = BEEPER_STATE_LONG_BEEP;
	beeper_counter = Beeper_MsToTicks(duration_ms);
	beep_phase = 0u;
	Beeper_On();
}

void Beeper_StartPulseTrain(uint16_t pulse_on_ms, uint16_t pulse_off_ms, uint8_t pulses, uint16_t repeat_period_ms)
{
	if (pulses == 0u) {
		Beeper_StopPattern();
		return;
	}
	g_resume_ctx.valid = 0u;
	pattern_on_ticks = Beeper_MsToTicks(pulse_on_ms);
	pattern_off_ticks = Beeper_MsToTicks(pulse_off_ms);
	pattern_repeat_ticks = (repeat_period_ms == 0u) ? 0u : Beeper_MsToTicks(repeat_period_ms);
	pattern_pulses_total = pulses;
	pattern_pulses_left = pulses;
	pattern_sound_phase = 1u;
	pattern_counter = pattern_on_ticks;
	pattern_repeat_counter = 0u;
	beeper_state = BEEPER_STATE_PATTERN;
	Beeper_On();
}

void Beeper_ButtonAcknowledge(void)
{
	Beeper_ShortBeep();
}

/**
 * @brief Функция обработки состояния пищалки (вызывать каждые 10мс)
 * @note Должна вызываться из таймера или основного цикла с периодом 10мс
 */
void Beeper_Process(void)
{


	switch (beeper_state)
	{
		case BEEPER_STATE_IDLE:
			// Ничего не делаем
			break;

		case BEEPER_STATE_SHORT_BEEP:
			// Одно короткое пищание
			if (beeper_counter > 0)
			{
				beeper_counter--;
			}
			else
			{
				// Пищание завершено
				Beeper_RestoreAfterOneShot();
			}
			break;

		case BEEPER_STATE_DOUBLE_SHORT_BEEP:
			// Два коротких пищания
			if (beeper_counter > 0)
			{
				beeper_counter--;
			}
			else
			{
				// Текущая фаза завершена
				if (beep_phase == 0)
				{
					// Первое пищание завершено, начинаем паузу
					Beeper_Off();
					beep_phase = 1;
					beeper_counter = BEEPER_PAUSE_DURATION;
				}
				else if (beep_phase == 1)
				{
					// Пауза завершена, начинаем второе пищание
					Beeper_On();
					beep_phase = 2;
					beeper_counter = BEEPER_SHORT_BEEP_DURATION;
				}
				else
				{
					// Второе пищание завершено
					beep_phase = 0;
					Beeper_RestoreAfterOneShot();
				}
			}
			break;

		case BEEPER_STATE_LONG_BEEP:
			// Длинное пищание
			if (beeper_counter > 0)
			{
				beeper_counter--;
			}
			else
			{
				// Пищание завершено
				Beeper_RestoreAfterOneShot();
			}
			break;

		case BEEPER_STATE_CONTINUOUS:
			// Постоянное пищание - ничего не делаем, звук уже включен
			break;

		case BEEPER_STATE_FIRE_ALARM:
			if (beeper_counter > 0u) {
				beeper_counter--;
			} else {
				if (fire_alarm_sound) {
					Beeper_Off();
					fire_alarm_sound = 0u;
					beeper_counter = BEEPER_FIRE_OFF_TICKS;
				} else {
					Beeper_On();
					fire_alarm_sound = 1u;
					beeper_counter = BEEPER_FIRE_ON_TICKS;
				}
			}
			break;

		case BEEPER_STATE_PATTERN:
			if (pattern_counter > 0u) {
				pattern_counter--;
				break;
			}
			if (pattern_sound_phase) {
				Beeper_Off();
				pattern_sound_phase = 0u;
				pattern_counter = pattern_off_ticks;
				if (pattern_pulses_left > 0u) {
					pattern_pulses_left--;
				}
			} else {
				if (pattern_pulses_left > 0u) {
					Beeper_On();
					pattern_sound_phase = 1u;
					pattern_counter = pattern_on_ticks;
				} else {
					if (pattern_repeat_ticks == 0u) {
						beeper_state = BEEPER_STATE_IDLE;
						Beeper_Off();
						break;
					}
					if (pattern_repeat_counter < pattern_repeat_ticks) {
						pattern_repeat_counter++;
						pattern_counter = 1u;
					} else {
						pattern_repeat_counter = 0u;
						pattern_pulses_left = pattern_pulses_total;
						Beeper_On();
						pattern_sound_phase = 1u;
						pattern_counter = pattern_on_ticks;
					}
				}
			}
			break;

		default:
			// Неизвестное состояние - переходим в IDLE
			beeper_state = BEEPER_STATE_IDLE;
			Beeper_Off();
			break;
	}
}

void Beeper_SoundOnOff(bool soundOn) {
	beep_sound = soundOn;
}
