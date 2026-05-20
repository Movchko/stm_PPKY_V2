#ifndef INC_SOUND_PROFILES_H_
#define INC_SOUND_PROFILES_H_

/* Базовые one-shot сигналы */
#define SOUND_BUTTON_ACK_ON_MS                 100u
#define SOUND_ONE_SHOT_LONG_ON_MS             1300u
#define SOUND_ONE_SHOT_PAUSE_MS                200u

/* НЕИСПРАВНОСТЬ: сигнальный + дежурный */
#define SOUND_FAULT_SIGNAL_ON_MS               500u
#define SOUND_FAULT_SIGNAL_OFF_MS              100u
#define SOUND_FAULT_SIGNAL_PULSES              2u
#define SOUND_FAULT_DUTY_ON_MS                  10u
#define SOUND_FAULT_DUTY_OFF_MS                 50u
#define SOUND_FAULT_DUTY_PULSES                 2u
#define SOUND_FAULT_DUTY_REPEAT_MS           10000u

/* ВНИМАНИЕ: сигнальный + дежурный (отдельный от НЕИСПРАВНОСТИ) */
#define SOUND_ATTN_SIGNAL_ON_MS                120u
#define SOUND_ATTN_SIGNAL_OFF_MS                80u
#define SOUND_ATTN_SIGNAL_PULSES                3u
#define SOUND_ATTN_DUTY_ON_MS                   80u
#define SOUND_ATTN_DUTY_OFF_MS                 120u
#define SOUND_ATTN_DUTY_PULSES                  2u
#define SOUND_ATTN_DUTY_REPEAT_MS             5000u

/* ПОЖАР/ПУСК */
/* ПОЖАР2 (основной пожар): текущие действующие звуки */
#define SOUND_FIRE_DUTY_ON_MS                   70u
#define SOUND_FIRE_DUTY_OFF_MS                  50u
#define SOUND_FIRE_DUTY_PULSES                  1u
#define SOUND_FIRE_DUTY_REPEAT_MS            10000u

/* ПОЖАР1 (первая сработка в зоне "И"): отдельные сигнальный/дежурный */
#define SOUND_FIRE1_SIGNAL_ON_MS              2000u
#define SOUND_FIRE1_SIGNAL_OFF_MS              500u
#define SOUND_FIRE1_SIGNAL_PULSES                1u
#define SOUND_FIRE1_SIGNAL_REPEAT_MS          2500u
#define SOUND_FIRE1_DUTY_ON_MS          SOUND_FIRE_DUTY_ON_MS
#define SOUND_FIRE1_DUTY_OFF_MS         SOUND_FIRE_DUTY_OFF_MS
#define SOUND_FIRE1_DUTY_PULSES         SOUND_FIRE_DUTY_PULSES
#define SOUND_FIRE1_DUTY_REPEAT_MS      (SOUND_FIRE_DUTY_REPEAT_MS / 2u)

#define SOUND_START_DUTY_ON_MS                 200u
#define SOUND_START_DUTY_OFF_MS                200u
#define SOUND_START_DUTY_PULSES                 1u
#define SOUND_START_DUTY_REPEAT_MS             100u

#define SOUND_START_ALL_HOLD_DUTY_MS           800u
#define SOUND_START_ALL_HOLD_PERIOD_MS        1600u

#endif /* INC_SOUND_PROFILES_H_ */
