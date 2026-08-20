/*
 * log_transport.h
 */

#ifndef INC_LOG_TRANSPORT_H_
#define INC_LOG_TRANSPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "log_stream.h"
#include "stm32h5xx_hal.h"

#define LOG_PKT_TYPE_REQ               16u

void LogTransport_Init(void);
void LogTransport_Process(void);
void LogTransport_OnUart2LogRequest(uint16_t seq,
                                    const uint8_t *payload,
                                    uint16_t payload_len);
void LogTransport_OnUartRxByte(UART_HandleTypeDef *huart);
void LogTransport_OnUartTxComplete(UART_HandleTypeDef *huart);
void LogTransport_OnUartError(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* INC_LOG_TRANSPORT_H_ */
