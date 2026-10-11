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
    uint8_t rx_dma_buf[RS_BUS_RX_DMA_SIZE];
    uint8_t rx_buf[RS_BUS_MAX_FRAME_SIZE];
    uint16_t rx_len;
    uint32_t rx_last_ms;
    /* Счётчики разбора (мастер копирует в Live Watch). */
    uint32_t rx_decode_crc_fail;
    uint32_t rx_decode_bad_hdr;
    uint32_t rx_decode_short_hold;
    uint32_t rx_resync_bytes;
    /* HAL_UART_ErrorCallback (FE/NE/ORE в DMA-режиме) → RecoverFromError. */
    uint32_t rx_err_count;
    uint32_t rx_last_err_code;
    /* RsBus_RxWatchdog нашёл RX не в BUSY_RX и перезапустил DMA. */
    uint32_t rx_rearm_wd;
} RsBusContext;

void RsBus_Init(RsBusContext *ctx,
                UART_HandleTypeDef *uart,
                GPIO_TypeDef *de_port,
                uint16_t de_pin,
                RsBusFrameHandler handler,
                void *handler_ctx);
/** ORE/FE в HAL_UART_ErrorCallback — сброс + снова ReceiveToIdle_DMA. */
void RsBus_RecoverFromError(RsBusContext *ctx);
/** Из HAL_UARTEx_RxEventCallback: перезапуск RX DMA без flush RDR (там может лежать 0xA5 следующего кадра). */
void RsBus_RxRearmFromIsr(RsBusContext *ctx);
/** Из основного цикла: если RX не BUSY_RX (умер после ошибки) — перезапустить. */
void RsBus_RxWatchdog(RsBusContext *ctx);
/**
 * Process10ms во время bus quiet: забрать байты из DMA без ожидания IDLE IRQ.
 * Иначе RSP панели (после addr_scan) лежит в буфере до следующего TX и
 * panel_count остаётся 0 → только SYS=0 probe → «ПРОВЕРКА» без TIME.
 */
void RsBus_RxHarvest(RsBusContext *ctx);
void RsBus_ProcessRxBytes(RsBusContext *ctx, const uint8_t *data, uint16_t len);
HAL_StatusTypeDef RsBus_SendFrame(RsBusContext *ctx,
                                  uint8_t addr,
                                  uint8_t seq,
                                  uint8_t flags,
                                  uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t payload_len);
HAL_StatusTypeDef RsBus_SendRaw(RsBusContext *ctx, const uint8_t *frame, uint16_t frame_len);

#ifdef __cplusplus
}
#endif

#endif /* RS_BUS_H */
