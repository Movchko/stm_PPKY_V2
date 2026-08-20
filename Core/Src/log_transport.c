#include "log_transport.h"
#include "backend.h"
#include "event_log.h"
#include "main.h"
#include "log_stream.h"
#include "can_bus.h"

#include <string.h>

#define LOG_TX_QUEUE_SIZE              8u
#define LOG_UART_BODY_MAX              246u

typedef struct {
	uint8_t data[LOG_STREAM_FRAME_MAX];
	uint16_t len;
	LogTransportPort_t port;
} LogTxEntry_t;

static LogTxEntry_t s_tx_queue[LOG_TX_QUEUE_SIZE];
static volatile uint8_t s_tx_head = 0u;
static volatile uint8_t s_tx_tail = 0u;
static volatile uint8_t s_uart2_log_tx_busy = 0u;
static uint8_t s_uart2_tx_buf[LOG_STREAM_FRAME_MAX];
static uint16_t s_uart2_tx_len = 0u;

extern UART_HandleTypeDef huart2;

static uint8_t log_tx_ring_next(uint8_t idx) { idx++; if (idx >= LOG_TX_QUEUE_SIZE) idx = 0u; return idx; }
static void log_transport_try_tx_uart2(void);
static void log_transport_pump_stream(void);

static uint16_t log_transport_build_frame(uint8_t *out, uint16_t out_max, uint16_t pkt_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	uint16_t pkt_size, pos, crc;
	if (out == NULL || payload_len > LOG_UART_BODY_MAX) return 0u;
	pkt_size = (uint16_t)(BSU_PKT_HEADER_SIZE + payload_len + BSU_PKT_CHECKSUM_SIZE);
	if (pkt_size > out_max || pkt_size > LOG_STREAM_FRAME_MAX) return 0u;
	pos = 0u;
	out[pos++] = BSU_PKT_PREAMBLE_LO; out[pos++] = BSU_PKT_PREAMBLE_HI;
	out[pos++] = (uint8_t)(pkt_size & 0xFFu); out[pos++] = (uint8_t)(pkt_size >> 8);
	out[pos++] = (uint8_t)(pkt_type & 0xFFu); out[pos++] = (uint8_t)(pkt_type >> 8);
	out[pos++] = (uint8_t)(seq & 0xFFu); out[pos++] = (uint8_t)(seq >> 8);
	if (payload_len > 0u && payload != NULL) { memcpy(&out[pos], payload, payload_len); pos = (uint16_t)(pos + payload_len); }
	crc = BSU_Checksum(out, pos); out[pos++] = (uint8_t)(crc & 0xFFu); out[pos++] = (uint8_t)(crc >> 8);
	return pkt_size;
}

static void log_transport_send_cb(LogTransportPort_t port, uint16_t pkt_type, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	uint8_t next = log_tx_ring_next(s_tx_head);
	LogTxEntry_t *entry;
	if (next == s_tx_tail) s_tx_tail = log_tx_ring_next(s_tx_tail);
	entry = &s_tx_queue[s_tx_head];
	entry->port = port;
	entry->len = log_transport_build_frame(entry->data, sizeof(entry->data), pkt_type, seq, payload, payload_len);
	if (entry->len == 0u) return;
	s_tx_head = next;
}

static uint8_t uart_bridge_channel_is_free(void) { return UartBridge_IsTxIdle(); }

static void log_transport_pump_stream(void)
{
	uint8_t budget = 16u;
	log_transport_try_tx_uart2();
	while (budget-- != 0u) {
		if (LogStream_IsDumpActive()) LogStream_Process();
		log_transport_try_tx_uart2();
		if (!LogStream_IsDumpActive()) break;
	}
}

static void log_transport_handle_log_request(LogTransportPort_t port, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	LogStream_HandleRequest(port, seq, payload, payload_len);
	log_transport_pump_stream();
	EventLog_LogHostLink((port == LOG_PORT_UART4) ? 1u : 0u);
}

static void log_transport_try_tx_uart2(void)
{
	LogTxEntry_t *entry;
	if (s_uart2_log_tx_busy != 0u || !uart_bridge_channel_is_free()) return;
	while (s_tx_head != s_tx_tail) {
		entry = &s_tx_queue[s_tx_tail];
		if (entry->port != LOG_PORT_UART2 || entry->len == 0u) { s_tx_tail = log_tx_ring_next(s_tx_tail); continue; }
		if (entry->len > sizeof(s_uart2_tx_buf)) { s_tx_tail = log_tx_ring_next(s_tx_tail); continue; }
		memcpy(s_uart2_tx_buf, entry->data, entry->len);
		s_uart2_tx_len = entry->len;
		s_tx_tail = log_tx_ring_next(s_tx_tail);
		if (HAL_UART_Transmit_IT(&huart2, s_uart2_tx_buf, s_uart2_tx_len) == HAL_OK) s_uart2_log_tx_busy = 1u;
		return;
	}
}

void LogTransport_Init(void)
{
	s_tx_head = 0u; s_tx_tail = 0u; s_uart2_log_tx_busy = 0u;
	LogStream_Init(log_transport_send_cb);
}

void LogTransport_Process(void)
{
	uint8_t budget = 4u;
	log_transport_try_tx_uart2();
	while (budget-- != 0u && LogStream_IsDumpActive()) {
		uint8_t next = log_tx_ring_next(s_tx_head);
		if (next == s_tx_tail) break;
		LogStream_Process();
	}
	log_transport_try_tx_uart2();
}

void LogTransport_OnUart2LogRequest(uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	log_transport_handle_log_request(LOG_PORT_UART2, seq, payload, payload_len);
}

void LogTransport_OnUartRxByte(UART_HandleTypeDef *huart) { (void)huart; }

void LogTransport_OnUartTxComplete(UART_HandleTypeDef *huart)
{
	if (huart == &huart2 && s_uart2_log_tx_busy != 0u) {
		s_uart2_log_tx_busy = 0u;
		log_transport_try_tx_uart2();
	}
}

void LogTransport_OnUartError(UART_HandleTypeDef *huart)
{
	if (huart == &huart2) s_uart2_log_tx_busy = 0u;
}
