/*
 * led.h
 *
 *  Created on: Dec 22, 2025
 *      Author: 79099
 */

#ifndef INC_LED_H_
#define INC_LED_H_


#define LED_POWER 		0
#define LED_NORM		1
#define LED_START		2
#define LED_STOP		3
#define LED_ERR			4
#define LED_FIRE		5
#define LED_AUTO_OFF	6

#define LED_BUT_START_ALL	7
#define LED_BUT_STOP		8
#define LED_BUT_START_SP	9
#define LED_BUT_ENTER_UP	13
#define LED_BUT_ESC_DW		14

#define LED_STR_START_ALL	10
#define LED_STR_STOP		11
#define LED_STR_START_SP	12

/* Яркость подсветки кнопок по умолчанию и при активности */
#define LED_BUT_DIM_BRIGHTNESS   50      /* базовая яркость кнопок ENTER/ESC */
#define LED_BUT_MAX_BRIGHTNESS   0xFF    /* максимальная яркость */
/* Яркость статусных ламп (POWER..AUTO_OFF) */
#define LED_STATUS_DIM_BRIGHTNESS 50
#define LED_STATUS_MAX_BRIGHTNESS 0xFF

/* Таймаут возврата к базовой яркости, в тиках Led_Process (10 мс) */
#define LED_BUT_IDLE_TIMEOUT_TICKS 500    /* 5 секунд при шаге 10 мс */
#define LED_STATUS_IDLE_TIMEOUT_TICKS 500 /* 5 секунд при шаге 10 мс */
/* Период синхронизации LED-состояний в I2C, в тиках Led_Process (10 мс) */
#define LED_I2C_SYNC_PERIOD_TICKS   5u     /* 50 мс */

#ifdef __cplusplus
extern "C" {
#endif

void Led_Init();
void Led_SetAll(uint8_t power);
void Led_OffAll();
void Led_Set(uint8_t led, uint8_t st);
void Led_Snake(uint8_t state);
void Led_TestToogle();
void Led_Process();
void Led_SetBrightness(uint8_t led, uint8_t power);
void Led_ForceStatusBright(uint8_t led);

#ifdef __cplusplus
}
#endif

#endif /* INC_LED_H_ */
