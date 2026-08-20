#ifndef RS_BUS_H
#define RS_BUS_H

#include "main.h"
#include "rs_bus_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RsBusFrameHandler)(const RsBusFrameView *frame, void *ctx);

typedef struct {
    UART_HandleTypeDef *uart;
    GPIO_TypeDef *de_port;
    uint16_t de_pin;
    RsBusFrameHandler handler;
    void *handler_ctx;
    uint8_t rx_dma_buf[64];
    uint8_t rx_buf[RS_BUS_MAX_FRAME_SIZE];
    uint16_t rx_len;
} RsBusContext;

void RsBus_Init(RsBusContext *ctx,
                UART_HandleTypeDef *uart,
                GPIO_TypeDef *de_port,
                uint16_t de_pin,
                RsBusFrameHandler handler,
                void *handler_ctx);
void RsBus_ProcessRxBytes(RsBusContext *ctx, const uint8_t *data, uint16_t len);
HAL_StatusTypeDef RsBus_SendFrame(RsBusContext *ctx,
                                  uint8_t addr,
                                  uint8_t seq,
                                  uint8_t flags,
                                  uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* RS_BUS_H */
