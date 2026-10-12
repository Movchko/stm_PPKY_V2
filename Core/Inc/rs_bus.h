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
    /* Сколько байт DMA уже отдали в ProcessRxBytes (harvest без Abort). */
    uint16_t rx_dma_read;
    uint32_t rx_last_ms;
    /* Short-hold «ждём хвост»: если > RX_HOLD_ABORT_MS — бросить преамбулу (иначе
     * шум с 2 панелей вечно освежает rx_last_ms и парсер сидит на сироте 5A). */
    uint32_t rx_hold_ms;
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
/** ORE/FE в HAL_UART_ErrorCallback — сброс + снова Receive_DMA. */
void RsBus_RecoverFromError(RsBusContext *ctx);
/** Legacy IDLE path (больше не используется для USART1 RS). */
void RsBus_RxRearmFromIsr(RsBusContext *ctx);
/** Из основного цикла: если RX не BUSY_RX (умер после ошибки) — перезапустить. */
void RsBus_RxWatchdog(RsBusContext *ctx);
/**
 * Process10ms / quiet: снять НОВЫЕ байты из DMA по NDTR без Abort.
 * Rearm — SendFrame / RxCplt (буфер полный) / почти полный буфер.
 */
void RsBus_RxHarvest(RsBusContext *ctx);
/** DMA RX complete (буфер полный) — добрать хвост и rearm. */
void RsBus_OnRxDmaComplete(RsBusContext *ctx);
/**
 * Legacy IDLE IRQ helper (не вызывается при Receive_DMA).
 */
void RsBus_ProcessIdleRx(RsBusContext *ctx,
                         const uint8_t *data,
                         uint16_t size,
                         uint16_t already_read);
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
