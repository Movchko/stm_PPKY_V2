/*
 * power_control.cpp
 *
 *  Created on: Nov 20, 2025
 *      Author: 79099
 */

#include "power_control.h"

PControl::PControl(uint8_t ch) {
	Num = ch;
	state = PControl_Init;
	current = 0.0f;
	status = 0;
	error_flag = false;
	want_on = false;
	fault_time_ms = 0;
	next_retry_ms = 0;
	retry_count = 0;
}

void PControl::PControlInit(uint8_t (*PControlGetSTCB)(uint8_t),
					  uint32_t (*PControlGetADCCB)(uint8_t),
					  void (*PControlSetOutCB)(uint8_t, uint8_t)) {

	PControlGetST = PControlGetSTCB;
	PControlGetADC = PControlGetADCCB;
	PControlSetOut = PControlSetOutCB;

	PControlSetOut(Num, false);
	state = PControl_Idle;
	error_flag = false;
	want_on = false;
	fault_time_ms = 0;
	next_retry_ms = 0;
	retry_count = 0;
}

void PControl::OnStatusFault(uint32_t now_ms) {
	// ST=1 при включённом выходе и желаемом включении — немедленное отключение и переход в Fault
	if (state == PControl_Normal && want_on) {
		PControlSetOut(Num, 0);
		state = PControl_Fault;
		error_flag = false;
		fault_time_ms = now_ms;
		next_retry_ms = now_ms + 2u; // N = 2 мс до первой повторной попытки
		retry_count = 0;
	}
}

void PControl::Process(uint32_t now_ms) {

	status = PControlGetST(Num);
	current = static_cast<float>(PControlGetADC(Num));

	switch (state) {
	case PControl_Init:
		PControlSetOut(Num, 0);
		state = PControl_Idle;
		error_flag = false;
		want_on = false;
		break;

	case PControl_Idle:
		error_flag = false;
		if (want_on) {
			PControlSetOut(Num, 1);
			state = PControl_Normal;
		} else {
			PControlSetOut(Num, 0);
		}
		break;

	case PControl_Normal:
		if (!want_on) {
			PControlSetOut(Num, 0);
			state = PControl_Idle;
		}
		// ST=0 в Normal считаем нормой, сбрасываем флаг ошибки, если он был
		if (status == 0u) {
			error_flag = false;
		}
		// Остальное (ST=1) обрабатывается в OnStatusFault по прерыванию
		break;

	case PControl_Fault:
		if (!want_on) {
			// Пользователь выключил — сбрасываем состояние
			PControlSetOut(Num, 0);
			state = PControl_Idle;
			error_flag = false;
			break;
		}

		// КЗ/ошибка могла уйти: если ST вернулся в 0, выходим из Fault и позволяем снова включиться
		if (status == 0u) {
			state = PControl_Idle;
			error_flag = false;
			fault_time_ms = 0;
			next_retry_ms = 0;
			retry_count = 0;
			break;
		}

		// Окно времени M=1000 мс с момента первой ошибки
		if ((now_ms - fault_time_ms) >= 1000u) {
			// Больше не пытаемся включаться, фиксируем ошибку питания
			error_flag = true;
			PControlSetOut(Num, 0);
			break;
		}

		// Одна повторная попытка включения в окне
		if (retry_count == 0 && now_ms >= next_retry_ms) {
			retry_count = 1;
			PControlSetOut(Num, 1);
			// Если снова будет ST=1, OnStatusFault переведёт нас обратно в Fault без изменения окна
		}
		break;
	}
}
