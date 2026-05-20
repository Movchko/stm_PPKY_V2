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
#include "stm32h5xx_hal.h"
#include <string.h>

#define CAN_RX_RING_SIZE      256
#define CAN_TX_RING_SIZE      256
#define CAN_NO_RX_TIMEOUT_MS  3000
#define CAN_DUP_WINDOW_MS     30
#define UART_BRIDGE_QUEUE_SIZE 128

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
	uint8_t  pkt[BSU_PKT_CAN_SIZE];
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
static uint8_t           uart_body_buf[BSU_PKT_CAN_PAYLOAD];
static uint16_t          uart_pkt_size = 0;
static uint16_t          uart_pkt_type = 0;
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

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern UART_HandleTypeDef huart2;
extern uint8_t isMainInit;
extern Device BoardDevicesList[];
extern uint8_t nDevs;
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
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		uart_rx_state = UART_RX_SEQ_HI;
		break;
	case UART_RX_SEQ_HI:
		uart_crc_acc = (uint16_t)(uart_crc_acc + b);
		if (uart_pkt_size < BSU_PKT_CAN_SIZE) {
			uart_rx_reset();
			break;
		}
		uart_body_total = (uint16_t)(uart_pkt_size - BSU_PKT_HEADER_SIZE - BSU_PKT_CHECKSUM_SIZE);
		if (uart_body_total != BSU_PKT_CAN_PAYLOAD) {
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
		if (recv_crc == calc_crc &&
		    (uart_pkt_type == BSU_PKT_TYPE_CAN || uart_pkt_type == BSU_PKT_TYPE_CAN2)) {
			uint32_t can_id = (uint32_t)uart_body_buf[0] |
			                  ((uint32_t)uart_body_buf[1] << 8) |
			                  ((uint32_t)uart_body_buf[2] << 16) |
			                  ((uint32_t)uart_body_buf[3] << 24);
			uart_rx_frame_push(can_id, &uart_body_buf[4]);
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
	if (uart_rx_started != 0u) {
		return;
	}
	if (HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1u) == HAL_OK) {
		uart_rx_started = 1u;
	}
}

static void uart_bridge_process_rx_frames(void)
{
	while (uart_rx_head != uart_rx_tail) {
		UartRxFrame *f = &uart_rx_ring[uart_rx_tail];
		uart_rx_tail = ring_next_u8(uart_rx_tail, UART_BRIDGE_QUEUE_SIZE);

		if (uart_frame_is_for_ppku(f->id)) {
			ProtocolParse(f->id, f->data, BUS_CAN12);
		} else {
			/* Кадр пришёл из UART (WiFi/конвертер) и не адресован ППКУ напрямую:
			 * обрабатываем его локально так же, как обычный входящий кадр из CAN,
			 * затем ретранслируем в обе CAN-линии для кольца. */
			ProtocolParse(f->id, f->data, BUS_CAN12);
			CanTxEnqueue(f->id, f->data, BUS_CAN12);
		}
	}
}

static void uart_bridge_process_tx(void)
{
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

static void CanTxEnqueue(uint32_t id, const uint8_t *data, uint8_t bus_mask)
{
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

		*last_id_cur = e->id;
		memcpy(last_data_cur, e->data, 8);

		pending_bus[dev] = other_bus;
		pending_timeout[dev] = now + CAN_DUP_WINDOW_MS;
	}
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
	}
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if (huart != &huart2) {
		return;
	}

	uart_tx_busy = 0u;
	uart_rx_started = 0u;
	(void)HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1u);
	uart_rx_started = 1u;
}
