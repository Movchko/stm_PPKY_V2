/**
  ******************************************************************************
  * @file    can_bus.c
  * @brief   CAN RX/TX модуль для двух шин:
  *          - приём, дедупликация и контроль ошибок по устройствам;
  *          - отправка через две независимые очереди (CAN1/CAN2).
  ******************************************************************************
  */

#include "can_bus.h"
#include "main.h"
#include "backend.h"
#include "device_config.h"
#include "service.h"
#include "tick_time.h"
#include "esp_manager.h"
#include "esp_protocol.h"
#include "log_transport.h"
#include "menu_ui.h"
#include "rs_panel_proto.h"
#include "app.hpp"
#include "stm32h5xx_hal.h"
#include <string.h>

#define CAN_RX_RING_SIZE      256
#define CAN_TX_RING_SIZE      256
#define CAN_NO_RX_TIMEOUT_MS  3000
#define CAN_DUP_WINDOW_MS     30
#define UART_BRIDGE_QUEUE_SIZE 128
#define LOG_UART_BODY_MAX     246u
#define UART_TX_PKT_MAX       256u

typedef struct {
	uint32_t id;
	uint8_t  data[8];
	uint8_t  can_bus;
} CanRxEntry;

typedef struct {
	uint32_t id;
	uint8_t  data[8];
} CanTxEntry;

static CanRxEntry        can_rx_ring[CAN_RX_RING_SIZE];
static volatile uint8_t  can_rx_head = 0;
static uint8_t           can_rx_tail = 0;

static CanTxEntry        can1_tx_ring[CAN_TX_RING_SIZE];
static volatile uint8_t  can1_tx_head = 0;
static volatile uint8_t  can1_tx_tail = 0;
static CanTxEntry        can2_tx_ring[CAN_TX_RING_SIZE];
static volatile uint8_t  can2_tx_head = 0;
static volatile uint8_t  can2_tx_tail = 0;

static volatile uint32_t last_rx_tick_can1 = 0;
static volatile uint32_t last_rx_tick_can2 = 0;

typedef struct {
	uint32_t id;
	uint8_t  data[8];
} UartRxFrame;

typedef struct {
	uint8_t  pkt[UART_TX_PKT_MAX];
	uint16_t len;
} UartTxPacket;

typedef enum {
	UART_RX_PREAMBLE_0 = 0,
	UART_RX_PREAMBLE_1,
	UART_RX_SIZE_LO,
	UART_RX_SIZE_HI,
	UART_RX_TYPE_LO,
	UART_RX_TYPE_HI,
	UART_RX_SEQ_LO,
	UART_RX_SEQ_HI,
	UART_RX_BODY,
	UART_RX_CRC_LO,
	UART_RX_CRC_HI
} UartRxState;

static UartRxFrame       uart_rx_ring[UART_BRIDGE_QUEUE_SIZE];
static volatile uint8_t  uart_rx_head = 0;
static volatile uint8_t  uart_rx_tail = 0;
static UartTxPacket      uart_tx_ring[UART_BRIDGE_QUEUE_SIZE];
static volatile uint8_t  uart_tx_head = 0;
static volatile uint8_t  uart_tx_tail = 0;
static volatile uint8_t  uart_tx_busy = 0;
static volatile uint8_t  uart_rx_started = 0;
static uint8_t           uart_rx_byte = 0;
static UartRxState       uart_rx_state = UART_RX_PREAMBLE_0;

static uint8_t           uart_body_buf[LOG_UART_BODY_MAX];
static uint16_t          uart_pkt_size = 0;
static uint16_t          uart_pkt_type = 0;
static uint16_t          uart_pkt_seq = 0;
static uint16_t          uart_body_total = 0;
static uint16_t          uart_body_pos = 0;
static uint16_t          uart_crc_acc = 0;
static uint8_t           uart_crc_lo = 0;

/** По каждому устройству: последний пакет с CAN1 и CAN2 (для дедупликации). ID = CAN_ID_NONE значит «ещё не было» */
#define CAN_ID_NONE  0xFFFFFFFFu
static uint32_t last_id_can1[CAN_MAX_DEVICES];
static uint8_t  last_data_can1[CAN_MAX_DEVICES][8];
static uint32_t last_id_can2[CAN_MAX_DEVICES];
static uint8_t  last_data_can2[CAN_MAX_DEVICES][8];
static uint8_t  can_init_done = 0;

/** Ожидание дубликата с другой шины: с какой шины ждём, до какого тика */
static uint8_t  pending_bus[CAN_MAX_DEVICES];
static uint32_t pending_timeout[CAN_MAX_DEVICES];

uint8_t can_bus_error_flags = 0;
uint8_t device_can_error[CAN_MAX_DEVICES] = {0};

/* Учёт веса физической позиции МКУ для последующего вычисления fault'ов. */
#define POSITION_MAX_HADR 32u
#define POSITION_RX_TIMEOUT_MS 3500u

typedef struct {
	uint8_t  w_can1;
	uint8_t  w_can2;
	uint8_t  has_can1;
	uint8_t  has_can2;
	uint32_t last_can1_ms;
	uint32_t last_can2_ms;
} PositionRxInfo;

static PositionRxInfo g_position_rx[POSITION_MAX_HADR + 1u];

typedef struct {
	uint32_t phase_since_ms;
	uint32_t snap_can1_ms;
	uint32_t snap_can2_ms;
	uint8_t rx_can1_cnt;
	uint8_t rx_can2_cnt;
	uint8_t latched;
} PositionFaultDebounce;

static PositionFaultDebounce g_position_debounce[POSITION_MAX_HADR + 1u];
static uint32_t g_position_fault_mask = 0u;

#define POSITION_FAULT_CONFIRM_MS 3000u
#define POSITION_FAULT_CONFIRM_RX 3u
#define POSITION_FAULT_CLEAR_MS 1000u
#define POSITION_FAULT_CLEAR_RX 2u

extern struct PPKYCfg PPKYConfig;

static void PositionRx_StoreWeight(uint8_t h_adr, uint8_t weight, uint8_t can_bus, uint32_t now_ms)
{
	PositionRxInfo *rx = &g_position_rx[h_adr];
	if (can_bus == CAN_BUS_1) {
		rx->w_can1 = weight;
		rx->has_can1 = 1u;
		rx->last_can1_ms = now_ms;
	} else {
		rx->w_can2 = weight;
		rx->has_can2 = 1u;
		rx->last_can2_ms = now_ms;
	}
}

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern UART_HandleTypeDef huart2;
extern uint8_t isMainInit;
extern Device BoardDevicesList[];
extern uint8_t nDevs;

extern ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
extern uint8_t g_active_devices_count;

static void CanTxEnqueue(uint32_t id, const uint8_t *data, uint8_t bus_mask);

static uint8_t ring_next_u8(uint8_t idx, uint8_t size)
{
	idx++;
	if (idx >= size) {
		idx = 0u;
	}
	return idx;
}

static void uart_rx_reset(void)
{
	uart_rx_state = UART_RX_PREAMBLE_0;
	uart_body_pos = 0u;
}

static uint8_t uart_frame_is_for_ppku(uint32_t msg_id)
{
	if (nDevs == 0u) {
		return 0u;
	}

	can_ext_id_t id;
	id.ID = msg_id;
	const Device *self = &BoardDevicesList[0];

	if ((id.field.d_type & 0x7Fu) != (self->d_type & 0x7Fu)) {
		return 0u;
	}

	uint8_t addr_match = ((id.field.zone & 0x7Fu) == (self->zone & 0x7Fu)) &&
	                     (id.field.h_adr == self->h_adr) &&
	                     ((id.field.l_adr & 0x3Fu) == (self->l_adr & 0x3Fu));
	uint8_t broadcast_match = (id.field.h_adr == 0u) && ((id.field.l_adr & 0x3Fu) == 0u);

	return (uint8_t)(addr_match || broadcast_match);
}

static void uart_rx_frame_push(uint32_t id, const uint8_t *data)
{
	uint8_t next = ring_next_u8(uart_rx_head, UART_BRIDGE_QUEUE_SIZE);
	if (next == uart_rx_tail) {
		uart_rx_tail = ring_next_u8(uart_rx_tail, UART_BRIDGE_QUEUE_SIZE);
	}

	uart_rx_ring[uart_rx_head].id = id;
	memcpy(uart_rx_ring[uart_rx_head].data, data, 8u);
	uart_rx_head = next;
}

static void uart_tx_bsu_push(uint16_t pkt_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	uint8_t next = ring_next_u8(uart_tx_head, UART_BRIDGE_QUEUE_SIZE);
	if (next == uart_tx_tail) {
		uart_tx_tail = ring_next_u8(uart_tx_tail, UART_BRIDGE_QUEUE_SIZE);
	}

	UartTxPacket *p = &uart_tx_ring[uart_tx_head];
	uint16_t len = BSU_PacketBuild(p->pkt, UART_TX_PKT_MAX, pkt_type, seq, payload, payload_len);
	if (len == 0u) {
		return;
	}
	p->len = len;
	uart_tx_head = next;
}

uint8_t UartBridge_SendBsuPacket(uint16_t pkt_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	if (!Esp32_IsEnabled() || payload_len > LOG_UART_BODY_MAX) {
		return 0u;
	}
	if (payload_len > 0u && payload == NULL) {
		return 0u;
	}
	uart_tx_bsu_push(pkt_type, seq, payload, payload_len);
	return 1u;
}

static void uart_tx_packet_push(uint8_t can_bus, uint32_t id, const uint8_t *data)
{
	uint8_t next = ring_next_u8(uart_tx_head, UART_BRIDGE_QUEUE_SIZE);
	if (next == uart_tx_tail) {
		uart_tx_tail = ring_next_u8(uart_tx_tail, UART_BRIDGE_QUEUE_SIZE);
	}

	UartTxPacket *p = &uart_tx_ring[uart_tx_head];
	uint16_t pos = 0u;
	p->pkt[pos++] = BSU_PKT_PREAMBLE_LO;
	p->pkt[pos++] = BSU_PKT_PREAMBLE_HI;
	p->pkt[pos++] = (uint8_t)(BSU_PKT_CAN_SIZE & 0xFFu);
	p->pkt[pos++] = (uint8_t)(BSU_PKT_CAN_SIZE >> 8);
	p->pkt[pos++] = (can_bus == CAN_BUS_2) ? (uint8_t)BSU_PKT_TYPE_CAN2 : (uint8_t)BSU_PKT_TYPE_CAN;
	p->pkt[pos++] = 0u;
	p->pkt[pos++] = 0u; /* seq lo */
	p->pkt[pos++] = 0u; /* seq hi */
	p->pkt[pos++] = (uint8_t)(id & 0xFFu);
	p->pkt[pos++] = (uint8_t)((id >> 8) & 0xFFu);
	p->pkt[pos++] = (uint8_t)((id >> 16) & 0xFFu);
	p->pkt[pos++] = (uint8_t)((id >> 24) & 0xFFu);
	memcpy(&p->pkt[pos], data, 8u);
	pos += 8u;
	uint16_t crc = BSU_Checksum(p->pkt, pos);
	p->pkt[pos++] = (uint8_t)(crc & 0xFFu);
	p->pkt[pos++] = (uint8_t)(crc >> 8);
	p->len = pos;

	uart_tx_head = next;
}

static void uart_bridge_on_rx_byte(uint8_t b)
{
	switch (uart_rx_state) {
	case UART_RX_PREAMBLE_0:
		if (b == BSU_PKT_PREAMBLE_LO) {
			uart_rx_state = UART_RX_PREAMBLE_1;
		}
		break;
	case UART_RX_PREAMBLE_1:
		if (b == BSU_PKT_PREAMBLE_HI) {
			uart_rx_state = UART_RX_SIZE_LO;
			uart_crc_acc = (uint16_t)(BSU_PKT_PREAMBLE_LO + BSU_PKT_PREAMBLE_HI);
		} else {
			uart_rx_state = UART_RX_PREAMBLE_0;
		}
		break;
	case UART_RX_SIZE_LO:
		uart_pkt_size = b;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_SIZE_HI;
		break;
	case UART_RX_SIZE_HI:
		uart_pkt_size |= (uint16_t)b << 8;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_TYPE_LO;
		break;
	case UART_RX_TYPE_LO:
		uart_pkt_type = b;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_TYPE_HI;
		break;
	case UART_RX_TYPE_HI:
		uart_pkt_type |= (uint16_t)b << 8;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_SEQ_LO;
		break;
	case UART_RX_SEQ_LO:
		uart_pkt_seq = b;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_SEQ_HI;
		break;
	case UART_RX_SEQ_HI:
		uart_pkt_seq |= (uint16_t)b << 8;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		if (uart_pkt_size < (BSU_PKT_HEADER_SIZE + BSU_PKT_CHECKSUM_SIZE) ||
		    uart_pkt_size > LOG_UART_BODY_MAX + BSU_PKT_HEADER_SIZE + BSU_PKT_CHECKSUM_SIZE) {
			uart_rx_reset();
			break;
		}
		uart_body_total = (uint16_t)(uart_pkt_size - BSU_PKT_HEADER_SIZE - BSU_PKT_CHECKSUM_SIZE);
		if (uart_pkt_type == BSU_PKT_TYPE_CAN || uart_pkt_type == BSU_PKT_TYPE_CAN2) {
			if (uart_body_total != BSU_PKT_CAN_PAYLOAD) {
				uart_rx_reset();
				break;
			}
		} else if (uart_pkt_type == LOG_PKT_TYPE_REQ) {
			if (uart_body_total > LOG_UART_BODY_MAX) {
				uart_rx_reset();
				break;
			}
		} else if (uart_pkt_type == BSU_PKT_TYPE_ESP_ACTIVITY) {
			if (uart_body_total != ESP_ACTIVITY_PAYLOAD_SIZE) {
				uart_rx_reset();
				break;
			}
		} else if (uart_pkt_type == BSU_PKT_TYPE_ESP_CAN) {
			if (uart_body_total != BSU_PKT_CAN_PAYLOAD) {
				uart_rx_reset();
				break;
			}
		} else if (uart_pkt_type == BSU_PKT_TYPE_ESP_UART) {
			if (uart_body_total == 0u || uart_body_total > ESP_UART_BODY_MAX) {
				uart_rx_reset();
				break;
			}
		} else {
			uart_rx_reset();
			break;
		}
		uart_body_pos = 0u;
		uart_rx_state = UART_RX_BODY;
		break;
	case UART_RX_BODY:
		uart_body_buf[uart_body_pos++] = b;
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		if (uart_body_pos >= uart_body_total) {
			uart_rx_state = UART_RX_CRC_LO;
		}
		break;
	case UART_RX_CRC_LO:
		uart_crc_lo = b;
		uart_rx_state = UART_RX_CRC_HI;
		break;
	case UART_RX_CRC_HI: {
		uint16_t recv_crc = (uint16_t)(uart_crc_lo | ((uint16_t)b << 8));
		uint16_t calc_crc = (uint16_t)(uart_crc_acc & 0xFFFFu);
		if (recv_crc == calc_crc) {
			if (uart_pkt_type == BSU_PKT_TYPE_CAN || uart_pkt_type == BSU_PKT_TYPE_CAN2) {
				uint32_t can_id = (uint32_t)uart_body_buf[0] |
				                  ((uint32_t)uart_body_buf[1] << 8) |
				                  ((uint32_t)uart_body_buf[2] << 16) |
				                  ((uint32_t)uart_body_buf[3] << 24);
				uart_rx_frame_push(can_id, &uart_body_buf[4]);
			} else if (uart_pkt_type == LOG_PKT_TYPE_REQ) {
				LogTransport_OnUart2LogRequest(uart_pkt_seq, uart_body_buf, uart_body_total);
			} else if (uart_pkt_type == BSU_PKT_TYPE_ESP_ACTIVITY) {
				EspManager_OnActivity(uart_body_buf, uart_body_total);
			} else if (uart_pkt_type == BSU_PKT_TYPE_ESP_UART) {
				/* ПО → ESP → ППКУ → RS485 (панель / бутлоадер). */
				(void)RsPanelMaster_InjectRawRsFrame(uart_body_buf, uart_body_total);
			}
		}
		uart_rx_reset();
		break;
	}
	default:
		uart_rx_reset();
		break;
	}
}

static void uart_bridge_rx_start(void)
{
	if (!Esp32_IsEnabled()) {
		return;
	}
	/* Если кто-то сделал блокирующий TX на huart2, RX IT мог умереть,
	 * а uart_rx_started остаться 1 — перевзводим по реальному RxState. */
	if (uart_rx_started != 0u && huart2.RxState == HAL_UART_STATE_BUSY) {
		return;
	}
	if (HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1u) == HAL_OK) {
		uart_rx_started = 1u;
	} else {
		uart_rx_started = 0u;
	}
}

static void uart_bridge_process_rx_frames(void)
{
	if (!Esp32_IsEnabled()) {
		return;
	}
	while (uart_rx_head != uart_rx_tail) {
		UartRxFrame *f = &uart_rx_ring[uart_rx_tail];
		uart_rx_tail = ring_next_u8(uart_rx_tail, UART_BRIDGE_QUEUE_SIZE);

		if (uart_frame_is_for_ppku(f->id)) {
			ProtocolParse(f->id, f->data, BUS_UART1);
		} else {
			/* Кадр пришёл из UART (WiFi/конвертер) и не адресован ППКУ напрямую:
			 * обрабатываем его локально так же, как обычный входящий кадр из CAN,
			 * затем ретранслируем в обе CAN-линии для кольца. */
			ProtocolParse(f->id, f->data, BUS_UART1);
			CanTxEnqueue(f->id, f->data, BUS_CAN12);
		}
	}
}

static void uart_bridge_process_tx(void)
{
	if (!Esp32_IsEnabled()) {
		return;
	}
	if (uart_tx_busy != 0u) {
		return;
	}
	if (uart_tx_head == uart_tx_tail) {
		return;
	}

	UartTxPacket *p = &uart_tx_ring[uart_tx_tail];
	if (HAL_UART_Transmit_IT(&huart2, p->pkt, p->len) == HAL_OK) {
		uart_tx_busy = 1u;
		uart_tx_tail = ring_next_u8(uart_tx_tail, UART_BRIDGE_QUEUE_SIZE);
	}
}

static void check_can_bus(FDCAN_HandleTypeDef *hfdcan)
{
	FDCAN_ProtocolStatusTypeDef protocolStatus = {};

	HAL_FDCAN_GetProtocolStatus(hfdcan, &protocolStatus);
	if (protocolStatus.BusOff) {
		uint16_t try = 0xFFFF;

		/* Правильный выход из Bus Off:
		 * 1) установить INIT
		 * 2) дождаться установки INIT
		 * 3) очистить INIT
		 * 4) дождаться выхода из INIT
		 */
		SET_BIT(hfdcan->Instance->CCCR, FDCAN_CCCR_INIT);
		while (((hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) == 0U) && (try--)) {}

		CLEAR_BIT(hfdcan->Instance->CCCR, FDCAN_CCCR_INIT);
		while (((hfdcan->Instance->CCCR & FDCAN_CCCR_INIT) != 0U) && (try--)) {}
	}
}

static void can_rx_push(uint32_t msg_id, const uint8_t *data, uint8_t can_bus)
{
	uint8_t next = can_rx_head + 1;
	if (next >= CAN_RX_RING_SIZE) {
		next = 0;
	}
	if (next == can_rx_tail) {
		can_rx_tail++;
		if (can_rx_tail >= CAN_RX_RING_SIZE) {
			can_rx_tail = 0;
		}
	}
	can_rx_ring[can_rx_head].id = msg_id;
	memcpy(can_rx_ring[can_rx_head].data, data, 8);
	can_rx_ring[can_rx_head].can_bus = can_bus;
	can_rx_head = next;

	if (can_bus == CAN_BUS_1) {
		last_rx_tick_can1 = HAL_GetTick();
	} else {
		last_rx_tick_can2 = HAL_GetTick();
	}
}

static void CanTxEnqueueOne(CanTxEntry *ring,
							volatile uint8_t *head,
							volatile uint8_t *tail,
							uint32_t id,
							const uint8_t *data)
{
	uint8_t next = (uint8_t)(*head + 1u);
	if (next >= CAN_TX_RING_SIZE) {
		next = 0u;
	}
	if (next == *tail) {
		(*tail)++;
		if (*tail >= CAN_TX_RING_SIZE) {
			*tail = 0u;
		}
	}

	ring[*head].id = id;
	for (uint8_t i = 0; i < 8u; i++) {
		ring[*head].data[i] = data[i];
	}
	*head = next;
}

/* Для BUS_CAN12 выбираем только одну линию:
 * приоритет CAN1, fallback CAN2, если CAN1 неактивен.
 * Неактивность определяется по can_bus_error_flags (бит выставлен = no-rx timeout). */
static uint8_t CanSelectSingleBusMask(uint8_t bus_mask)
{
	uint8_t has_can1 = ((bus_mask & BUS_CAN0) != 0u) ? 1u : 0u;
	uint8_t has_can2 = ((bus_mask & BUS_CAN1) != 0u) ? 1u : 0u;

	if (!(has_can1 && has_can2)) {
		return bus_mask;
	}

	uint8_t can1_active = ((can_bus_error_flags & 0x01u) == 0u) ? 1u : 0u;
	uint8_t can2_active = ((can_bus_error_flags & 0x02u) == 0u) ? 1u : 0u;

	if (can1_active) {
		return BUS_CAN0;
	}
	if (can2_active) {
		return BUS_CAN1;
	}

	/* Если обе линии сейчас неактивны — оставляем приоритет CAN1. */
	return BUS_CAN0;
}

static void CanTxEnqueue(uint32_t id, const uint8_t *data, uint8_t bus_mask)
{
	bus_mask = CanSelectSingleBusMask(bus_mask);

	if ((bus_mask & BUS_CAN0) != 0u) {
		CanTxEnqueueOne(can1_tx_ring, &can1_tx_head, &can1_tx_tail, id, data);
	}
	if ((bus_mask & BUS_CAN1) != 0u) {
		CanTxEnqueueOne(can2_tx_ring, &can2_tx_head, &can2_tx_tail, id, data);
	}
}

static void App_CanTxProcessBus(FDCAN_HandleTypeDef *hfdcan,
								CanTxEntry *ring,
								volatile uint8_t *head,
								volatile uint8_t *tail)
{
	uint8_t can_bus = (hfdcan == &hfdcan2) ? CAN_BUS_2 : CAN_BUS_1;
	while (*head != *tail) {
		CanTxEntry *e = &ring[*tail];
		FDCAN_TxHeaderTypeDef txHeader;

		txHeader.Identifier = e->id;
		txHeader.IdType = FDCAN_EXTENDED_ID;
		txHeader.TxFrameType = FDCAN_DATA_FRAME;
		txHeader.DataLength = FDCAN_DLC_BYTES_8;
		txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
		txHeader.BitRateSwitch = FDCAN_BRS_OFF;
		txHeader.FDFormat = FDCAN_CLASSIC_CAN;
		txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
		txHeader.MessageMarker = 0;

		if (HAL_FDCAN_GetTxFifoFreeLevel(hfdcan) == 0U) {
			check_can_bus(hfdcan);
			break;
		}
		if (HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &txHeader, e->data) != HAL_OK) {
			break;
		}
		/* Зеркалим исходящий кадр ППКУ в UART (BSU-обёртка). */
		uart_tx_packet_push(can_bus, e->id, e->data);

		(*tail)++;
		if (*tail >= CAN_TX_RING_SIZE) {
			*tail = 0u;
		}
	}
}

static void can_rx_drain_fifo(FDCAN_HandleTypeDef *hfdcan, uint32_t rx_fifo, uint8_t can_bus)
{
	(void)hfdcan;
	uint8_t data[8];
	FDCAN_RxHeaderTypeDef msg;
	while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, rx_fifo) > 0U) {
		if (HAL_FDCAN_GetRxMessage(hfdcan, rx_fifo, &msg, data) != HAL_OK) {
			break;
		}
		uart_tx_packet_push(can_bus, msg.Identifier, data);
		CanRxPush(msg.Identifier, data, can_bus);
	}
}

void CanRxPush(uint32_t id, const uint8_t *data, uint8_t can_bus)
{
	can_rx_push(id, data, can_bus);
}

void CanInit(void)
{
	for (uint16_t i = 0; i < CAN_MAX_DEVICES; i++) {
		last_id_can1[i] = CAN_ID_NONE;
		last_id_can2[i] = CAN_ID_NONE;
	}
	can_init_done = 1;
}

/* Кольцо целое (1), если ни у одного online МКУ с can_status_valid
 * нет КЗ (1) или обрыва (2) по CAN0/CAN1.
 */
uint8_t CanRingIsIntact(void)
{
	if (can_init_done == 0u) {
		return 1u;
	}

	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || !m->can_status_valid) {
			continue;
		}

		for (uint8_t can_idx = 0u; can_idx < 2u; can_idx++) {
			uint8_t shift = (uint8_t)(can_idx * 2u);
			uint8_t can_state = (uint8_t)((m->can_state_mask >> shift) & 0x3u);
			if (can_state == 1u || can_state == 2u) {
				return 0u;
			}
		}
	}

	return 1u;
}

void CanProcess(void)
{
	uint32_t now = HAL_GetTick();

	if (!can_init_done) {
		CanInit();
	}

	/* Флаги «нет приёма по шине» */
	if (now - last_rx_tick_can1 <= CAN_NO_RX_TIMEOUT_MS) {
		can_bus_error_flags &= ~(uint8_t)1;
	} else {
		can_bus_error_flags |= 1;
	}

	if (now - last_rx_tick_can2 <= CAN_NO_RX_TIMEOUT_MS) {
		can_bus_error_flags &= ~(uint8_t)2;
	} else {
		can_bus_error_flags |= 2;
	}

	/* Таймауты ожидания дубликата по каждому устройству */
	for (uint16_t d = 0; d < CAN_MAX_DEVICES; d++) {
		if (pending_timeout[d] == 0) {
			continue;
		}
		if (now <= pending_timeout[d]) {
			continue;
		}
		device_can_error[d] |= (uint8_t)(1 << (pending_bus[d] - 1));
		pending_timeout[d] = 0;
	}

	/* while */ if (can_rx_head != can_rx_tail) {
		CanRxEntry *e = &can_rx_ring[can_rx_tail];
		can_rx_tail++;
		if (can_rx_tail >= CAN_RX_RING_SIZE) {
			can_rx_tail = 0;
		}

		uint8_t dev;
		uint8_t other_bus;
		uint32_t *last_id_other;
		uint8_t  *last_data_other;
		uint32_t *last_id_cur;
		uint8_t  *last_data_cur;

		dev = CAN_DEVICE_INDEX(e->id);
		if (e->can_bus == CAN_BUS_1) {
			other_bus = CAN_BUS_2;
		} else {
			other_bus = CAN_BUS_1;
		}

		if (other_bus == CAN_BUS_1) {
			last_id_other = &last_id_can1[dev];
			last_data_other = last_data_can1[dev];
		} else {
			last_id_other = &last_id_can2[dev];
			last_data_other = last_data_can2[dev];
		}

		if (e->can_bus == CAN_BUS_1) {
			last_id_cur = &last_id_can1[dev];
			last_data_cur = last_data_can1[dev];
		} else {
			last_id_cur = &last_id_can2[dev];
			last_data_cur = last_data_can2[dev];
		}

		/* Дубликат с другой шины: тот же пакет уже пришёл с другой линии — не парсить, снять ожидание */
		if (*last_id_other != CAN_ID_NONE && e->id == *last_id_other && memcmp(e->data, last_data_other, 8) == 0) {
			*last_id_cur = e->id;
			memcpy(last_data_cur, e->data, 8);
			pending_timeout[dev] = 0;
			device_can_error[dev] &= (uint8_t)(~(1 << (e->can_bus - 1)));
			return; /* continue; */
		}

		/* Один и тот же пакет дважды с одной шины — пропустить */
		if (*last_id_cur != CAN_ID_NONE && e->id == *last_id_cur && memcmp(e->data, last_data_cur, 8) == 0) {
			return; /* continue; */
		}

		/* Уникальный пакет: разобрать один раз, ждать дубликат с другой шины */
		ProtocolParse(e->id, e->data, BUS_CAN12);
		App_PositionRxFromCan(e->id, e->data, e->can_bus, now);

		*last_id_cur = e->id;
		memcpy(last_data_cur, e->data, 8);

		pending_bus[dev] = other_bus;
		pending_timeout[dev] = now + CAN_DUP_WINDOW_MS;
	}
}

static uint8_t IsMcuDType(uint8_t d_type)
{
	return (d_type == DEVICE_MCU_IGN_TYPE ||
	        d_type == DEVICE_MCU_TC_TYPE ||
	        d_type == DEVICE_MCU_K1 ||
	        d_type == DEVICE_MCU_K2 ||
	        d_type == DEVICE_MCU_K3 ||
	        d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

void App_PositionRxFromCan(uint32_t msg_id, const uint8_t *msg_data, uint8_t can_bus, uint32_t now_ms)
{
	can_ext_id_t id;
	id.ID = msg_id;

	/* Мы обрабатываем только «ответ/статус» (dir != 0) для МКУ. */
	if (id.field.dir == 0u || !IsMcuDType((uint8_t)id.field.d_type)) {
		return;
	}
	if (msg_data == 0 || msg_data[0] != ServiceCmd_PositionDevice) {
		return;
	}

	uint8_t h_adr = (uint8_t)id.field.h_adr;
	if (h_adr == 0u || h_adr > POSITION_MAX_HADR) {
		return;
	}

	if (can_bus != CAN_BUS_1 && can_bus != CAN_BUS_2) {
		return;
	}

	PositionRx_StoreWeight(h_adr, msg_data[1], can_bus, now_ms);
}

static uint8_t Position_RxReady(const PositionRxInfo *rx, uint8_t can1_ok, uint8_t can2_ok, uint32_t now_ms)
{
	if (can1_ok && can2_ok) {
		return (rx->has_can1 != 0u && rx->has_can2 != 0u &&
			TickAgeWithinMs(now_ms, rx->last_can1_ms, POSITION_RX_TIMEOUT_MS) != 0u &&
			TickAgeWithinMs(now_ms, rx->last_can2_ms, POSITION_RX_TIMEOUT_MS) != 0u) ? 1u : 0u;
	}
	if (can1_ok) {
		return (rx->has_can1 != 0u &&
			TickAgeWithinMs(now_ms, rx->last_can1_ms, POSITION_RX_TIMEOUT_MS) != 0u) ? 1u : 0u;
	}
	if (can2_ok) {
		return (rx->has_can2 != 0u &&
			TickAgeWithinMs(now_ms, rx->last_can2_ms, POSITION_RX_TIMEOUT_MS) != 0u) ? 1u : 0u;
	}
	return 0u;
}

static uint8_t Position_WeightsMatch(const PositionRxInfo *rx, uint8_t exp_a, uint8_t exp_b,
				     uint8_t can1_ok, uint8_t can2_ok)
{
	if (can1_ok && can2_ok) {
		return (rx->w_can1 == exp_a && rx->w_can2 == exp_b) ? 1u : 0u;
	}
	if (can1_ok) {
		return (rx->w_can1 == exp_a) ? 1u : 0u;
	}
	if (can2_ok) {
		return (rx->w_can2 == exp_b) ? 1u : 0u;
	}
	return 0u;
}

static uint8_t Position_IsOnlineMcuByHadr(uint8_t h_adr)
{
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online) {
			continue;
		}
		if (!IsMcuDType(m->dev.d_type)) {
			continue;
		}
		if (m->dev.h_adr == h_adr) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t Position_CollectConfiguredHadr(uint8_t *out, uint8_t max_out)
{
	uint8_t n = 0u;
	for (uint8_t i = 0u; i < 32u && n < max_out; i++) {
		const Device *dv = &PPKYConfig.CfgDevices[i].UId.devId;
		uint8_t d_type = (uint8_t)(dv->d_type & 0x7Fu);
		if (!IsMcuDType(d_type)) {
			continue;
		}
		uint8_t h_adr = (uint8_t)dv->h_adr;
		if (h_adr == 0u || h_adr > POSITION_MAX_HADR) {
			continue;
		}
		uint8_t exists = 0u;
		for (uint8_t j = 0u; j < n; j++) {
			if (out[j] == h_adr) {
				exists = 1u;
				break;
			}
		}
		if (!exists) {
			out[n++] = h_adr;
		}
	}
	return n;
}

static uint8_t Position_IsCan1Healthy(void)
{
	return ((can_bus_error_flags & 0x01u) == 0u) ? 1u : 0u;
}

static uint8_t Position_IsCan2Healthy(void)
{
	return ((can_bus_error_flags & 0x02u) == 0u) ? 1u : 0u;
}

static void PositionDebounce_ResetPhase(PositionFaultDebounce *db)
{
	db->phase_since_ms = 0u;
	db->rx_can1_cnt = 0u;
	db->rx_can2_cnt = 0u;
	db->snap_can1_ms = 0u;
	db->snap_can2_ms = 0u;
}

static void PositionDebounce_StartPhase(PositionFaultDebounce *db, const PositionRxInfo *rx,
					uint32_t now_ms, uint8_t can1_ok, uint8_t can2_ok)
{
	db->phase_since_ms = now_ms;
	db->rx_can1_cnt = (can1_ok && rx->has_can1 != 0u) ? 1u : 0u;
	db->rx_can2_cnt = (can2_ok && rx->has_can2 != 0u) ? 1u : 0u;
	db->snap_can1_ms = rx->last_can1_ms;
	db->snap_can2_ms = rx->last_can2_ms;
}

static void PositionDebounce_CountNewRx(PositionFaultDebounce *db, const PositionRxInfo *rx,
					uint8_t can1_ok, uint8_t can2_ok)
{
	if (can1_ok && rx->has_can1 != 0u && rx->last_can1_ms != db->snap_can1_ms) {
		db->snap_can1_ms = rx->last_can1_ms;
		if (db->rx_can1_cnt < 255u) {
			db->rx_can1_cnt++;
		}
	}
	if (can2_ok && rx->has_can2 != 0u && rx->last_can2_ms != db->snap_can2_ms) {
		db->snap_can2_ms = rx->last_can2_ms;
		if (db->rx_can2_cnt < 255u) {
			db->rx_can2_cnt++;
		}
	}
}

static uint8_t PositionDebounce_RxEnough(const PositionFaultDebounce *db, uint8_t can1_ok,
					 uint8_t can2_ok, uint8_t need)
{
	if (can1_ok && can2_ok) {
		return (db->rx_can1_cnt >= need && db->rx_can2_cnt >= need) ? 1u : 0u;
	}
	if (can1_ok) {
		return (db->rx_can1_cnt >= need) ? 1u : 0u;
	}
	if (can2_ok) {
		return (db->rx_can2_cnt >= need) ? 1u : 0u;
	}
	return 0u;
}

static uint8_t PositionDebounce_PhaseDone(const PositionFaultDebounce *db, uint32_t now_ms,
					 uint32_t need_ms, uint8_t can1_ok, uint8_t can2_ok,
					 uint8_t need_rx)
{
	if (db->phase_since_ms == 0u) {
		return 0u;
	}
	if (!TickAgeExpiredMs(now_ms, db->phase_since_ms, need_ms)) {
		return 0u;
	}
	return PositionDebounce_RxEnough(db, can1_ok, can2_ok, need_rx);
}

void Position_EvaluateMismatch(uint32_t now_ms)
{
	uint8_t hadrs[POSITION_MAX_HADR];
	uint8_t n = Position_CollectConfiguredHadr(hadrs, POSITION_MAX_HADR);
	uint8_t can1_ok = Position_IsCan1Healthy();
	uint8_t can2_ok = Position_IsCan2Healthy();
	uint32_t new_mask = 0u;

	if (n == 0u || (!can1_ok && !can2_ok)) {
		memset(g_position_debounce, 0, sizeof(g_position_debounce));
		g_position_fault_mask = 0u;
		return;
	}

	for (uint8_t i = 0u; i < n; i++) {
		uint8_t h_adr = hadrs[i];
		PositionFaultDebounce *db = &g_position_debounce[h_adr];

		if (!Position_IsOnlineMcuByHadr(h_adr)) {
			PositionDebounce_ResetPhase(db);
			db->latched = 0u;
			continue;
		}

		const PositionRxInfo *rx = &g_position_rx[h_adr];
		if (!Position_RxReady(rx, can1_ok, can2_ok, now_ms)) {
			if (db->latched != 0u) {
				new_mask |= (1u << (h_adr - 1u));
			}
			continue;
		}

		uint8_t exp_a = i;
		uint8_t exp_b = (uint8_t)(n - 1u - i);
		uint8_t match = Position_WeightsMatch(rx, exp_a, exp_b, can1_ok, can2_ok);

		if (match == 0u) {
			if (db->latched == 0u) {
				if (db->phase_since_ms == 0u) {
					PositionDebounce_StartPhase(db, rx, now_ms, can1_ok, can2_ok);
				} else {
					PositionDebounce_CountNewRx(db, rx, can1_ok, can2_ok);
				}
				if (PositionDebounce_PhaseDone(db, now_ms, POSITION_FAULT_CONFIRM_MS,
							       can1_ok, can2_ok,
							       POSITION_FAULT_CONFIRM_RX) != 0u) {
					db->latched = 1u;
					PositionDebounce_ResetPhase(db);
				}
			} else {
				PositionDebounce_ResetPhase(db);
			}
		} else {
			if (db->latched != 0u) {
				if (db->phase_since_ms == 0u) {
					PositionDebounce_StartPhase(db, rx, now_ms, can1_ok, can2_ok);
				} else {
					PositionDebounce_CountNewRx(db, rx, can1_ok, can2_ok);
				}
				if (PositionDebounce_PhaseDone(db, now_ms, POSITION_FAULT_CLEAR_MS,
							       can1_ok, can2_ok,
							       POSITION_FAULT_CLEAR_RX) != 0u) {
					db->latched = 0u;
					PositionDebounce_ResetPhase(db);
				}
			} else {
				PositionDebounce_ResetPhase(db);
			}
		}

		if (db->latched != 0u) {
			new_mask |= (1u << (h_adr - 1u));
		}
	}

	g_position_fault_mask = new_mask;
}

uint32_t Position_GetFaultMask(void)
{
	return g_position_fault_mask;
}

void App_CanTxProcess(void)
{
	if (isMainInit == 0u) {
		return;
	}

	uart_bridge_rx_start();
	uart_bridge_process_rx_frames();
	uart_bridge_process_tx();

	App_CanTxProcessBus(&hfdcan1, can1_tx_ring, &can1_tx_head, &can1_tx_tail);
	App_CanTxProcessBus(&hfdcan2, can2_tx_ring, &can2_tx_head, &can2_tx_tail);
}

void CANSendData(uint8_t *Buf)
{
	if (isMainInit == 0u) {
		return;
	}

	/* Buf layout как в backend:
	 *  [0..3]   -> uint32_t id
	 *  [4..11]  -> 8 байт данных
	 *  [12]     -> bus_mask (BUS_CAN0/BUS_CAN1)
	 */
	uint32_t id = (*(uint32_t *)Buf);
	const uint8_t *data = &Buf[4];
	uint8_t bus_mask = Buf[4 + 8];

	CanTxEnqueue(id, data, bus_mask);
}

void UARTSendData(uint8_t *Buf)
{
	if (isMainInit == 0u) {
		return;
	}

	uint32_t id = (*(uint32_t *)Buf);
	const uint8_t *data = &Buf[4];
	uart_tx_packet_push(CAN_BUS_1, id, data);
}

void UartBridge_Stop(void)
{
	(void)HAL_UART_AbortReceive_IT(&huart2);
	(void)HAL_UART_AbortTransmit_IT(&huart2);
	uart_tx_busy = 0u;
	uart_rx_started = 0u;
	uart_tx_head = uart_tx_tail;
	uart_rx_head = uart_rx_tail;
	uart_rx_reset();
}

uint8_t UartBridge_IsTxIdle(void)
{
	return (uint8_t)((uart_tx_busy == 0u) && (uart_tx_head == uart_tx_tail));
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifoITs)
{
	(void)RxFifoITs;
	if (hfdcan == &hfdcan1) {
		can_rx_drain_fifo(hfdcan, FDCAN_RX_FIFO0, CAN_BUS_1);
	} else if (hfdcan == &hfdcan2) {
		can_rx_drain_fifo(hfdcan, FDCAN_RX_FIFO0, CAN_BUS_2);
	}
}

void HAL_FDCAN_RxFifo1Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifoITs)
{
	(void)RxFifoITs;
	if (hfdcan == &hfdcan1) {
		can_rx_drain_fifo(hfdcan, FDCAN_RX_FIFO1, CAN_BUS_1);
	} else if (hfdcan == &hfdcan2) {
		can_rx_drain_fifo(hfdcan, FDCAN_RX_FIFO1, CAN_BUS_2);
	}
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
	if ((ErrorStatusITs & FDCAN_IT_BUS_OFF) != RESET) {
		check_can_bus(hfdcan);
	}
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart != &huart2) {
		return;
	}

	uart_bridge_on_rx_byte(uart_rx_byte);
	(void)HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1u);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart == &huart2) {
		uart_tx_busy = 0u;
		LogTransport_OnUartTxComplete(huart);
	}
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart != &huart2) {
		return;
	}

	uart_tx_busy = 0u;
	uart_rx_started = 0u;
	uart_rx_reset();
	LogTransport_OnUartError(huart);

	/* Сброс FE/NE/ORE и слив RDR — иначе Receive_IT часто не встаёт снова. */
	__HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_OREF);
	if (__HAL_UART_GET_FLAG(huart, UART_FLAG_RXNE) != 0u) {
		volatile uint32_t discard = huart->Instance->RDR;
		(void)discard;
	}
	huart->ErrorCode = HAL_UART_ERROR_NONE;
	huart->RxState = HAL_UART_STATE_READY;

	if (Esp32_IsEnabled() && HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1u) == HAL_OK) {
		uart_rx_started = 1u;
	}
}
