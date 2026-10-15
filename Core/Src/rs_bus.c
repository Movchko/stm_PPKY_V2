#include "rs_bus.h"
#include "rs_panel_master_debug.h"

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

static void rs_bus_rx_flush_errors(UART_HandleTypeDef *uart)
{
    if (uart == 0) {
        return;
    }
    __HAL_UART_CLEAR_FLAG(uart, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                UART_CLEAR_NEF | UART_CLEAR_OREF);
    __HAL_UART_SEND_REQ(uart, UART_RXDATA_FLUSH_REQUEST);
    uart->ErrorCode = HAL_UART_ERROR_NONE;
}

/* @250 кбод: символ ≈40 мкс. DE lead — несколько символов mark после DE↑,
 * чтобы A5 не сел на ringing драйвера (история с 2 Мбод: без паузы теряли A5). */
#define RS_BUS_DE_LEAD_US 300u
/* Жертвенные 0xFF перед A5: щель >1 симв в TX → orphan 5A на мастере. */
#define RS_BUS_TX_PAD_N   4u

static void rs_bus_delay_us(uint32_t us)
{
    /* Итерация volatile-цикла ≥ 4 тактов → задержка не короче заданной. */
    volatile uint32_t n = (SystemCoreClock / 1000000u) * us / 4u;
    while (n > 0u) {
        n--;
    }
}

/*
 * Pad 0xFF + кадр одним Transmit: два вызова давали IDLE между FF и A5,
 * мастер rearm mid-RSP → src_size 28..32 < total_size, frames_ok=0 при 2+ панелях.
 */

static void rs_bus_rx_start_dma(RsBusContext *ctx)
{
    ctx->rx_dma_read = 0u;
    /*
     * НЕ ReceiveToIdle: щель mid-RSP → IDLE callback + rearm съедает хвост.
     * Receive_DMA: байты копятся, harvest снимает по NDTR без Abort.
     */
    (void)HAL_UART_Receive_DMA(ctx->uart, ctx->rx_dma_buf, sizeof(ctx->rx_dma_buf));
    if (ctx->uart->hdmarx != 0) {
        __HAL_DMA_DISABLE_IT(ctx->uart->hdmarx, DMA_IT_HT);
    }
    /* IDLE не нужен — иначе снова mid-frame stop. */
    ATOMIC_CLEAR_BIT(ctx->uart->Instance->CR1, USART_CR1_IDLEIE);
    /* HAL в DMA-режиме считает FE/NE/PE блокирующими: abort RX + ErrorCallback.
     * Помеха на фронте DE не должна останавливать приём — байт уходит в DMA, кадр отсеет CRC. */
    ATOMIC_CLEAR_BIT(ctx->uart->Instance->CR3, USART_CR3_EIE);
    ATOMIC_CLEAR_BIT(ctx->uart->Instance->CR1, USART_CR1_PEIE);
}

static void rs_bus_rx_arm(RsBusContext *ctx)
{
    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    (void)HAL_UART_AbortReceive(ctx->uart);
    rs_bus_rx_flush_errors(ctx->uart);
    ctx->uart->RxState = HAL_UART_STATE_READY;
    ctx->uart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
    rs_bus_rx_start_dma(ctx);
}

void RsBus_RxRearmFromIsr(RsBusContext *ctx)
{
    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    if (ctx->uart->RxState != HAL_UART_STATE_READY) {
        return;
    }
    /* Без RXDATA_FLUSH: в RDR уже может лежать 0xA5 следующего кадра. */
    __HAL_UART_CLEAR_FLAG(ctx->uart, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                     UART_CLEAR_NEF | UART_CLEAR_OREF);
    rs_bus_rx_start_dma(ctx);
}

void RsBus_RxWatchdog(RsBusContext *ctx)
{
    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    if (ctx->uart->RxState == HAL_UART_STATE_BUSY_RX) {
        return;
    }
    rs_bus_rx_irq_lock(ctx->uart);
    if (ctx->uart->RxState != HAL_UART_STATE_BUSY_RX) {
        ctx->rx_rearm_wd++;
        rs_bus_rx_arm(ctx);
    }
    rs_bus_rx_irq_unlock(ctx->uart);
}

/* После TC/DE→RX: дать шине устаканиться, иначе в DMA ringing без 0xA5. */
static void rs_bus_post_tx_settle(void)
{
    rs_bus_delay_us(RS_BUS_DE_LEAD_US);
}

/** После ORE/FE: сброс флагов + слив RDR/FIFO, иначе IRQ крутится с ErrorCode=ORE. */
void RsBus_RecoverFromError(RsBusContext *ctx)
{
    UART_HandleTypeDef *uart;

    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    uart = ctx->uart;
    ctx->rx_err_count++;
    ctx->rx_last_err_code = uart->ErrorCode;
    __HAL_UART_CLEAR_FLAG(uart, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                UART_CLEAR_NEF | UART_CLEAR_OREF);
    __HAL_UART_SEND_REQ(uart, UART_RXDATA_FLUSH_REQUEST);
    uart->ErrorCode = HAL_UART_ERROR_NONE;
    uart->RxState = HAL_UART_STATE_READY;
    uart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
    rs_bus_rx_arm(ctx);
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
    if (uart != 0) {
        /* OVRDIS: при переполнении RDR перезаписывается, ORE не ставится и приём
         * не блокируется (ORE в DMA-режиме = abort RX). Писать CR3.OVRDIS только при UE=0. */
        __HAL_UART_DISABLE(uart);
        SET_BIT(uart->Instance->CR3, USART_CR3_OVRDIS);
        __HAL_UART_ENABLE(uart);
    }
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

    {
        /* Недособранный кадр: сброс по тишине (хвост придёт следующим harvest). */
        uint32_t now = HAL_GetTick();
        if (ctx->rx_len != 0u && (now - ctx->rx_last_ms) > 50u) {
            ctx->rx_len = 0u;
        }
        ctx->rx_last_ms = now;
    }
    if ((uint32_t)ctx->rx_len + len > sizeof(ctx->rx_buf)) {
        ctx->rx_len = 0u;
    }

    memcpy(&ctx->rx_buf[ctx->rx_len], data, len);
    ctx->rx_len = (uint16_t)(ctx->rx_len + len);

    pos = 0u;
    while (pos < ctx->rx_len) {
        /* Ответ панели без A5 (DE↑ съел A5) — «сирота» 5A.
         * Live 2 панели: 5A 01 81… — len=1 ложный sync (0x81=RSP_POLL не на месте). */
        if (ctx->rx_buf[pos] == RS_BUS_PREAMBLE_1) {
            uint16_t avail = (uint16_t)(ctx->rx_len - pos);
            uint8_t tmp[RS_BUS_MAX_FRAME_SIZE];
            uint16_t need;
            uint16_t len_field;
            uint16_t total_size;
            uint8_t addr_b;
            uint8_t flags_b;
            uint8_t cmd_b;

            if (avail < 8u) {
                ctx->rx_decode_short_hold++;
                break;
            }
            len_field = ctx->rx_buf[(uint16_t)(pos + 1u)];
            /* Панель BuildRsp ≤96 → len≤100. Больший len = ложный sync → вечный break@373. */
            if (len_field < 4u || len_field > 100u) {
                pos++;
                ctx->rx_resync_bytes++;
                continue;
            }
            /* 5A|len|addr|seq|flags|cmd — отсечь ложный 5A в середине потока. */
            addr_b = ctx->rx_buf[(uint16_t)(pos + 2u)];
            flags_b = ctx->rx_buf[(uint16_t)(pos + 4u)];
            cmd_b = ctx->rx_buf[(uint16_t)(pos + 5u)];
            if (addr_b < 1u || addr_b > 8u ||
                (flags_b & (uint8_t)RS_BUS_FLAG_DIR) == 0u ||
                (cmd_b & 0x80u) == 0u) {
                pos++;
                ctx->rx_resync_bytes++;
                continue;
            }
            total_size = (uint16_t)(2u + 1u + len_field + 2u);
            if (avail < (uint16_t)(total_size - 1u)) {
                /* Хвост доберёт следующий DMA harvest — без RDR/NDTR drain. */
                g_rs_master_dbg.rx_short_avail = avail;
                g_rs_master_dbg.rx_short_need =
                    (uint16_t)(total_size - 1u);
                ctx->rx_decode_short_hold++;
                break;
            }
            need = (uint16_t)(1u + avail);
            if (need > (uint16_t)sizeof(tmp)) {
                need = (uint16_t)sizeof(tmp);
            }
            tmp[0] = RS_BUS_PREAMBLE_0;
            memcpy(&tmp[1], &ctx->rx_buf[pos], (size_t)(need - 1u));
            if (RsBus_FrameDecode(tmp, need, &frame, &consumed) != 0u &&
                consumed >= 2u) {
                if (ctx->handler != 0) {
                    ctx->handler(&frame, ctx->handler_ctx);
                }
                pos = (uint16_t)(pos + (uint16_t)(consumed - 1u));
                continue;
            }
            pos++;
            ctx->rx_resync_bytes++;
            continue;
        }
        if (ctx->rx_buf[pos] != RS_BUS_PREAMBLE_0) {
            pos++;
            ctx->rx_resync_bytes++;
            continue;
        }
        if ((uint16_t)(ctx->rx_len - pos) < 2u) {
            ctx->rx_decode_short_hold++;
            break;
        }
        if (ctx->rx_buf[(uint16_t)(pos + 1u)] != RS_BUS_PREAMBLE_1) {
            pos++;
            ctx->rx_resync_bytes++;
            continue;
        }
        if (!RsBus_FrameDecode(&ctx->rx_buf[pos], (uint16_t)(ctx->rx_len - pos), &frame, &consumed)) {
            uint16_t avail = (uint16_t)(ctx->rx_len - pos);
            uint16_t len_field;
            uint16_t total_size;

            if (avail < 9u) {
                ctx->rx_decode_short_hold++;
                break;
            }
            len_field = ctx->rx_buf[(uint16_t)(pos + 2u)];
            if (len_field < 4u) {
                ctx->rx_decode_bad_hdr++;
                pos++;
                continue;
            }
            total_size = (uint16_t)(2u + 1u + len_field + 2u);
            if (avail < total_size) {
                ctx->rx_decode_short_hold++;
                break;
            }
            /* Preamble+len ок, но CRC/поля не сошлись. */
            ctx->rx_decode_crc_fail++;
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

/*
 * До AbortReceive: забрать уже лежащие в DMA байты. Иначе ответ панели,
 * пришедший между IDLE и следующим TX (Process @2 кГц), выбрасывается —
 * Live: rx_raw шумит, rx_frames_ok=0, rx_resync==raw.
 * Вызывать только при уже заблокированном RX IRQ.
 */
static void rs_bus_rx_drain_pending(RsBusContext *ctx)
{
    UART_HandleTypeDef *uart;
    uint16_t size;
    uint16_t ndtr;
    uint16_t n;

    if (ctx == 0 || ctx->uart == 0 || ctx->uart->hdmarx == 0) {
        return;
    }
    uart = ctx->uart;
    size = (uint16_t)sizeof(ctx->rx_dma_buf);
    ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(uart->hdmarx);
    if (ndtr > size) {
        return;
    }
    n = (uint16_t)(size - ndtr);
    if (n <= ctx->rx_dma_read) {
        return;
    }
    {
        uint16_t start = ctx->rx_dma_read;
        uint16_t chunk = (uint16_t)(n - start);
        /* Сначала отметить диапазон: ProcessRxBytes может добрать хвост по NDTR. */
        ctx->rx_dma_read = n;
        RsBus_ProcessRxBytes(ctx, &ctx->rx_dma_buf[start], chunk);
    }
}

void RsBus_RxHarvest(RsBusContext *ctx)
{
    UART_HandleTypeDef *uart;
    uint16_t size;
    uint16_t ndtr;
    uint16_t n;

    if (ctx == 0 || ctx->uart == 0 || ctx->uart->hdmarx == 0) {
        return;
    }
    uart = ctx->uart;
    size = (uint16_t)sizeof(ctx->rx_dma_buf);
    ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(uart->hdmarx);
    if (ndtr > size) {
        return;
    }
    n = (uint16_t)(size - ndtr);
    /* Нет новых байт — не трогаем UART (раньше Abort @2 кГц убивал RSP). */
    if (n <= ctx->rx_dma_read) {
        return;
    }

    rs_bus_rx_irq_lock(uart);
    ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(uart->hdmarx);
    if (ndtr <= size) {
        n = (uint16_t)(size - ndtr);
        if (n > ctx->rx_dma_read) {
            uint16_t start = ctx->rx_dma_read;
            uint16_t chunk = (uint16_t)(n - start);
            RsPanelMasterDebug_OnRxDma(chunk);
            RsPanelMasterDebug_OnRxHead(&ctx->rx_dma_buf[start], chunk);
            /* Сначала отметить диапазон: ProcessRxBytes может добрать хвост по NDTR. */
            ctx->rx_dma_read = n;
            RsBus_ProcessRxBytes(ctx, &ctx->rx_dma_buf[start], chunk);
            /* DMA почти полный — иначе хвост кадра потеряется. */
            if (ctx->rx_dma_read >= (uint16_t)(size - 16u)) {
                (void)HAL_UART_AbortReceive(uart);
                rs_bus_rx_flush_errors(uart);
                uart->RxState = HAL_UART_STATE_READY;
                uart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
                rs_bus_rx_start_dma(ctx);
            }
        }
    }
    rs_bus_rx_irq_unlock(uart);
}

void RsBus_ProcessIdleRx(RsBusContext *ctx,
                         const uint8_t *data,
                         uint16_t size,
                         uint16_t already_read)
{
    if (ctx == 0 || data == 0 || size == 0u) {
        return;
    }
    if (already_read >= size) {
        return;
    }
    RsBus_ProcessRxBytes(ctx, &data[already_read],
                         (uint16_t)(size - already_read));
}

void RsBus_OnRxDmaComplete(RsBusContext *ctx)
{
    if (ctx == 0 || ctx->uart == 0) {
        return;
    }
    /* Буфер DMA полный: снять остаток и снова arm (без IDLE). */
    rs_bus_rx_irq_lock(ctx->uart);
    if (ctx->rx_dma_read < (uint16_t)sizeof(ctx->rx_dma_buf)) {
        RsBus_ProcessRxBytes(ctx, &ctx->rx_dma_buf[ctx->rx_dma_read],
                             (uint16_t)(sizeof(ctx->rx_dma_buf) - ctx->rx_dma_read));
    }
    ctx->uart->RxState = HAL_UART_STATE_READY;
    ctx->uart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
    rs_bus_rx_start_dma(ctx);
    rs_bus_rx_irq_unlock(ctx->uart);
}

HAL_StatusTypeDef RsBus_SendFrame(RsBusContext *ctx,
                                  uint8_t addr,
                                  uint8_t seq,
                                  uint8_t flags,
                                  uint8_t cmd,
                                  const uint8_t *payload,
                                  uint16_t payload_len)
{
    uint8_t wire[RS_BUS_MAX_FRAME_SIZE + RS_BUS_TX_PAD_N];
    uint16_t frame_len;
    uint16_t wire_len;
    HAL_StatusTypeDef st;

    if (ctx == 0 || ctx->uart == 0) {
        return HAL_ERROR;
    }

    memset(wire, 0xFFu, RS_BUS_TX_PAD_N);
    frame_len = RsBus_FrameEncode(&wire[RS_BUS_TX_PAD_N], RS_BUS_MAX_FRAME_SIZE,
                                  addr, seq, flags, cmd, payload, payload_len);
    if (frame_len == 0u) {
        return HAL_ERROR;
    }
    wire_len = (uint16_t)(frame_len + RS_BUS_TX_PAD_N);

    rs_bus_rx_irq_lock(ctx->uart);
    rs_bus_rx_drain_pending(ctx);
    (void)HAL_UART_AbortReceive(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_SET);
    rs_bus_delay_us(RS_BUS_DE_LEAD_US);
    /* Не __disable_irq на весь TX: @250 кбод кадр до ~11 мс — SysTick/UI
     * отстают. RX — Receive_DMA без IDLE, щель между байтами допустима.
     * UART RX IRQ всё равно заблокирован (half-duplex). */
    st = HAL_UART_Transmit(ctx->uart, wire, wire_len, 50u);
    rs_bus_wait_tx_complete(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_RESET);
    rs_bus_post_tx_settle();
    rs_bus_rx_arm(ctx);
    rs_bus_rx_irq_unlock(ctx->uart);
    return st;
}

HAL_StatusTypeDef RsBus_SendRaw(RsBusContext *ctx, const uint8_t *frame, uint16_t frame_len)
{
    uint8_t wire[RS_BUS_MAX_FRAME_SIZE + RS_BUS_TX_PAD_N];
    uint16_t wire_len;
    HAL_StatusTypeDef st;

    if (ctx == 0 || ctx->uart == 0 || frame == 0 || frame_len == 0u) {
        return HAL_ERROR;
    }
    if (frame_len > RS_BUS_MAX_FRAME_SIZE) {
        return HAL_ERROR;
    }
    memset(wire, 0xFFu, RS_BUS_TX_PAD_N);
    memcpy(&wire[RS_BUS_TX_PAD_N], frame, frame_len);
    wire_len = (uint16_t)(frame_len + RS_BUS_TX_PAD_N);

    rs_bus_rx_irq_lock(ctx->uart);
    rs_bus_rx_drain_pending(ctx);
    (void)HAL_UART_AbortReceive(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_SET);
    rs_bus_delay_us(RS_BUS_DE_LEAD_US);
    st = HAL_UART_Transmit(ctx->uart, wire, wire_len, 50u);
    rs_bus_wait_tx_complete(ctx->uart);
    rs_bus_de_set(ctx, GPIO_PIN_RESET);
    rs_bus_post_tx_settle();
    rs_bus_rx_arm(ctx);
    rs_bus_rx_irq_unlock(ctx->uart);
    return st;
}
