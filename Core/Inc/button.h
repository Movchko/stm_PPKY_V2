/*
 * button.h
 *
 *  Created on: Dec 22, 2025
 *      Author: 79099
 */

#ifndef INC_BUTTON_H_
#define INC_BUTTON_H_

#include "main.h"

#define NUM_BUTTON 	7

#define BUT_ENTER 	3
#define BUT_UP 		1
#define BUT_DOWN	2
#define BUT_ESC		0
#define BUT_FORCE	4
#define BUT_STOP	5
#define BUT_FIRE 	6

/* Button_Process теперь вызывается раз в ~100 мс.
 * Пороговые счётчики считаются в количествах вызовов Button_Process. */
#define BUTTON_TICK_MS 10
#define SHORT_PRESS_MS 50
#define LONG_PRESS_MS 2000

#define LONG_PRESS_COUNT  (LONG_PRESS_MS / BUTTON_TICK_MS)  /* 2s  */
#define SHORT_PRESS_COUNT (SHORT_PRESS_MS / BUTTON_TICK_MS) /*  */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ButtonState {
	ButtonStateReset = 0,
	ButtonStatePress = 1,
	ButtonStateLongPress = 2,
	ButtonStateError = 3
}ButtonState;

struct Button {
	ButtonState state;
	uint8_t 	press_counter;
	uint8_t		ispress;
};



void Button_Init();
void Button_Process(); // таймер 10гц
ButtonState Button_GetState(uint8_t but);
void Button_ReadPin(); // чтение состояний
/* Weak hook: переинициализация микросхемы чтения кнопок (пока может быть пустым). */
void Button_ReinitReaderChip(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_BUTTON_H_ */
