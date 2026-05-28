#ifndef INC_WARNING_H_
#define INC_WARNING_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Обработка предупреждений/неисправностей виртуальных устройств. */
void WarningProcess1ms(void);
/* Ошибки выходов модуля power: bits0..1 => ВЫХОД1/2 (МКУ), bit2 => панель */
void Warning_SetPowerFaultMask(uint8_t mask);
/* Ошибки входов питания ППКУ: bits0..1 => ПИТАНИЕ1/2 */
void Warning_SetPpkuInputFaultMask(uint8_t mask);
uint8_t Warning_HasActiveFault(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_WARNING_H_ */

