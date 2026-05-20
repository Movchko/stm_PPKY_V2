/*
 * button.c
 *
 *  Created on: Dec 4, 2025
 *      Author: 79099
 */

#include "button.h"
#include "beeper.h"
#include "fire.h"

struct Button Buttons[NUM_BUTTON];

/* Интервал между попытками восстановления I2C (в тиках Button_Process, ~10 мс). */
#define BUTTON_I2C_RECOVERY_RETRY_TICKS 10u

static uint8_t s_btn_i2c_recovery_tick = BUTTON_I2C_RECOVERY_RETRY_TICKS;

static void Button_SetAllError(void)
{
	for(uint8_t i = 0; i < NUM_BUTTON; i++) {
		Buttons[i].state = ButtonStateError;
	}
}

static void Button_ResetStates(void)
{
	for (uint8_t i = 0; i < NUM_BUTTON; i++) {
		Buttons[i].state = ButtonStateReset;
		Buttons[i].press_counter = 0;
		Buttons[i].ispress = 0;
	}
}

static uint8_t Button_ReinitI2CDriver(void)
{
	return 1u;
}

__attribute__((weak)) void Button_ReinitReaderChip(void)
{
	/* Задел под будущую программную реинициализацию внешней кнопочной микросхемы. */
}

void Button_Init() {
	for(uint8_t i = 0; i < NUM_BUTTON; i++) {
		Buttons[i].state = ButtonStateReset;
		Buttons[i].press_counter = 0;
		Buttons[i].ispress = 0;
	}
	HAL_StatusTypeDef st = HAL_ERROR;
	uint8_t but = 0xFF;

}

void Button_Process() {
	Button_ReadPin();
	if(Buttons[0].state == ButtonStateError) {
		if (s_btn_i2c_recovery_tick < BUTTON_I2C_RECOVERY_RETRY_TICKS) {
			s_btn_i2c_recovery_tick++;
			return;
		}
		s_btn_i2c_recovery_tick = 0u;
		if (Button_ReinitI2CDriver()) {
			Button_ReinitReaderChip();
			Button_ResetStates();
		} else {
			Button_SetAllError();
		}
		return;
	}
	s_btn_i2c_recovery_tick = BUTTON_I2C_RECOVERY_RETRY_TICKS;

	for(uint8_t i = 0; i < NUM_BUTTON; i++) {
		if(Buttons[i].ispress == 0) {
			Buttons[i].state = ButtonStateReset;
			Buttons[i].press_counter = 0;
		} else {

			if((Buttons[i].press_counter >= LONG_PRESS_COUNT) && (Buttons[i].state == ButtonStatePress))
				Buttons[i].state = ButtonStateLongPress;
			if((Buttons[i].press_counter >= SHORT_PRESS_COUNT) && (Buttons[i].state == ButtonStateReset)) {
				Buttons[i].state = ButtonStatePress;
				/* Подтверждаем нажатие, не прерывая дежурные фоновые паттерны. */
				Beeper_ButtonAcknowledge();

			}

			Buttons[i].press_counter++;
		}
	}
}

ButtonState Button_GetState(uint8_t but) {
	return Buttons[but].state;
}

void Button_ReadPin() {
	HAL_StatusTypeDef st = HAL_ERROR;
	uint8_t but = 0xFF;

}
