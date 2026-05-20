#include "config_sync.hpp"

#include <string.h>

extern "C" {
#include "backend.h"
#include "service.h"
}

typedef enum {
	CFGSYNC_OP_NONE = 0,
	CFGSYNC_OP_READ_ALL,
	CFGSYNC_OP_VERIFY_CRC,
	CFGSYNC_OP_APPLY_ALL
} CfgSyncOp;

typedef enum {
	CFGSYNC_STEP_IDLE = 0,
	CFGSYNC_STEP_WAIT_CFG_SIZE,
	CFGSYNC_STEP_WAIT_CFG_WORD,
	CFGSYNC_STEP_WAIT_CFG_CRC,
	CFGSYNC_STEP_WAIT_SET_CFG_WORD,
	CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC
} CfgSyncStep;

typedef struct {
	CfgSyncOp op;
	CfgSyncStep step;
	uint8_t busy;
	uint8_t success;
	uint8_t failed_count;

	uint8_t target_slots[32];
	uint8_t target_count;
	uint8_t target_pos;

	Device current_dev;
	uint8_t current_slot;

	uint32_t remote_cfg_size;
	uint16_t total_words;
	uint16_t word_index;
	uint8_t rx_buf[sizeof(MKUCfg)];

	uint32_t expected_crc;
	uint32_t remote_crc;

	uint8_t waiting_reply;
	uint8_t req_cmd;
	uint8_t req_params[7];
	uint8_t retries;
	uint32_t deadline_ms;
} CfgSyncCtx;

static CfgSyncCtx g_cfg_sync = {};

static PPKYCfg *g_cfg = nullptr;
static ActiveDeviceInfo *g_active_devices = nullptr;
static uint8_t *g_active_devices_count = nullptr;
static void (*g_save_config_cb)(void) = nullptr;
static uint8_t *g_crc_mismatch_flag = nullptr;

#define CFGSYNC_REQ_TIMEOUT_MS   150u
#define CFGSYNC_REQ_MAX_RETRIES  5u

static uint8_t IsMcuType(uint8_t d_type) {
	return (d_type == DEVICE_MCU_IGN_TYPE ||
	        d_type == DEVICE_MCU_TC_TYPE ||
	        d_type == DEVICE_MCU_K1 ||
	        d_type == DEVICE_MCU_K2 ||
	        d_type == DEVICE_MCU_K3 ||
	        d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

static uint8_t IsValidCfgSlot(uint8_t slot) {
	if (g_cfg == nullptr || slot >= 32u) {
		return 0u;
	}
	return IsMcuType(g_cfg->CfgDevices[slot].UId.devId.d_type);
}

static void BuildCfgListFromActiveDevices(void) {
	if (g_cfg == nullptr || g_active_devices == nullptr || g_active_devices_count == nullptr) {
		return;
	}

	memset(g_cfg->CfgDevices, 0, sizeof(g_cfg->CfgDevices));

	uint8_t out_i = 0u;
	for (uint8_t i = 0u; i < *g_active_devices_count && out_i < 32u; i++) {
		if (g_active_devices[i].online == 0u) {
			continue;
		}
		if (!IsMcuType(g_active_devices[i].dev.d_type)) {
			continue;
		}
		g_cfg->CfgDevices[out_i].UId.devId = g_active_devices[i].dev;
		out_i++;
	}
}

static void CfgSync_SendReq(const Device *dev, uint8_t cmd, const uint8_t *params, uint32_t now_ms) {
	can_ext_id_t can_id;
	uint8_t data[8] = {0u};
	can_id.ID = 0u;
	can_id.field.dir = 1u;
	can_id.field.d_type = dev->d_type & 0x7Fu;
	can_id.field.h_adr = dev->h_adr;
	can_id.field.l_adr = dev->l_adr & 0x3Fu;
	can_id.field.zone = dev->zone & 0x7Fu;

	data[0] = cmd;
	for (uint8_t i = 0u; i < 7u; i++) {
		data[i + 1u] = params ? params[i] : 0u;
		g_cfg_sync.req_params[i] = params ? params[i] : 0u;
	}
	g_cfg_sync.req_cmd = cmd;
	g_cfg_sync.waiting_reply = 1u;
	g_cfg_sync.retries = 0u;
	g_cfg_sync.deadline_ms = now_ms + CFGSYNC_REQ_TIMEOUT_MS;
	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
}

static void CfgSync_ResendReq(uint32_t now_ms) {
	can_ext_id_t can_id;
	uint8_t data[8] = {0u};
	can_id.ID = 0u;
	can_id.field.dir = 1u;
	can_id.field.d_type = g_cfg_sync.current_dev.d_type & 0x7Fu;
	can_id.field.h_adr = g_cfg_sync.current_dev.h_adr;
	can_id.field.l_adr = g_cfg_sync.current_dev.l_adr & 0x3Fu;
	can_id.field.zone = g_cfg_sync.current_dev.zone & 0x7Fu;

	data[0] = g_cfg_sync.req_cmd;
	for (uint8_t i = 0u; i < 7u; i++) {
		data[i + 1u] = g_cfg_sync.req_params[i];
	}

	g_cfg_sync.deadline_ms = now_ms + CFGSYNC_REQ_TIMEOUT_MS;
	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
}

static void CfgSync_MarkCurrentFailed(void) {
	g_cfg_sync.failed_count++;
	if (g_crc_mismatch_flag != nullptr) {
		*g_crc_mismatch_flag = 1u;
	}
}

static void CfgSync_Finish(uint8_t success, uint8_t save_ppky_cfg) {
	g_cfg_sync.busy = 0u;
	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.success = success ? 1u : 0u;
	if (success && g_crc_mismatch_flag != nullptr) {
		*g_crc_mismatch_flag = 0u;
	}
	if (save_ppky_cfg && g_save_config_cb != nullptr) {
		g_save_config_cb();
	}
}

static uint8_t CfgSync_StartTargetByPos(uint32_t now_ms) {
	while (g_cfg_sync.target_pos < g_cfg_sync.target_count) {
		uint8_t slot = g_cfg_sync.target_slots[g_cfg_sync.target_pos];
		g_cfg_sync.current_slot = slot;
		g_cfg_sync.current_dev = g_cfg->CfgDevices[slot].UId.devId;

		if (!IsMcuType(g_cfg_sync.current_dev.d_type)) {
			g_cfg_sync.target_pos++;
			continue;
		}

		memset(g_cfg_sync.rx_buf, 0, sizeof(g_cfg_sync.rx_buf));
		g_cfg_sync.remote_cfg_size = 0u;
		g_cfg_sync.total_words = 0u;
		g_cfg_sync.word_index = 0u;
		g_cfg_sync.expected_crc = 0u;
		g_cfg_sync.remote_crc = 0u;

		uint8_t p[7] = {0u};
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigSize, p, now_ms);
		g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_SIZE;
		return 1u;
	}
	return 0u;
}

static void CfgSync_StartCommon(CfgSyncOp op, uint8_t rebuild_from_active) {
	if (g_cfg_sync.busy || g_cfg == nullptr) {
		return;
	}

	if (rebuild_from_active) {
		BuildCfgListFromActiveDevices();
	}

	memset(&g_cfg_sync, 0, sizeof(g_cfg_sync));
	g_cfg_sync.busy = 1u;
	g_cfg_sync.op = op;
	g_cfg_sync.step = CFGSYNC_STEP_IDLE;

	for (uint8_t i = 0u; i < 32u; i++) {
		if (IsValidCfgSlot(i)) {
			g_cfg_sync.target_slots[g_cfg_sync.target_count++] = i;
		}
	}
	if (g_cfg_sync.target_count == 0u) {
		CfgSync_Finish(1u, (op == CFGSYNC_OP_READ_ALL) ? 1u : 0u);
	}
}

static void CfgSync_NextTargetOrFinish(uint32_t now_ms) {
	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.target_pos++;
	if (!CfgSync_StartTargetByPos(now_ms)) {
		uint8_t ok = (g_cfg_sync.failed_count == 0u) ? 1u : 0u;
		CfgSync_Finish(ok, (g_cfg_sync.op == CFGSYNC_OP_READ_ALL) ? 1u : 0u);
	}
}

static void CfgSync_HandleCfgSizeReply(const uint8_t *MsgData, uint32_t now_ms) {
	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.remote_cfg_size = ((uint32_t)MsgData[1] << 24) |
	                             ((uint32_t)MsgData[2] << 16) |
	                             ((uint32_t)MsgData[3] << 8)  |
	                             ((uint32_t)MsgData[4] << 0);

	if (g_cfg_sync.remote_cfg_size == 0u || g_cfg_sync.remote_cfg_size > sizeof(MKUCfg) ||
	    (g_cfg_sync.remote_cfg_size & 0x3u) != 0u) {
		CfgSync_MarkCurrentFailed();
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}
	g_cfg_sync.total_words = (uint16_t)(g_cfg_sync.remote_cfg_size / 4u);
	g_cfg_sync.word_index = 0u;

	if (g_cfg_sync.op == CFGSYNC_OP_READ_ALL) {
		uint8_t p[7] = {0u};
		p[0] = (uint8_t)((g_cfg_sync.word_index >> 8) & 0xFFu);
		p[1] = (uint8_t)(g_cfg_sync.word_index & 0xFFu);
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigWord, p, now_ms);
		g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_WORD;
		return;
	}

	if (g_cfg_sync.op == CFGSYNC_OP_VERIFY_CRC) {
		g_cfg_sync.expected_crc = crc32(POLYNOM, &g_cfg->CfgDevices[g_cfg_sync.current_slot], sizeof(MKUCfg));
		uint8_t p[7] = {0u}; /* saved config crc */
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigCRC, p, now_ms);
		g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_CRC;
		return;
	}

	/* CFGSYNC_OP_APPLY_ALL */
	if (g_cfg_sync.remote_cfg_size != sizeof(MKUCfg)) {
		CfgSync_MarkCurrentFailed();
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}

	const uint8_t *cfg_bytes = (const uint8_t *)&g_cfg->CfgDevices[g_cfg_sync.current_slot];
	uint32_t word = ((uint32_t)cfg_bytes[0] << 24) |
	               ((uint32_t)cfg_bytes[1] << 16) |
	               ((uint32_t)cfg_bytes[2] << 8)  |
	               ((uint32_t)cfg_bytes[3] << 0);
	uint8_t p[7] = {0u};
	p[0] = 0u;
	p[1] = 0u;
	p[2] = (uint8_t)((word >> 24) & 0xFFu);
	p[3] = (uint8_t)((word >> 16) & 0xFFu);
	p[4] = (uint8_t)((word >> 8) & 0xFFu);
	p[5] = (uint8_t)(word & 0xFFu);
	CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_SetConfigWord, p, now_ms);
	g_cfg_sync.step = CFGSYNC_STEP_WAIT_SET_CFG_WORD;
}

static void CfgSync_HandleCfgWordReply(const uint8_t *MsgData, uint32_t now_ms) {
	uint16_t idx = ((uint16_t)MsgData[1] << 8) | MsgData[2];
	if (idx != g_cfg_sync.word_index) {
		return;
	}
	g_cfg_sync.waiting_reply = 0u;

	uint32_t word = ((uint32_t)MsgData[3] << 24) |
	                ((uint32_t)MsgData[4] << 16) |
	                ((uint32_t)MsgData[5] << 8)  |
	                ((uint32_t)MsgData[6] << 0);
	uint32_t byte_index = (uint32_t)idx * 4u;
	if ((byte_index + 4u) <= sizeof(g_cfg_sync.rx_buf)) {
		g_cfg_sync.rx_buf[byte_index + 0] = (uint8_t)((word >> 24) & 0xFFu);
		g_cfg_sync.rx_buf[byte_index + 1] = (uint8_t)((word >> 16) & 0xFFu);
		g_cfg_sync.rx_buf[byte_index + 2] = (uint8_t)((word >> 8) & 0xFFu);
		g_cfg_sync.rx_buf[byte_index + 3] = (uint8_t)(word & 0xFFu);
	}

	g_cfg_sync.word_index++;
	if (g_cfg_sync.word_index < g_cfg_sync.total_words) {
		uint8_t p[7] = {0u};
		p[0] = (uint8_t)((g_cfg_sync.word_index >> 8) & 0xFFu);
		p[1] = (uint8_t)(g_cfg_sync.word_index & 0xFFu);
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigWord, p, now_ms);
		return;
	}

	memcpy(&g_cfg->CfgDevices[g_cfg_sync.current_slot], g_cfg_sync.rx_buf, sizeof(MKUCfg));
	CfgSync_NextTargetOrFinish(now_ms);
}

static void CfgSync_HandleCfgCrcReply(const uint8_t *MsgData, uint32_t now_ms) {
	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.remote_crc = ((uint32_t)MsgData[1] << 24) |
	                        ((uint32_t)MsgData[2] << 16) |
	                        ((uint32_t)MsgData[3] << 8)  |
	                        ((uint32_t)MsgData[4] << 0);

	if (g_cfg_sync.remote_crc != g_cfg_sync.expected_crc) {
		CfgSync_MarkCurrentFailed();
	}
	CfgSync_NextTargetOrFinish(now_ms);
}

static void CfgSync_HandleSetCfgWordReply(const uint8_t *MsgData, uint32_t now_ms) {
	uint16_t idx = ((uint16_t)MsgData[1] << 8) | MsgData[2];
	if (idx != g_cfg_sync.word_index) {
		return;
	}

	const uint8_t *cfg_bytes = (const uint8_t *)&g_cfg->CfgDevices[g_cfg_sync.current_slot];
	uint32_t byte_index = (uint32_t)idx * 4u;
	uint32_t expected_word = ((uint32_t)cfg_bytes[byte_index + 0] << 24) |
	                         ((uint32_t)cfg_bytes[byte_index + 1] << 16) |
	                         ((uint32_t)cfg_bytes[byte_index + 2] << 8)  |
	                         ((uint32_t)cfg_bytes[byte_index + 3] << 0);
	uint32_t remote_word = ((uint32_t)MsgData[3] << 24) |
	                       ((uint32_t)MsgData[4] << 16) |
	                       ((uint32_t)MsgData[5] << 8)  |
	                       ((uint32_t)MsgData[6] << 0);
	if (remote_word != expected_word) {
		CfgSync_MarkCurrentFailed();
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}

	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.word_index++;
	if (g_cfg_sync.word_index < g_cfg_sync.total_words) {
		uint8_t p[7] = {0u};
		uint32_t next_byte_index = (uint32_t)g_cfg_sync.word_index * 4u;
		uint32_t w = ((uint32_t)cfg_bytes[next_byte_index + 0] << 24) |
		             ((uint32_t)cfg_bytes[next_byte_index + 1] << 16) |
		             ((uint32_t)cfg_bytes[next_byte_index + 2] << 8)  |
		             ((uint32_t)cfg_bytes[next_byte_index + 3] << 0);
		p[0] = (uint8_t)((g_cfg_sync.word_index >> 8) & 0xFFu);
		p[1] = (uint8_t)(g_cfg_sync.word_index & 0xFFu);
		p[2] = (uint8_t)((w >> 24) & 0xFFu);
		p[3] = (uint8_t)((w >> 16) & 0xFFu);
		p[4] = (uint8_t)((w >> 8) & 0xFFu);
		p[5] = (uint8_t)(w & 0xFFu);
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_SetConfigWord, p, now_ms);
		return;
	}

	/* Все слова записали: SaveConfig на МКУ, затем читаем CRC назад. */
	can_ext_id_t can_id;
	uint8_t data[8] = {0u};
	can_id.ID = 0u;
	can_id.field.dir = 1u;
	can_id.field.d_type = g_cfg_sync.current_dev.d_type & 0x7Fu;
	can_id.field.h_adr = g_cfg_sync.current_dev.h_adr;
	can_id.field.l_adr = g_cfg_sync.current_dev.l_adr & 0x3Fu;
	can_id.field.zone = g_cfg_sync.current_dev.zone & 0x7Fu;
	data[0] = ServiceCmd_SaveConfig;
	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);

	g_cfg_sync.expected_crc = crc32(POLYNOM, &g_cfg->CfgDevices[g_cfg_sync.current_slot], sizeof(MKUCfg));
	uint8_t p_crc[7] = {0u};
	CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigCRC, p_crc, now_ms);
	g_cfg_sync.step = CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC;
}

extern "C" void ConfigSync_Init(PPKYCfg *cfg,
                                ActiveDeviceInfo *active_devices,
                                uint8_t *active_devices_count,
                                void (*save_config_cb)(void),
                                uint8_t *mismatch_flag_ptr) {
	g_cfg = cfg;
	g_active_devices = active_devices;
	g_active_devices_count = active_devices_count;
	g_save_config_cb = save_config_cb;
	g_crc_mismatch_flag = mismatch_flag_ptr;
	memset(&g_cfg_sync, 0, sizeof(g_cfg_sync));
}

extern "C" void ConfigSync_Process1ms(uint32_t now_ms) {
	if (!g_cfg_sync.busy) {
		return;
	}

	if (g_cfg_sync.step == CFGSYNC_STEP_IDLE) {
		if (!CfgSync_StartTargetByPos(now_ms)) {
			CfgSync_Finish(1u, (g_cfg_sync.op == CFGSYNC_OP_READ_ALL) ? 1u : 0u);
		}
		return;
	}

	if (!g_cfg_sync.waiting_reply) {
		return;
	}

	if ((int32_t)(now_ms - g_cfg_sync.deadline_ms) < 0) {
		return;
	}

	if (g_cfg_sync.retries < CFGSYNC_REQ_MAX_RETRIES) {
		g_cfg_sync.retries++;
		CfgSync_ResendReq(now_ms);
		return;
	}

	CfgSync_MarkCurrentFailed();
	CfgSync_NextTargetOrFinish(now_ms);
}

extern "C" void ConfigSync_OnListenerMessage(uint32_t msg_id, const uint8_t *msg_data) {
	if (!g_cfg_sync.busy || !g_cfg_sync.waiting_reply || msg_data == nullptr) {
		return;
	}

	can_ext_id_t id;
	id.ID = msg_id;
	if (id.field.dir == 0u) {
		return;
	}
	if (id.field.d_type != (g_cfg_sync.current_dev.d_type & 0x7Fu) ||
	    id.field.h_adr != g_cfg_sync.current_dev.h_adr ||
	    (id.field.l_adr & 0x3Fu) != (g_cfg_sync.current_dev.l_adr & 0x3Fu) ||
	    (id.field.zone & 0x7Fu) != (g_cfg_sync.current_dev.zone & 0x7Fu)) {
		return;
	}
	if (msg_data[0] != g_cfg_sync.req_cmd) {
		return;
	}

	uint32_t now_ms = HAL_GetTick();
	switch (g_cfg_sync.step) {
	case CFGSYNC_STEP_WAIT_CFG_SIZE:
		CfgSync_HandleCfgSizeReply(msg_data, now_ms);
		break;
	case CFGSYNC_STEP_WAIT_CFG_WORD:
		CfgSync_HandleCfgWordReply(msg_data, now_ms);
		break;
	case CFGSYNC_STEP_WAIT_CFG_CRC:
		CfgSync_HandleCfgCrcReply(msg_data, now_ms);
		break;
	case CFGSYNC_STEP_WAIT_SET_CFG_WORD:
		CfgSync_HandleSetCfgWordReply(msg_data, now_ms);
		break;
	case CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC:
		CfgSync_HandleCfgCrcReply(msg_data, now_ms);
		break;
	default:
		break;
	}
}

extern "C" void ConfigSync_StartReadAllAndSave(void) {
	CfgSync_StartCommon(CFGSYNC_OP_READ_ALL, 1u);
}

extern "C" void ConfigSync_StartVerify(void) {
	CfgSync_StartCommon(CFGSYNC_OP_VERIFY_CRC, 0u);
}

extern "C" void ConfigSync_StartApply(void) {
	CfgSync_StartCommon(CFGSYNC_OP_APPLY_ALL, 0u);
}

extern "C" uint8_t ConfigSync_IsBusy(void) {
	return g_cfg_sync.busy ? 1u : 0u;
}
