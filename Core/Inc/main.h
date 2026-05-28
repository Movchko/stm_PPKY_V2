/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h5xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "spif.h"
#include "backend.h"
#include "can_bus.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void TGFX_SignalVSync(void);

void AppTimer1ms();
void AppTimer10ms();

void AppInit();
void AppProcess();
void LedInit();
void LedSetAll(uint8_t power);
void LedOffAll();
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PANEL_DG_Pin GPIO_PIN_3
#define PANEL_DG_GPIO_Port GPIOC
#define PANEL_DG_EXTI_IRQn EXTI3_IRQn
#define FLASH_CS_Pin GPIO_PIN_4
#define FLASH_CS_GPIO_Port GPIOA
#define ESP32_EN_Pin GPIO_PIN_5
#define ESP32_EN_GPIO_Port GPIOC
#define ESP32_BOOT_Pin GPIO_PIN_2
#define ESP32_BOOT_GPIO_Port GPIOB
#define PANEL_EN_Pin GPIO_PIN_10
#define PANEL_EN_GPIO_Port GPIOB
#define BRP_485_EN_Pin GPIO_PIN_6
#define BRP_485_EN_GPIO_Port GPIOC
#define KEY_2_Pin GPIO_PIN_8
#define KEY_2_GPIO_Port GPIOC
#define KEY_1_Pin GPIO_PIN_9
#define KEY_1_GPIO_Port GPIOC
#define ST2_MK_Pin GPIO_PIN_8
#define ST2_MK_GPIO_Port GPIOA
#define ST2_MK_EXTI_IRQn EXTI8_IRQn
#define ST1_MK_Pin GPIO_PIN_9
#define ST1_MK_GPIO_Port GPIOA
#define ST1_MK_EXTI_IRQn EXTI9_IRQn
#define LED_Pin GPIO_PIN_10
#define LED_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */
#define GFX_RATIO_MS 10
#define LED_TOGGLE HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);

extern RTC_HandleTypeDef hrtc;

#define NUM_ADC_CHANNEL 5
#define FILTERSIZE 128

#define RTC_BKP_MAGIC  0xAABB

extern uint16_t ADC_VAL[NUM_ADC_CHANNEL];
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
