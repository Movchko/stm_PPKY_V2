#include "rs_bus.h"

#include <string.h>

static void rs_bus_de_set(RsBusContext *ctx, GPIO_PinState level)
{
    if (ctx != 0 && ctx->de_port != 0) {
        HAL_GPIO_WritePin(ctx->de_port, ctx->de_pin, level);
    }
}

static void rs_bus_wait_tx_complete(UART_HandleTypeDef *uart)
{
    uint32_t spins = 0u;

    if (uart == 0) {
        return;
    }
    /* Не завязываемся на HAL_GetTick(): TX может идти с запрещённым UART IRQ,
     * а раньше SysTick был ниже UART и тик замирал. */
    while (__HAL_UART_GET_FLAG(uart, UART_FLAG_TC) == RESET) {
        spins++;
        if (spins > 2000000u) {
            break;
        }
    }
}

/* RX IDLE/DMA IRQ не должен врываться в polling TX (re-arm DMA посреди кадра). */
static void rs_bus_rx_irq_lock(UART_HandleTypeDef *uart)
{
    if (uart == 0) {
        return;
    }
    if (uart->Instance == USART1) {
        HAL_NVIC_DisableIRQ(USART1_IRQn);
        HAL_NVIC_DisableIRQ(GPDMA1_Channel2_IRQn);
    } else if (uart->Instance == USART2) {
        HAL_NVIC_DisableIRQ(USART2_IRQn);
        HAL_NVIC_DisableIRQ(GPDMA1_Channel1_IRQn);
    }
}

static void rs_bus_rx_irq_unlock(UART_HandleTypeDef *uart)
{
    if (uart == 0) {
        return;
    }
    if (uart->Instance == USART1) {
        HAL_NVIC_EnableIRQ(GPDMA1_Channel2_IRQn);
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    } else if (uart->Instance == USART2) {
        HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
}

static void rs_bus_rx_arm(RsBusContext *ctx)
{
    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    (void)HAL_UART_AbortReceive(ctx->uart);
    (void)HAL_UARTEx_ReceiveToIdle_DMA(ctx->uart, ctx->rx_dma_buf, sizeof(ctx->rx_dma_buf));
    if (ctx->uart->hdmarx != 0) {
        __HAL_DMA_DISABLE_IT(ctx->uart->hdmarx, DMA_IT_HT);
    }
}

uint16_t RsBus_Checksum16(const uint8_t *data, uint16_t len)
{
    uint32_t sum = 0u;
    uint16_t i;

    if (data == 0) {
        return 0u;
    }

    for (i = 0u; i < len; i++) {
        sum += data[i];
    }

    return (uint16_t)(sum & 0xFFFFu);
}

uint16_t RsBus_FrameEncode(uint8_t *dst,
                           uint16_t dst_size,
                           uint8_t addr,
                           uint8_t seq,
                           uint8_t flags,
                           uint8_t cmd,
                           const uint8_t *payload,
                           uint16_t payload_len)
{
    uint16_t len_field;
    uint16_t total_size;
    uint16_t crc;

    if (dst == 0 || payload_len > RS_BUS_MAX_WIRE_PAYLOAD) {
        return 0u;
    }

    len_field = (uint16_t)(4u + payload_len);
    total_size = (uint16_t)(2u + 1u + len_field + 2u);
    if (dst_size < total_size) {
        return 0u;
    }

    dst[0] = RS_BUS_PREAMBLE_0;
    dst[1] = RS_BUS_PREAMBLE_1;
    dst[2] = (uint8_t)len_field;
    dst[3] = addr;
    dst[4] = seq;
    dst[5] = flags;
    dst[6] = cmd;
    if (payload_len != 0u && payload != 0) {
        memcpy(&dst[7], payload, payload_len);
    }
    crc = RsBus_Checksum16(&dst[3], len_field);
    dst[(uint16_t)(7u + payload_len)] = (uint8_t)(crc & 0xFFu);
    dst[(uint16_t)(8u + payload_len)] = (uint8_t)(crc >> 8);
    return total_size;
}

uint8_t RsBus_FrameDecode(const uint8_t *src,
                          uint16_t src_size,
                          RsBusFrameView *out_frame,
                          uint16_t *out_consumed)
{
    uint16_t len_field;
    uint16_t total_size;
    uint16_t rx_crc;
    uint16_t calc_crc;

    if (out_consumed != 0) {
        *out_consumed = 0u;
    }
    if (src == 0 || src_size < 9u) {
        return 0u;
    }
    if (src[0] != RS_BUS_PREAMBLE_0 || src[1] != RS_BUS_PREAMBLE_1) {
        return 0u;
    }

    len_field = src[2];
    if (len_field < 4u) {
        return 0u;
    }
    total_size = (uint16_t)(2u + 1u + len_field + 2u);
    if (src_size < total_size) {
        return 0u;
    }

    rx_crc = (uint16_t)src[(uint16_t)(3u + len_field)] |
             (uint16_t)((uint16_t)src[(uint16_t)(4u + len_field)] << 8);
    calc_crc = RsBus_Checksum16(&src[3], len_field);
    if (rx_crc != calc_crc) {
        return 0u;
    }

    if (out_frame != 0) {
        out_frame->addr = src[3];
        out_frame->seq = src[4];
        out_frame->flags = src[5];
        out_frame->cmd = src[6];
        out_frame->payload = &src[7];
        out_frame->payload_len = (uint16_t)(len_field - 4u);
    }
    if (out_consumed != 0) {
        *out_consumed = total_size;
    }
    return 1u;
}

void RsBus_Init(RsBusContext *ctx,
                UART_HandleTypeDef *uart,
                GPIO_TypeDef *de_port,
                uint16_t de_pin,
                RsBusFrameHandler handler,
                void *handler_ctx)
{
    if (ctx == 0) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->uart = uart;
    ctx->de_port = de_port;
    ctx->de_pin = de_pin;
    ctx->handler = handler;
    ctx->handler_ctx = handler_ctx;
    rs_bus_de_set(ctx, GPIO_PIN_RESET);
    rs_bus_rx_arm(ctx);
}

void RsBus_ProcessRxBytes(RsBusContext *ctx, const uint8_t *data, uint16_t len)
{
    uint16_t pos;
    uint16_t consumed;
    RsBusFrameView frame;

    if (ctx == 0 || data == 0 || len == 0u) {
        return;
    }

    if ((uint32_t)ctx->rx_len + len > sizeof(ctx->rx_buf)) {
        ctx->rx_len = 0u;
    }

    memcpy(&ctx->rx_buf[ctx->rx_len], data, len);
    ctx->rx_len = (uint16_t)(ctx->rx_len + len);

    pos = 0u;
    while (pos < ctx->rx_len) {
        if (ctx->rx_buf[pos] != RS_BUS_PREAMBLE_0) {
            pos++;
            continue;
        }
        if ((uint16_t)(ctx->rx_len - pos) < 2u || ctx->rx_buf[(uint16_t)(pos + 1u)] != RS_BUS_PREAMBLE_1) {
            break;
        }
        if (!RsBus_FrameDecode(&ctx->rx_buf[pos], (uint16_t)(ctx->rx_len - pos), &frame, &consumed)) {
            if ((uint16_t)(ctx->rx_len - pos) < 9u) {
                break;
            }
            pos++;
            continue;
        }
        if (ctx->handler != 0) {
            ctx->handler(&frame, ctx->handler_ctx);
        }
        pos = (uint16_t)(pos + consumed);
    }

    if (pos != 0u) {
        memmove(ctx->rx_buf, &ctx->rx_buf[pos], (size_t)(ctx->rx_len - pos));
        ctx->rx_len = (uint16_t)(ctx->rx_len - pos);
    }
}

HAL_StatusTypeDef RsBus_SendFrame(RsBusContext *ctx,
                                  uint8_t addr,
                                  uint8_t seq,
                                  uint8_t flags,
                                  uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    uint8_t frame[RS_BUS_MAX_FRAME_SIZE];
    uint16_t frame_len;

    if (ctx == 0 || ctx->uart == 0) {
        return HAL_ERROR;
    }

    frame_len = RsBus_FrameEncode(frame, sizeof(frame), addr, seq, flags, cmd, payload, payload_len);
    if (frame_len == 0u) {
        return HAL_ERROR;
    }

    rs_bus_rx_irq_lock(ctx->uart);
    (void)HAL_UART_AbortReceive(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_SET);
    if (HAL_UART_Transmit(ctx->uart, frame, frame_len, 20u) != HAL_OK) {
        rs_bus_wait_tx_complete(ctx->uart);
        rs_bus_de_set(ctx, GPIO_PIN_RESET);
        rs_bus_rx_irq_unlock(ctx->uart);
        rs_bus_rx_arm(ctx);
        return HAL_ERROR;
    }
    rs_bus_wait_tx_complete(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_RESET);
    rs_bus_rx_irq_unlock(ctx->uart);
    rs_bus_rx_arm(ctx);
    return HAL_OK;
}

HAL_StatusTypeDef RsBus_SendRaw(RsBusContext *ctx, const uint8_t *frame, uint16_t frame_len)
{
    if (ctx == 0 || ctx->uart == 0 || frame == 0 || frame_len == 0u) {
        return HAL_ERROR;
    }
    rs_bus_rx_irq_lock(ctx->uart);
    (void)HAL_UART_AbortReceive(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_SET);
    if (HAL_UART_Transmit(ctx->uart, (uint8_t *)frame, frame_len, 50u) != HAL_OK) {
        rs_bus_wait_tx_complete(ctx->uart);
        rs_bus_de_set(ctx, GPIO_PIN_RESET);
        rs_bus_rx_irq_unlock(ctx->uart);
        rs_bus_rx_arm(ctx);
        return HAL_ERROR;
    }
    rs_bus_wait_tx_complete(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_RESET);
    rs_bus_rx_irq_unlock(ctx->uart);
    rs_bus_rx_arm(ctx);
    return HAL_OK;
}
