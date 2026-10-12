/*
 * led.h — на ППКУ нет локальных LED (индикация на панели из SYS/ZONES).
 * Заголовки оставлены пустыми stubs на случай legacy include.
 */

#ifndef INC_LED_H_
#define INC_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void Led_Init(void);
void Led_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_LED_H_ */
