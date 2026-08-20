#include "log_stream.h"
#include "event_log_reader.h"
#include "event_log.h"

#include <string.h>

#define LOG_PKT_TYPE_RSP               17u
#define LOG_PKT_TYPE_DATA              18u
#define LOG_STREAM_HDR_SIZE            16u
#define LOG_STREAM_REC_SIZE            38u
#define LOG_STREAM_MAX_REC_PER_PKT     6u
#define LOG_STREAM_BSU_PAYLOAD_MAX     246u
#define LOG_PING_RSP_SIZE              4u
#define LOG_GET_INFO_RSP_SIZE          32u
#define LOG_START_DUMP_RSP_SIZE        16u
#define LOG_STOP_DUMP_RSP_SIZE         2u
#define LOG_GET_STATUS_RSP_SIZE        11u

typedef struct {
	uint8_t active;
	LogTransportPort_t port;
	uint16_t stream_id;
	uint16_t pkt_num;
	uint16_t pkt_sent;
	uint32_t records_sent;
	uint8_t dump_tier_mode;
	uint8_t cur_tier;
	uint8_t first_packet;
	uint32_t next_logical;
	uint32_t end_logical_crit;
	uint32_t end_logical_gen;
	uint32_t write_head_crit;
	uint32_t write_head_gen;
	uint32_t total_records;
} LogStreamSession_t;

static LogStream_SendFn s_send_fn;
static LogStreamSession_t s_session;
static uint16_t s_next_stream_id = 1u;

static void put_u16_le(uint8_t *buf, uint16_t value) { buf[0] = (uint8_t)(value & 0xFFu); buf[1] = (uint8_t)(value >> 8); }
static void put_u32_le(uint8_t *buf, uint32_t value) { buf[0]=(uint8_t)(value&0xFFu); buf[1]=(uint8_t)((value>>8)&0xFFu); buf[2]=(uint8_t)((value>>16)&0xFFu); buf[3]=(uint8_t)((value>>24)&0xFFu); }
static void LogStream_SendRsp(LogTransportPort_t port, uint16_t seq, const uint8_t *payload, uint16_t len) { if (s_send_fn != NULL) s_send_fn(port, LOG_PKT_TYPE_RSP, seq, payload, len); }
static void LogStream_SendData(LogTransportPort_t port, uint16_t seq, const uint8_t *payload, uint16_t len) { if (s_send_fn != NULL) s_send_fn(port, LOG_PKT_TYPE_DATA, seq, payload, len); }
static void LogStream_ResetSession(void) { memset(&s_session, 0, sizeof(s_session)); }
static uint32_t LogStream_CurrentTierEnd(void) { return (s_session.cur_tier == LOG_TIER_CRITICAL) ? s_session.end_logical_crit : s_session.end_logical_gen; }
static uint32_t LogStream_CurrentWriteHead(void) { return (s_session.cur_tier == LOG_TIER_CRITICAL) ? s_session.write_head_crit : s_session.write_head_gen; }

static void LogStream_HandlePing(LogTransportPort_t port, uint16_t seq)
{
	uint8_t rsp[LOG_PING_RSP_SIZE] = { LOG_STATUS_OK, LOG_OPCODE_PING, LOG_STREAM_PROTO_VER, 32u };
	LogStream_SendRsp(port, seq, rsp, sizeof(rsp));
}

static void LogStream_HandleGetInfo(LogTransportPort_t port, uint16_t seq)
{
	EventLogTierInfo_t crit;
	EventLogTierInfo_t gen;
	uint8_t rsp[LOG_GET_INFO_RSP_SIZE];

	memset(rsp, 0, sizeof(rsp));
	if (!EventLog_IsInitialized()) {
		rsp[0] = LOG_STATUS_NOT_INIT;
		rsp[1] = LOG_OPCODE_GET_INFO;
		LogStream_SendRsp(port, seq, rsp, 2u);
		return;
	}
	if (!EventLogReader_GetTierInfo(LOG_TIER_CRITICAL, &crit)) memset(&crit, 0, sizeof(crit));
	if (!EventLogReader_GetTierInfo(LOG_TIER_GENERAL, &gen)) memset(&gen, 0, sizeof(gen));

	rsp[0] = LOG_STATUS_OK; rsp[1] = LOG_OPCODE_GET_INFO; rsp[2] = LOG_STREAM_PROTO_VER; rsp[3] = 32u;
	put_u32_le(&rsp[4], crit.capacity); put_u32_le(&rsp[8], crit.count); put_u32_le(&rsp[12], crit.write_head);
	put_u32_le(&rsp[16], gen.capacity); put_u32_le(&rsp[20], gen.count); put_u32_le(&rsp[24], gen.write_head);
	put_u32_le(&rsp[28], EventLogReader_GetCatalogCrc32());
	LogStream_SendRsp(port, seq, rsp, sizeof(rsp));
}

static uint8_t LogStream_StartDump(LogTransportPort_t port, uint16_t seq, uint8_t tier, uint32_t start_logical, uint8_t rsp[LOG_START_DUMP_RSP_SIZE])
{
	EventLogTierInfo_t crit, gen;
	uint32_t total = 0u;
	(void)seq;
	memset(rsp, 0, LOG_START_DUMP_RSP_SIZE);
	rsp[1] = LOG_OPCODE_START_DUMP;
	if (!EventLog_IsInitialized()) { rsp[0] = LOG_STATUS_NOT_INIT; return 0u; }
	if (tier > LOG_TIER_BOTH) { rsp[0] = LOG_STATUS_BAD_PARAM; return 0u; }
	if (s_session.active != 0u) { rsp[0] = LOG_STATUS_BUSY; return 0u; }
	if (!EventLogReader_GetTierInfo(LOG_TIER_CRITICAL, &crit) || !EventLogReader_GetTierInfo(LOG_TIER_GENERAL, &gen)) { rsp[0] = LOG_STATUS_INTERNAL; return 0u; }

	LogStream_ResetSession();
	s_session.port = port; s_session.dump_tier_mode = tier; s_session.write_head_crit = crit.write_head; s_session.write_head_gen = gen.write_head;
	s_session.first_packet = 1u; s_session.stream_id = s_next_stream_id++; if (s_next_stream_id == 0u) s_next_stream_id = 1u;

	if (tier == LOG_TIER_CRITICAL) {
		if (start_logical > crit.count) { rsp[0] = LOG_STATUS_OUT_OF_RANGE; return 0u; }
		s_session.cur_tier = LOG_TIER_CRITICAL; s_session.next_logical = start_logical; s_session.end_logical_crit = crit.count; total = crit.count - start_logical;
	} else if (tier == LOG_TIER_GENERAL) {
		if (start_logical > gen.count) { rsp[0] = LOG_STATUS_OUT_OF_RANGE; return 0u; }
		s_session.cur_tier = LOG_TIER_GENERAL; s_session.next_logical = start_logical; s_session.end_logical_gen = gen.count; total = gen.count - start_logical;
	} else {
		if (start_logical > (crit.count + gen.count)) { rsp[0] = LOG_STATUS_OUT_OF_RANGE; return 0u; }
		s_session.end_logical_crit = crit.count; s_session.end_logical_gen = gen.count;
		if (start_logical < crit.count) { s_session.cur_tier = LOG_TIER_CRITICAL; s_session.next_logical = start_logical; total = (crit.count - start_logical) + gen.count; }
		else { s_session.cur_tier = LOG_TIER_GENERAL; s_session.next_logical = start_logical - crit.count; total = (gen.count > s_session.next_logical) ? (gen.count - s_session.next_logical) : 0u; }
	}

	s_session.total_records = total;
	rsp[0] = LOG_STATUS_OK; put_u16_le(&rsp[2], s_session.stream_id); put_u32_le(&rsp[4], total); put_u32_le(&rsp[8], crit.write_head); put_u32_le(&rsp[12], gen.write_head);
	if (total == 0u) { LogStream_ResetSession(); return 1u; }
	s_session.active = 1u;
	return 1u;
}

static void LogStream_HandleStopDump(LogTransportPort_t port, uint16_t seq) { uint8_t rsp[LOG_STOP_DUMP_RSP_SIZE] = { LOG_STATUS_OK, LOG_OPCODE_STOP_DUMP }; LogStream_ResetSession(); LogStream_SendRsp(port, seq, rsp, sizeof(rsp)); }
static void LogStream_HandleGetDumpStatus(LogTransportPort_t port, uint16_t seq) { uint8_t rsp[LOG_GET_STATUS_RSP_SIZE] = {0}; rsp[0]=LOG_STATUS_OK; rsp[1]=LOG_OPCODE_GET_DUMP_STATUS; rsp[2]=s_session.active; put_u16_le(&rsp[3], s_session.stream_id); put_u16_le(&rsp[5], s_session.pkt_sent); put_u32_le(&rsp[7], s_session.records_sent); LogStream_SendRsp(port, seq, rsp, sizeof(rsp)); }

void LogStream_Init(LogStream_SendFn send_fn) { s_send_fn = send_fn; LogStream_ResetSession(); }

void LogStream_HandleRequest(LogTransportPort_t port, uint16_t seq, const uint8_t *payload, uint16_t payload_len)
{
	uint8_t opcode;
	if (payload == NULL || payload_len == 0u) return;
	opcode = payload[0];
	switch (opcode) {
	case LOG_OPCODE_PING: LogStream_HandlePing(port, seq); break;
	case LOG_OPCODE_GET_INFO: LogStream_HandleGetInfo(port, seq); break;
	case LOG_OPCODE_START_DUMP: {
		uint8_t rsp[LOG_START_DUMP_RSP_SIZE];
		if (payload_len < 6u) { rsp[0] = LOG_STATUS_BAD_PARAM; rsp[1] = LOG_OPCODE_START_DUMP; LogStream_SendRsp(port, seq, rsp, 2u); break; }
		uint8_t tier = payload[1];
		uint32_t start_logical = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8) | ((uint32_t)payload[4] << 16) | ((uint32_t)payload[5] << 24);
		if (LogStream_StartDump(port, seq, tier, start_logical, rsp)) LogStream_SendRsp(port, seq, rsp, sizeof(rsp));
		else LogStream_SendRsp(port, seq, rsp, (rsp[0] == LOG_STATUS_OK) ? sizeof(rsp) : 2u);
		break;
	}
	case LOG_OPCODE_STOP_DUMP: LogStream_HandleStopDump(port, seq); break;
	case LOG_OPCODE_GET_DUMP_STATUS: LogStream_HandleGetDumpStatus(port, seq); break;
	default: { uint8_t rsp[2] = { LOG_STATUS_BAD_PARAM, opcode }; LogStream_SendRsp(port, seq, rsp, sizeof(rsp)); break; }
	}
}

bool LogStream_IsDumpActive(void) { return (s_session.active != 0u); }

void LogStream_Process(void)
{
	uint8_t payload[LOG_STREAM_BSU_PAYLOAD_MAX];
	EventLogRecord_t record;
	if (s_session.active == 0u) return;

	uint32_t tier_end = LogStream_CurrentTierEnd();
	if (s_session.next_logical >= tier_end) {
		if (s_session.dump_tier_mode == LOG_TIER_BOTH && s_session.cur_tier == LOG_TIER_CRITICAL && s_session.end_logical_gen > 0u) {
			s_session.cur_tier = LOG_TIER_GENERAL; s_session.next_logical = 0u; tier_end = s_session.end_logical_gen;
		} else { s_session.active = 0u; return; }
	}

	memset(payload, 0, sizeof(payload));
	uint8_t rec_count = 0u, flags = 0u, read_error = 0u;
	uint32_t first_logical = s_session.next_logical;
	uint16_t payload_len = LOG_STREAM_HDR_SIZE;

	while (rec_count < LOG_STREAM_MAX_REC_PER_PKT && (payload_len + LOG_STREAM_REC_SIZE) <= LOG_STREAM_BSU_PAYLOAD_MAX && s_session.next_logical < tier_end) {
		EventLogRecStatus_t status;
		if (!EventLogReader_ReadLogical(s_session.cur_tier, s_session.next_logical, &status, &record)) { read_error = 1u; break; }
		put_u32_le(&payload[payload_len], s_session.next_logical);
		payload[payload_len + 4u] = (uint8_t)status;
		payload[payload_len + 5u] = s_session.cur_tier;
		memcpy(&payload[payload_len + 6u], &record, sizeof(record));
		payload_len = (uint16_t)(payload_len + LOG_STREAM_REC_SIZE);
		rec_count++; s_session.next_logical++; s_session.records_sent++;
	}
	if (rec_count == 0u) { s_session.active = 0u; return; }
	if (s_session.first_packet != 0u) { flags |= LOG_STREAM_FLAG_FIRST; s_session.first_packet = 0u; }
	if (read_error != 0u) { flags |= LOG_STREAM_FLAG_ERROR | LOG_STREAM_FLAG_LAST; s_session.active = 0u; }
	else if (s_session.next_logical >= tier_end) {
		if (s_session.dump_tier_mode == LOG_TIER_BOTH && s_session.cur_tier == LOG_TIER_CRITICAL && s_session.end_logical_gen > 0u) flags |= LOG_STREAM_FLAG_TIER_CHANGE;
		else { flags |= LOG_STREAM_FLAG_LAST; s_session.active = 0u; }
	}

	put_u16_le(&payload[0], s_session.stream_id); put_u16_le(&payload[2], s_session.pkt_num); payload[4] = flags; payload[5] = s_session.cur_tier; payload[6] = rec_count; payload[7] = 0u;
	put_u32_le(&payload[8], first_logical); put_u32_le(&payload[12], LogStream_CurrentWriteHead());
	LogStream_SendData(s_session.port, 0u, payload, payload_len);
	s_session.pkt_num++; s_session.pkt_sent++;
}
