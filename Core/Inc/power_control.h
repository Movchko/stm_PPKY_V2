/*
 * power_control.hpp
 *
 *  Created on: Nov 20, 2025
 *      Author: 79099
 */

#ifndef INC_POWER_CONTROL_H_
#define INC_POWER_CONTROL_H_

#include <stdint.h>
#include <string.h>

enum PControlState {
	PControl_Init,
	PControl_Idle,
	PControl_Normal,
	PControl_Fault
};

class PControl {
	uint8_t Num;
	PControlState state;

	float current;
	uint8_t status;

	bool      error_flag;
	bool      want_on;
	uint32_t  fault_time_ms;
	uint32_t  next_retry_ms;
	uint8_t   retry_count;

//	uint8_t	(*PControlGetST)(uint8_t ch);
//	uint32_t (*PControlGetADC)(uint8_t ch);
//	void (*PControlSetOut)(uint8_t ch, uint8_t out);


public:
	PControl(uint8_t ch);
	void PControlInit(uint8_t (*PControlGetSTCB)(uint8_t),
					  uint32_t (*PControlGetADCCB)(uint8_t),
					  void (*PControlSetOutCB)(uint8_t, uint8_t));
	void Process(uint32_t now_ms);

	void SetEnable(bool on) { want_on = on; }
	bool GetEnable() const { return want_on; }

	void OnStatusFault(uint32_t now_ms);

	PControlState GetState() const { return state; }
	float GetCurrent() const { return current; }
	bool IsError() const { return error_flag; }

	uint8_t	(*PControlGetST)(uint8_t ch);
	uint32_t (*PControlGetADC)(uint8_t ch);
	void (*PControlSetOut)(uint8_t ch, uint8_t out);


};


#endif /* INC_POWER_CONTROL_H_ */
