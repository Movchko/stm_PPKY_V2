#include "config_sync.hpp"

#include "config_monitor.h"
#include "config_zone_block.h"

#include <string.h>

extern "C" {
#include "backend.h"
#include "service.h"
#include "event_log.h"
}

typedef enum {
	CFGSYNC_OP_NONE = 0,
	CFGSYNC_OP_READ_ALL,
	CFGSYNC_OP_VERIFY_CRC,
	CFGSYNC_OP_APPLY_ALL,
	CFGSYNC_OP_PERIODIC_VERIFY,
	CFGSYNC_OP_READ_MCU_UID,
	CFGSYNC_OP_SYNC_IGN_BLOCK
} CfgSyncOp;

typedef enum {
	IGB_SUB_READ = 0,
	IGB_SUB_APPLY = 1,
	IGB_SUB_READBACK = 2
} IgnBlockSubphase;

typedef enum {
	CFGSYNC_STEP_IDLE = 0,
	CFGSYNC_STEP_WAIT_CFG_SIZE,
	CFGSYNC_STEP_WAIT_CFG_WORD,
	CFGSYNC_STEP_WAIT_CFG_CRC,
	CFGSYNC_STEP_WAIT_SET_CFG_WORD,
	CFGSYNC_STEP_WAIT_SAVE,
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
	uint8_t ign_block_subphase;
	uint8_t ign_block_any_apply;
} CfgSyncCtx;

static CfgSyncCtx g_cfg_sync = {};

static PPKYCfg *g_cfg = nullptr;
static ActiveDeviceInfo *g_active_devices = nullptr;
static uint8_t *g_active_devices_count = nullptr;
static void (*g_save_config_cb)(void) = nullptr;
static void (*g_apply_success_cb)(void) = nullptr;
static uint8_t *g_crc_mismatch_flag = nullptr;

#define CFGSYNC_REQ_TIMEOUT_MS    150u
#define CFGSYNC_REQ_MAX_RETRIES   5u
#define CFGSYNC_SAVE_TIMEOUT_MS   2500u
#define CFGSYNC_SAVE_MAX_RETRIES  3u

static void ResolveRuntimeDevForSlot(uint8_t slot, Device *out_dev);
static void CfgSync_SendReq(const Device *dev, uint8_t cmd, const uint8_t *params, uint32_t now_ms);

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

static uint8_t CfgSync_SlotHasIgniterInCfg(uint8_t slot) {
	if (g_cfg == nullptr || slot >= 32u) {
		return 0u;
	}
	const MKUCfg *mcu = &g_cfg->CfgDevices[slot];
	for (uint8_t vi = 0u; vi < NUM_DEV_IN_MCU; vi++) {
		if ((uint8_t)(mcu->VDtype[vi] & 0xFFu) == DEVICE_IGNITER_TYPE) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t CfgSync_DevMatchesActive(const Device *a, const Device *b) {
	if (a == nullptr || b == nullptr) {
		return 0u;
	}
	return (a->d_type == b->d_type &&
	        a->h_adr == b->h_adr &&
	        (a->l_adr & 0x3Fu) == (b->l_adr & 0x3Fu) &&
	        (a->zone & 0x7Fu) == (b->zone & 0x7Fu)) ? 1u : 0u;
}

static uint8_t CfgSync_ActiveDevHasIgniterVdev(const Device *dev) {
	if (dev == nullptr || g_active_devices == nullptr || g_active_devices_count == nullptr) {
		return 0u;
	}
	for (uint8_t i = 0u; i < *g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (ad->online == 0u) {
			continue;
		}
		if (!CfgSync_DevMatchesActive(&ad->dev, dev)) {
			continue;
		}
		for (uint8_t vi = 0u; vi < ad->vdev_count; vi++) {
			if (ad->vdevs[vi].online != 0u &&
			    ad->vdevs[vi].v_d_type == DEVICE_IGNITER_TYPE) {
				return 1u;
			}
		}
		return 0u;
	}
	return 0u;
}

static uint8_t CfgSync_SlotEligibleForIgnBlock(uint8_t slot) {
	Device dev;
	if (!IsValidCfgSlot(slot)) {
		return 0u;
	}
	ResolveRuntimeDevForSlot(slot, &dev);
	if ((dev.zone & 0x7Fu) == 0u) {
		return 0u;
	}
	if (!CfgSync_ActiveDevHasIgniterVdev(&dev)) {
		return 0u;
	}
	if (!CfgSync_SlotHasIgniterInCfg(slot)) {
		return 0u;
	}
	return 1u;
}

static uint8_t CfgSync_PatchIgniterBlocks(uint8_t slot, uint8_t zone_can) {
	if (g_cfg == nullptr || slot >= 32u || zone_can == 0u) {
		return 0u;
	}
	const uint8_t expected = PPKY_ZoneLaunchBlockedByCanZone(zone_can);
	MKUCfg *mcu = &g_cfg->CfgDevices[slot];
	uint8_t dirty = 0u;
	for (uint8_t vi = 0u; vi < NUM_DEV_IN_MCU; vi++) {
		if ((uint8_t)(mcu->VDtype[vi] & 0xFFu) != DEVICE_IGNITER_TYPE) {
			continue;
		}
		DeviceIgniterConfig *ic = reinterpret_cast<DeviceIgniterConfig *>(mcu->Devices[vi].reserv);
		if (ic->block != expected) {
			ic->block = expected;
			dirty = 1u;
		}
	}
	return dirty;
}

static void CfgSync_BeginReadWords(uint32_t now_ms) {
	g_cfg_sync.word_index = 0u;
	uint8_t p[7] = {0u};
	p[0] = (uint8_t)((g_cfg_sync.word_index >> 8) & 0xFFu);
	p[1] = (uint8_t)(g_cfg_sync.word_index & 0xFFu);
	CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigWord, p, now_ms);
	g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_WORD;
}

static void CfgSync_BeginApplyWords(uint32_t now_ms) {
	const uint8_t *cfg_bytes = (const uint8_t *)&g_cfg->CfgDevices[g_cfg_sync.current_slot];
	g_cfg_sync.word_index = 0u;
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

/* Для APPLY/VERIFY адресовать команды нужно на фактический "живой" адрес МКУ в шине,
 * а не на адрес из образа (в образе зона/адрес уже могут быть изменены).
 * Иначе первая же команда уйдёт в "новую" зону и устройство не ответит. */
static void ResolveRuntimeDevForSlot(uint8_t slot, Device *out_dev) {
	if (out_dev == nullptr || g_cfg == nullptr) {
		return;
	}

	*out_dev = g_cfg->CfgDevices[slot].UId.devId;
	if (g_active_devices == nullptr || g_active_devices_count == nullptr) {
		return;
	}

	const Device *desired = &g_cfg->CfgDevices[slot].UId.devId;

	/* 1) Основное правило: h_adr у МКУ постоянный, зона может меняться.
	 * Поэтому сначала ищем по d_type + h_adr (без привязки к zone/l_adr). */
	for (uint8_t i = 0u; i < *g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online) {
			continue;
		}
		if (!IsMcuType(ad->dev.d_type)) {
			continue;
		}
		if (ad->dev.d_type == desired->d_type &&
		    ad->dev.h_adr == desired->h_adr) {
			*out_dev = ad->dev;
			return;
		}
	}

	/* 2) Fallback: только по h_adr среди МКУ (если d_type в образе не совпал). */
	for (uint8_t i = 0u; i < *g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (!ad->online) {
			continue;
		}
		if (!IsMcuType(ad->dev.d_type)) {
			continue;
		}
		if (ad->dev.h_adr == desired->h_adr) {
			*out_dev = ad->dev;
			return;
		}
	}
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
	can_id.field.dir = 0u;
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
	can_id.field.dir = 0u;
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

static void CfgSync_MarkCurrentFailed(uint8_t reason) {
	g_cfg_sync.failed_count++;
	if (g_cfg_sync.op == CFGSYNC_OP_PERIODIC_VERIFY) {
		ConfigMonitor_OnPeriodicCrcResult(g_cfg_sync.current_slot, 0u, 1u);
	} else if (g_cfg_sync.op == CFGSYNC_OP_READ_MCU_UID) {
		ConfigMonitor_OnRemoteUidRead(&g_cfg_sync.current_dev, nullptr, 0u);
	} else if (g_crc_mismatch_flag != nullptr) {
		*g_crc_mismatch_flag = 1u;
	}
	if (g_cfg_sync.op == CFGSYNC_OP_APPLY_ALL ||
	    (g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK && g_cfg_sync.ign_block_subphase == IGB_SUB_APPLY)) {
		EventLog_LogConfigApplyFail(g_cfg_sync.current_dev.d_type,
		                            g_cfg_sync.current_dev.h_adr,
		                            g_cfg_sync.current_dev.l_adr,
		                            g_cfg_sync.current_dev.zone,
		                            g_cfg_sync.current_slot,
		                            reason);
	}
}

static void CfgSync_Finish(uint8_t success, uint8_t save_ppky_cfg) {
	CfgSyncOp finished_op = g_cfg_sync.op;
	uint8_t failed_count = g_cfg_sync.failed_count;
	uint8_t target_count = g_cfg_sync.target_count;
	uint8_t ign_block_any_apply = g_cfg_sync.ign_block_any_apply;
	g_cfg_sync.busy = 0u;
	g_cfg_sync.waiting_reply = 0u;
	g_cfg_sync.success = success ? 1u : 0u;
	if (success && g_crc_mismatch_flag != nullptr &&
	    finished_op != CFGSYNC_OP_PERIODIC_VERIFY && finished_op != CFGSYNC_OP_READ_MCU_UID) {
		*g_crc_mismatch_flag = 0u;
	}
	if (save_ppky_cfg && g_save_config_cb != nullptr) {
		g_save_config_cb();
	}
	if (finished_op == CFGSYNC_OP_APPLY_ALL) {
		if (success) {
			uint8_t ok_count = (target_count >= failed_count) ?
			                   (uint8_t)(target_count - failed_count) : 0u;
			EventLog_LogConfigApplyOk(ok_count, target_count);
			EventLog_LogAllCfgMcusSaved();
			/* Подтверждение в ПО: cmd=168, p0=ok_count, p1=target_count. */
			{
				uint8_t data[7] = {0};
				data[0] = ok_count;
				data[1] = target_count;
				SendMessage(0, ServiceCmd_ApplyConfigDone, data, SEND_NOW,
				            (uint8_t)(BUS_CAN12 | BUS_UART1));
			}
			if (g_apply_success_cb != nullptr) {
				g_apply_success_cb();
			}
		}
	}
	if (finished_op == CFGSYNC_OP_READ_ALL && success) {
		EventLog_LogAllCfgMcusSaved();
	}
	if (finished_op == CFGSYNC_OP_SYNC_IGN_BLOCK && success && ign_block_any_apply != 0u) {
		if (g_apply_success_cb != nullptr) {
			g_apply_success_cb();
		}
	}
}

static uint8_t CfgSync_StartTargetByPos(uint32_t now_ms) {
	if (g_cfg_sync.op == CFGSYNC_OP_READ_MCU_UID) {
		if (g_cfg_sync.target_pos >= g_cfg_sync.target_count) {
			return 0u;
		}
		g_cfg_sync.target_pos++;
		memset(g_cfg_sync.rx_buf, 0, sizeof(g_cfg_sync.rx_buf));
		g_cfg_sync.total_words = (uint16_t)(sizeof(UniqId) / 4u);
		g_cfg_sync.word_index = 0u;
		uint8_t p[7] = {0u};
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigWord, p, now_ms);
		g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_WORD;
		return 1u;
	}

	while (g_cfg_sync.target_pos < g_cfg_sync.target_count) {
		uint8_t slot = g_cfg_sync.target_slots[g_cfg_sync.target_pos];
		g_cfg_sync.current_slot = slot;
		if (g_cfg_sync.op == CFGSYNC_OP_APPLY_ALL ||
		    g_cfg_sync.op == CFGSYNC_OP_VERIFY_CRC ||
		    g_cfg_sync.op == CFGSYNC_OP_PERIODIC_VERIFY ||
		    g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK) {
			ResolveRuntimeDevForSlot(slot, &g_cfg_sync.current_dev);
		} else if (g_cfg_sync.op == CFGSYNC_OP_READ_MCU_UID) {
			/* current_dev уже задан вызывающим кодом */
		} else {
			g_cfg_sync.current_dev = g_cfg->CfgDevices[slot].UId.devId;
		}

		if (!IsMcuType(g_cfg_sync.current_dev.d_type)) {
			g_cfg_sync.target_pos++;
			continue;
		}
		if (g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK) {
			if ((g_cfg_sync.current_dev.zone & 0x7Fu) == 0u) {
				g_cfg_sync.target_pos++;
				continue;
			}
			g_cfg_sync.ign_block_subphase = IGB_SUB_READ;
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

static void CfgSync_StartCommon(CfgSyncOp op, uint8_t rebuild_from_active, int8_t single_slot)
{
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

	if (single_slot >= 0) {
		if (IsValidCfgSlot((uint8_t)single_slot)) {
			g_cfg_sync.target_slots[g_cfg_sync.target_count++] = (uint8_t)single_slot;
		}
	} else {
		for (uint8_t i = 0u; i < 32u; i++) {
			if (IsValidCfgSlot(i)) {
				g_cfg_sync.target_slots[g_cfg_sync.target_count++] = i;
			}
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
		CfgSync_MarkCurrentFailed(1u); /* bad_size */
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}
	g_cfg_sync.total_words = (uint16_t)(g_cfg_sync.remote_cfg_size / 4u);
	g_cfg_sync.word_index = 0u;

	if (g_cfg_sync.op == CFGSYNC_OP_READ_ALL || g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK) {
		CfgSync_BeginReadWords(now_ms);
		return;
	}

	if (g_cfg_sync.op == CFGSYNC_OP_VERIFY_CRC || g_cfg_sync.op == CFGSYNC_OP_PERIODIC_VERIFY) {
		g_cfg_sync.expected_crc = crc32(POLYNOM, &g_cfg->CfgDevices[g_cfg_sync.current_slot], sizeof(MKUCfg));
		uint8_t p[7] = {0u}; /* saved config crc */
		CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigCRC, p, now_ms);
		g_cfg_sync.step = CFGSYNC_STEP_WAIT_CFG_CRC;
		return;
	}

	/* CFGSYNC_OP_APPLY_ALL */
	if (g_cfg_sync.remote_cfg_size != sizeof(MKUCfg)) {
		CfgSync_MarkCurrentFailed(1u); /* bad_size */
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}

	CfgSync_BeginApplyWords(now_ms);
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

	if (g_cfg_sync.op == CFGSYNC_OP_READ_MCU_UID) {
		ConfigMonitor_OnRemoteUidRead(&g_cfg_sync.current_dev, g_cfg_sync.rx_buf,
					      (g_cfg_sync.failed_count == 0u) ? 1u : 0u);
		CfgSync_NextTargetOrFinish(now_ms);
		return;
	}

	if (g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK) {
		memcpy(&g_cfg->CfgDevices[g_cfg_sync.current_slot], g_cfg_sync.rx_buf, sizeof(MKUCfg));
		if (g_cfg_sync.ign_block_subphase == IGB_SUB_READBACK) {
			CfgSync_NextTargetOrFinish(now_ms);
			return;
		}
		const uint8_t dirty = CfgSync_PatchIgniterBlocks(g_cfg_sync.current_slot,
		                                                 g_cfg_sync.current_dev.zone & 0x7Fu);
		if (dirty == 0u) {
			CfgSync_NextTargetOrFinish(now_ms);
			return;
		}
		g_cfg_sync.ign_block_subphase = IGB_SUB_APPLY;
		g_cfg_sync.ign_block_any_apply = 1u;
		CfgSync_BeginApplyWords(now_ms);
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
		if (g_cfg_sync.op == CFGSYNC_OP_PERIODIC_VERIFY) {
			ConfigMonitor_OnPeriodicCrcResult(g_cfg_sync.current_slot, 0u, 0u);
		} else if (!(g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK &&
		             g_cfg_sync.step == CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC)) {
			CfgSync_MarkCurrentFailed(3u); /* crc_mismatch */
		}
	} else if (g_cfg_sync.op == CFGSYNC_OP_PERIODIC_VERIFY) {
		ConfigMonitor_OnPeriodicCrcResult(g_cfg_sync.current_slot, 1u, 0u);
	}

	if (g_cfg_sync.step == CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC &&
	    g_cfg_sync.op == CFGSYNC_OP_SYNC_IGN_BLOCK) {
		if (g_cfg_sync.remote_crc == g_cfg_sync.expected_crc) {
			g_cfg_sync.ign_block_subphase = IGB_SUB_READBACK;
			CfgSync_BeginReadWords(now_ms);
		} else {
			CfgSync_MarkCurrentFailed(3u);
			CfgSync_NextTargetOrFinish(now_ms);
		}
		return;
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
		CfgSync_MarkCurrentFailed(2u); /* echo_mismatch */
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

	/* Все слова записали: SaveConfig на МКУ, дождаться ACK, затем CRC. */
	uint8_t p_save[7] = {0u};
	CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_SaveConfig, p_save, now_ms);
	g_cfg_sync.deadline_ms = now_ms + CFGSYNC_SAVE_TIMEOUT_MS;
	g_cfg_sync.retries = 0u;
	g_cfg_sync.step = CFGSYNC_STEP_WAIT_SAVE;
}

static void CfgSync_HandleSaveReply(uint32_t now_ms)
{
	g_cfg_sync.waiting_reply = 0u;
	/* После Save МКУ применяет zone из образа (AplyConfig после ACK).
	 * Дальнейший CRC/команды шлём уже на целевой адрес из конфиг-образа. */
	g_cfg_sync.current_dev = g_cfg->CfgDevices[g_cfg_sync.current_slot].UId.devId;
	g_cfg_sync.expected_crc = crc32(POLYNOM, &g_cfg->CfgDevices[g_cfg_sync.current_slot], sizeof(MKUCfg));
	uint8_t p_crc[7] = {0u};
	CfgSync_SendReq(&g_cfg_sync.current_dev, ServiceCmd_GetConfigCRC, p_crc, now_ms);
	g_cfg_sync.step = CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC;
}

extern "C" void ConfigSync_Init(PPKYCfg *cfg,
                                ActiveDeviceInfo *active_devices,
                                uint8_t *active_devices_count,
                                void (*save_config_cb)(void),
                                void (*apply_success_cb)(void),
                                uint8_t *mismatch_flag_ptr) {
	g_cfg = cfg;
	g_active_devices = active_devices;
	g_active_devices_count = active_devices_count;
	g_save_config_cb = save_config_cb;
	g_apply_success_cb = apply_success_cb;
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

	const uint8_t max_retries = (g_cfg_sync.step == CFGSYNC_STEP_WAIT_SAVE) ?
	                            CFGSYNC_SAVE_MAX_RETRIES : CFGSYNC_REQ_MAX_RETRIES;
	if (g_cfg_sync.retries < max_retries) {
		g_cfg_sync.retries++;
		CfgSync_ResendReq(now_ms);
		if (g_cfg_sync.step == CFGSYNC_STEP_WAIT_SAVE) {
			g_cfg_sync.deadline_ms = now_ms + CFGSYNC_SAVE_TIMEOUT_MS;
		}
		return;
	}

	CfgSync_MarkCurrentFailed(0u); /* timeout */
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
	case CFGSYNC_STEP_WAIT_SAVE:
		CfgSync_HandleSaveReply(now_ms);
		break;
	case CFGSYNC_STEP_WAIT_SAVE_AND_READBACK_CRC:
		CfgSync_HandleCfgCrcReply(msg_data, now_ms);
		break;
	default:
		break;
	}
}

extern "C" void ConfigSync_StartReadAllAndSave(void) {
	CfgSync_StartCommon(CFGSYNC_OP_READ_ALL, 1u, -1);
}

extern "C" void ConfigSync_StartVerify(void) {
	CfgSync_StartCommon(CFGSYNC_OP_VERIFY_CRC, 0u, -1);
}

extern "C" void ConfigSync_StartApply(void) {
	CfgSync_StartCommon(CFGSYNC_OP_APPLY_ALL, 0u, -1);
}

extern "C" void ConfigSync_StartPeriodicVerifySlot(uint8_t slot) {
	CfgSync_StartCommon(CFGSYNC_OP_PERIODIC_VERIFY, 0u, (int8_t)slot);
}

extern "C" void ConfigSync_StartReadMcuUid(const Device *dev) {
	if (g_cfg_sync.busy || g_cfg == nullptr || dev == nullptr) {
		return;
	}

	memset(&g_cfg_sync, 0, sizeof(g_cfg_sync));
	g_cfg_sync.busy = 1u;
	g_cfg_sync.op = CFGSYNC_OP_READ_MCU_UID;
	g_cfg_sync.current_dev = *dev;
	g_cfg_sync.target_count = 1u;
	g_cfg_sync.total_words = (uint16_t)(sizeof(UniqId) / 4u);
}

extern "C" void ConfigSync_StartIgnBlockSync(void) {
	if (g_cfg_sync.busy || g_cfg == nullptr) {
		return;
	}

	memset(&g_cfg_sync, 0, sizeof(g_cfg_sync));
	g_cfg_sync.busy = 1u;
	g_cfg_sync.op = CFGSYNC_OP_SYNC_IGN_BLOCK;
	g_cfg_sync.step = CFGSYNC_STEP_IDLE;

	for (uint8_t i = 0u; i < 32u; i++) {
		if (!CfgSync_SlotEligibleForIgnBlock(i)) {
			continue;
		}
		if (g_cfg_sync.target_count < 32u) {
			g_cfg_sync.target_slots[g_cfg_sync.target_count++] = i;
		}
	}

	if (g_cfg_sync.target_count == 0u) {
		CfgSync_Finish(1u, 0u);
	}
}

extern "C" uint8_t ConfigSync_IsBusy(void) {
	return g_cfg_sync.busy ? 1u : 0u;
}

