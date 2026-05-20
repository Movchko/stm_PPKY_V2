/*
 * display.h
 *
 *  Created on: Oct 28, 2025
 *      Author: 79099
 */

#ifndef INC_DISPLAY_DISPLAY_H_
#define INC_DISPLAY_DISPLAY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Display initialization
void InitDisplay(void);

// Basic SPI communication functions
void Write_Cmd(uint8_t cmd);
void Write_Data(uint8_t data);

// Hardware control
void Display_Reset(void);

// TouchGFX integration functions
void Display_UpdateRect(uint8_t *framebuffer, uint16_t x, uint16_t y, uint16_t width, uint16_t height);

void Display_Enable(uint8_t en);

#ifdef __cplusplus
}
#endif

#endif /* INC_DISPLAY_DISPLAY_H_ */
