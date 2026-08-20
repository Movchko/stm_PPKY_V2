/*
 * config_monitor.cpp - сверка PPKYConfig с активными устройствами на CAN-кольце.
 *
 * Условия работы: >=10 с после старта (CM_GRACE_MS). Кольцо может быть разорвано.
 * Сопоставление МКУ с шиной: d_type+h_adr (zone/l_adr в образе могут отличаться от CAN).
 *
 * Событие 4 DEVICE_MISSING: слот CfgDevices не online (5 с таймаут RefreshActiveDevices).
 *   Сопоставление МКУ: d_type+h_adr (zone/l_adr на шине могут отличаться от образа в PPKYConfig).
 *   Виртуальные каналы VDtype[] не проверяются.
 * Событие 5 DEVICE_FOUND: online МКУ не в конфиге; debounce >=3 с и >=3 кадра.
 *   Виртуальные каналы (Спичка/ДПТ/кнопка/...) не учитываются - как и в DEVICE_MISSING.
 * Событие 6 CONFIG_MISMATCH: CRC MKUCfg через ConfigSync, round-robin 1 online слот / 5 с.
 *   Не проверяется для missing/new МКУ; откладывается до завершения boot IgnBlockSync.
 *
 * UI: warning.cpp SyncConfigMonitorItems -> ОТСУСТВ. / НОВОЕ / ОШ. КОНФ.
 * Серийник "НОВОЕ": чтение UniqId с МКУ (ConfigSync READ_MCU_UID).
 */
#include "config_monitor.h"

#include "backend.h"
#include "config_ign_block_sync.h"
#include "config_sync.hpp"
#include "event_log.h"
#include "tick_time.h"

#include <string.h>

extern PPKYCfg PPKYConfig;
extern ActiveDeviceInfo g_active_devices[];
extern uint8_t g_active_devices_count;

static constexpr uint32_t CM_GRACE_MS = 10000u;
static constexpr uint32_t CM_FOUND_CONFIRM_MS = 3000u;
static constexpr uint8_t CM_FOUND_CONFIRM_RX = 3u;
static constexpr uint32_t CM_CRC_PERIOD_MS = 5000u;
static constexpr uint8_t CM_CRC_FAIL_LATCH = 2u;

typedef struct {
	uint8_t zone;
	uint8_t h_adr;
	uint8_t l_adr;
	uint8_t d_type;
	uint8_t v_l_adr;
	uint8_t v_d_type;
} CmDevKey;

typedef struct {
	CmDevKey key;
	uint8_t latched;
	uint32_t phase_since_ms;
	uint32_t snap_last_ms;
	uint8_t rx_cnt;
} CmFoundDebounce;

typedef struct {
	Device dev;
	uint32_t uid0;
	uint32_t uid1;
	uint32_t uid2;
	uint8_t valid;
} CmRemoteUid;

static uint32_t s_boot_ms = 0u;
static uint8_t s_missing_latched[32];
static uint8_t s_crc_latched[32];
static uint8_t s_crc_fail_streak[32];
static CmFoundDebounce s_found_db[32];
static uint8_t s_found_latched_count = 0u;
static CmRemoteUid s_remote_uid[16];
static uint32_t s_last_crc_ms = 0u;
static uint8_t s_crc_rr_slot = 0u;
static uint32_t s_now_ms = 0u;

static uint8_t IsMcuType(uint8_t d_type)
{
	return (d_type == DEVICE_MCU_IGN_TYPE ||
	        d_type == DEVICE_MCU_K1 ||
	        d_type == DEVICE_MCU_K2 ||
	        d_type == DEVICE_MCU_K3 ||
	        d_type == DEVICE_MCU_KR ||
	        d_type == DEVICE_MCU_TC_TYPE) ? 1u : 0u;
}

static uint8_t CmCanRun(uint32_t now_ms)
{
	return ((int32_t)(now_ms - s_boot_ms) >= (int32_t)CM_GRACE_MS) ? 1u : 0u;
}

static uint8_t CmKeysEqual(const CmDevKey *a, const CmDevKey *b)
{
	return (a->zone == b->zone &&
	        a->h_adr == b->h_adr &&
	        a->l_adr == b->l_adr &&
	        a->d_type == b->d_type &&
	        a->v_l_adr == b->v_l_adr &&
	        a->v_d_type == b->v_d_type) ? 1u : 0u;
}

static int CmFindActiveExact(const Device *dev, uint8_t require_online)
{
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (require_online && ad->online == 0u) {
			continue;
		}
		if (ad->dev.zone == dev->zone &&
		    ad->dev.h_adr == dev->h_adr &&
		    ad->dev.l_adr == dev->l_adr &&
		    ad->dev.d_type == dev->d_type) {
			return (int)i;
		}
	}
	return -1;
}

static int CmFindCfgSlotExact(const Device *dev)
{
	for (uint8_t slot = 0u; slot < 32u; slot++) {
		const Device *dv = &PPKYConfig.CfgDevices[slot].UId.devId;
		if (!IsMcuType(dv->d_type)) {
			continue;
		}
		if (dv->zone == dev->zone &&
		    dv->h_adr == dev->h_adr &&
		    dv->l_adr == dev->l_adr &&
		    dv->d_type == dev->d_type) {
			return (int)slot;
		}
	}
	return -1;
}

static int CmFindActiveForCfgDev(const Device *cfg_dev, uint8_t require_online)
{
	int idx = CmFindActiveExact(cfg_dev, require_online);
	if (idx >= 0) {
		return idx;
	}
	if (!IsMcuType(cfg_dev->d_type)) {
		return -1;
	}
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *ad = &g_active_devices[i];
		if (require_online && ad->online == 0u) {
			continue;
		}
		if (!IsMcuType(ad->dev.d_type)) {
			continue;
		}
		if (ad->dev.d_type == cfg_dev->d_type &&
		    ad->dev.h_adr == cfg_dev->h_adr) {
			return (int)i;
		}
	}
	return -1;
}

static int CmFindCfgSlotForActive(const Device *dev)
{
	int slot = CmFindCfgSlotExact(dev);
	if (slot >= 0) {
		return slot;
	}
	if (!IsMcuType(dev->d_type)) {
		return -1;
	}
	for (uint8_t s = 0u; s < 32u; s++) {
		const Device *dv = &PPKYConfig.CfgDevices[s].UId.devId;
		if (!IsMcuType(dv->d_type)) {
			continue;
		}
		if (dv->d_type == dev->d_type &&
		    dv->h_adr == dev->h_adr) {
			return (int)s;
		}
	}
	return -1;
}

static uint8_t CmIsMcuMissingSlot(uint8_t slot)
{
	const Device *dv = &PPKYConfig.CfgDevices[slot].UId.devId;
	if (!IsMcuType(dv->d_type)) {
		return 0u;
	}
	return (CmFindActiveForCfgDev(dv, 1u) < 0) ? 1u : 0u;
}

static uint8_t CmIsMcuNew(const Device *dev)
{
	return (CmFindCfgSlotForActive(dev) < 0) ? 1u : 0u;
}

static uint8_t CmIsMcuMissingOrNewSlot(uint8_t slot)
{
	const Device *dv = &PPKYConfig.CfgDevices[slot].UId.devId;
	if (CmFindActiveForCfgDev(dv, 1u) < 0) {
		return 1u;
	}
	return CmIsMcuNew(dv);
}

static uint32_t CmBuildCanHeader(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone)
{
	can_ext_id_t id;
	id.ID = 0u;
	id.field.zone = zone & 0x7Fu;
	id.field.l_adr = l_adr & 0x3Fu;
	id.field.h_adr = h_adr;
	id.field.d_type = d_type & 0x7Fu;
	id.field.dir = 1u;
	return id.ID & 0x1FFFFFFFu;
}

static void CmPostMissingEvent(const Device *dev, uint8_t cfg_slot, uint8_t v_l_adr, uint8_t cleared)
{
	EventLogPayload_t payload;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.can_header = CmBuildCanHeader(dev->d_type, dev->h_adr, dev->l_adr, dev->zone);
	payload.additional[0] = cleared ? 1u : 0u;
	payload.additional[1] = cfg_slot;
	payload.additional[2] = v_l_adr;
	(void)EventLog_Post(EVENT_LOG_DEVICE_MISSING, &payload);
}

static void CmPostFoundEvent(const Device *dev, uint8_t cleared)
{
	EventLogPayload_t payload;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.can_header = CmBuildCanHeader(dev->d_type, dev->h_adr, dev->l_adr, dev->zone);
	payload.additional[0] = cleared ? 1u : 0u;
	(void)EventLog_Post(EVENT_LOG_DEVICE_FOUND, &payload);
}

static void CmPostConfigMismatchEvent(uint8_t slot, const Device *dev, uint8_t cleared)
{
	EventLogPayload_t payload;
	memset(&payload, 0, sizeof(payload));
	payload.master_wagon_num = PPKYConfig.UId.devId.h_adr;
	payload.can_header = CmBuildCanHeader(dev->d_type, dev->h_adr, dev->l_adr, dev->zone);
	payload.additional[0] = cleared ? 1u : 0u;
	payload.additional[1] = slot;
	payload.additional[2] = 0u;
	(void)EventLog_Post(EVENT_LOG_CONFIG_MISMATCH, &payload);
}

static CmFoundDebounce *CmFindFoundDb(const CmDevKey *key)
{
	for (uint8_t i = 0u; i < s_found_latched_count; i++) {
		if (CmKeysEqual(&s_found_db[i].key, key)) {
			return &s_found_db[i];
		}
	}
	return nullptr;
}

static CmFoundDebounce *CmAllocFoundDb(const CmDevKey *key)
{
	CmFoundDebounce *db = CmFindFoundDb(key);
	if (db != nullptr) {
		return db;
	}
	if (s_found_latched_count >= 32u) {
		return nullptr;
	}
	db = &s_found_db[s_found_latched_count++];
	memset(db, 0, sizeof(*db));
	db->key = *key;
	return db;
}

static void CmResetFoundDb(CmFoundDebounce *db)
{
	db->phase_since_ms = 0u;
	db->snap_last_ms = 0u;
	db->rx_cnt = 0u;
}

static void CmStartFoundPhase(CmFoundDebounce *db, uint32_t last_ms, uint32_t now_ms)
{
	db->phase_since_ms = now_ms;
	db->snap_last_ms = last_ms;
	db->rx_cnt = 1u;
}

static void CmCountFoundRx(CmFoundDebounce *db, uint32_t last_ms)
{
	if (last_ms != db->snap_last_ms) {
		db->snap_last_ms = last_ms;
		if (db->rx_cnt < 255u) {
			db->rx_cnt++;
		}
	}
}

static uint8_t CmFoundPhaseDone(const CmFoundDebounce *db, uint32_t now_ms)
{
	if (db->phase_since_ms == 0u) {
		return 0u;
	}
	if (!TickAgeExpiredMs(now_ms, db->phase_since_ms, CM_FOUND_CONFIRM_MS)) {
		return 0u;
	}
	return (db->rx_cnt >= CM_FOUND_CONFIRM_RX) ? 1u : 0u;
}

static CmRemoteUid *CmFindRemoteUid(const Device *dev)
{
	for (uint8_t i = 0u; i < 16u; i++) {
		if (!s_remote_uid[i].valid) {
			continue;
		}
		if (s_remote_uid[i].dev.zone == dev->zone &&
		    s_remote_uid[i].dev.h_adr == dev->h_adr &&
		    s_remote_uid[i].dev.l_adr == dev->l_adr &&
		    s_remote_uid[i].dev.d_type == dev->d_type) {
			return &s_remote_uid[i];
		}
	}
	return nullptr;
}

static CmRemoteUid *CmAllocRemoteUid(const Device *dev)
{
	CmRemoteUid *e = CmFindRemoteUid(dev);
	if (e != nullptr) {
		return e;
	}
	for (uint8_t i = 0u; i < 16u; i++) {
		if (s_remote_uid[i].valid) {
			continue;
		}
		memset(&s_remote_uid[i], 0, sizeof(s_remote_uid[i]));
		s_remote_uid[i].dev = *dev;
		return &s_remote_uid[i];
	}
	return nullptr;
}

static void CmRequestRemoteUidIfNeeded(const Device *dev)
{
	if (CmFindRemoteUid(dev) != nullptr || ConfigSync_IsBusy()) {
		return;
	}
	ConfigSync_StartReadMcuUid(dev);
}

static void CmUpdateMissing(uint32_t now_ms)
{
	(void)now_ms;
	for (uint8_t slot = 0u; slot < 32u; slot++) {
		const Device *mcu = &PPKYConfig.CfgDevices[slot].UId.devId;
		if (!IsMcuType(mcu->d_type)) {
			s_missing_latched[slot] = 0u;
			continue;
		}

		uint8_t mcu_raw = CmIsMcuMissingSlot(slot);
		if (mcu_raw != 0u && s_missing_latched[slot] == 0u) {
			s_missing_latched[slot] = 1u;
			CmPostMissingEvent(mcu, slot, 0u, 0u);
		} else if (mcu_raw == 0u && s_missing_latched[slot] != 0u) {
			s_missing_latched[slot] = 0u;
			CmPostMissingEvent(mcu, slot, 0u, 1u);
		}
	}
}

static void CmUpdateFound(uint32_t now_ms)
{
	uint8_t seen[32];
	memset(seen, 0, sizeof(seen));

	for (uint8_t ai = 0u; ai < g_active_devices_count; ai++) {
		const ActiveDeviceInfo *ad = &g_active_devices[ai];
		if (!ad->online || !IsMcuType(ad->dev.d_type)) {
			continue;
		}

		CmDevKey mcu_key = {};
		mcu_key.zone = ad->dev.zone;
		mcu_key.h_adr = ad->dev.h_adr;
		mcu_key.l_adr = ad->dev.l_adr;
		mcu_key.d_type = ad->dev.d_type;

		if (CmIsMcuNew(&ad->dev)) {
			CmFoundDebounce *db = CmAllocFoundDb(&mcu_key);
			if (db != nullptr) {
				if (db->latched == 0u) {
					if (db->phase_since_ms == 0u) {
						CmStartFoundPhase(db, ad->last_seen_ms, now_ms);
					} else {
						CmCountFoundRx(db, ad->last_seen_ms);
					}
					if (CmFoundPhaseDone(db, now_ms)) {
						db->latched = 1u;
						CmResetFoundDb(db);
						CmPostFoundEvent(&ad->dev, 0u);
						CmRequestRemoteUidIfNeeded(&ad->dev);
					}
				}
				for (uint8_t i = 0u; i < s_found_latched_count; i++) {
					if (CmKeysEqual(&s_found_db[i].key, &mcu_key)) {
						seen[i] = 1u;
					}
				}
			}
		}
	}

	for (uint8_t i = 0u; i < s_found_latched_count; i++) {
		CmFoundDebounce *db = &s_found_db[i];
		if (db->latched == 0u) {
			if (!seen[i]) {
				CmResetFoundDb(db);
			}
			continue;
		}
		if (seen[i]) {
			continue;
		}
		db->latched = 0u;
		CmResetFoundDb(db);
		Device dev = {};
		dev.zone = db->key.zone;
		dev.h_adr = db->key.h_adr;
		dev.l_adr = db->key.l_adr;
		dev.d_type = db->key.d_type;
		CmPostFoundEvent(&dev, 1u);
	}
}

static void CmUpdateCrcFault(uint8_t slot, uint8_t active)
{
	const Device *dev = &PPKYConfig.CfgDevices[slot].UId.devId;
	if (active != 0u) {
		if (s_crc_fail_streak[slot] < 255u) {
			s_crc_fail_streak[slot]++;
		}
		if (s_crc_fail_streak[slot] >= CM_CRC_FAIL_LATCH && s_crc_latched[slot] == 0u) {
			s_crc_latched[slot] = 1u;
			CmPostConfigMismatchEvent(slot, dev, 0u);
		}
		return;
	}

	s_crc_fail_streak[slot] = 0u;
	if (s_crc_latched[slot] != 0u) {
		s_crc_latched[slot] = 0u;
		CmPostConfigMismatchEvent(slot, dev, 1u);
	}
}

static void CmSchedulePeriodicCrc(uint32_t now_ms)
{
	if (ConfigSync_IsBusy()) {
		return;
	}
	if (ConfigIgnBlockSync_ShouldDeferCrc() != 0u) {
		return;
	}
	if (!TickAgeExpiredMs(now_ms, s_last_crc_ms, CM_CRC_PERIOD_MS)) {
		return;
	}

	for (uint8_t n = 0u; n < 32u; n++) {
		uint8_t slot = (uint8_t)((s_crc_rr_slot + n) % 32u);
		s_crc_rr_slot = (uint8_t)((slot + 1u) % 32u);

		if (!IsMcuType(PPKYConfig.CfgDevices[slot].UId.devId.d_type)) {
			continue;
		}
		if (s_missing_latched[slot] != 0u) {
			continue;
		}
		if (CmIsMcuMissingOrNewSlot(slot)) {
			continue;
		}
		if (CmFindActiveForCfgDev(&PPKYConfig.CfgDevices[slot].UId.devId, 1u) < 0) {
			continue;
		}

		s_last_crc_ms = now_ms;
		ConfigSync_StartPeriodicVerifySlot(slot);
		return;
	}
}

extern "C" void ConfigMonitor_Init(uint32_t boot_ms)
{
	s_boot_ms = boot_ms;
	memset(s_missing_latched, 0, sizeof(s_missing_latched));
	memset(s_crc_latched, 0, sizeof(s_crc_latched));
	memset(s_crc_fail_streak, 0, sizeof(s_crc_fail_streak));
	memset(s_found_db, 0, sizeof(s_found_db));
	memset(s_remote_uid, 0, sizeof(s_remote_uid));
	s_found_latched_count = 0u;
	s_last_crc_ms = 0u;
	s_crc_rr_slot = 0u;
}

extern "C" void ConfigMonitor_Process1ms(uint32_t now_ms)
{
	s_now_ms = now_ms;
	if (!CmCanRun(now_ms)) {
		return;
	}

	CmUpdateMissing(now_ms);
	CmUpdateFound(now_ms);
	CmSchedulePeriodicCrc(now_ms);
}

extern "C" void ConfigMonitor_OnPeriodicCrcResult(uint8_t slot, uint8_t crc_match, uint8_t comm_failed)
{
	if (slot >= 32u) {
		return;
	}
	if (comm_failed != 0u) {
		return;
	}
	if (crc_match != 0u) {
		CmUpdateCrcFault(slot, 0u);
	} else {
		CmUpdateCrcFault(slot, 1u);
	}
}

extern "C" void ConfigMonitor_OnRemoteUidRead(const Device *dev, const uint8_t *uid_bytes, uint8_t ok)
{
	if (dev == nullptr) {
		return;
	}
	CmRemoteUid *e = CmAllocRemoteUid(dev);
	if (e == nullptr) {
		return;
	}
	if (ok == 0u || uid_bytes == nullptr) {
		return;
	}
	const UniqId *uid = (const UniqId *)uid_bytes;
	e->uid0 = uid->UId0;
	e->uid1 = uid->UId1;
	e->uid2 = uid->UId2;
	e->valid = 1u;
}

extern "C" const uint8_t *ConfigMonitor_GetRemoteSerial(const Device *dev, uint8_t *out_valid)
{
	if (out_valid != nullptr) {
		*out_valid = 0u;
	}
	if (dev == nullptr) {
		return nullptr;
	}
	CmRemoteUid *e = CmFindRemoteUid(dev);
	if (e == nullptr || e->valid == 0u) {
		return nullptr;
	}
	if (out_valid != nullptr) {
		*out_valid = 1u;
	}
	return (const uint8_t *)&e->uid0;
}

extern "C" uint8_t ConfigMonitor_IsMcuMissingLatched(uint8_t cfg_slot)
{
	return (cfg_slot < 32u) ? s_missing_latched[cfg_slot] : 0u;
}

extern "C" uint8_t ConfigMonitor_IsCrcFaultLatched(uint8_t cfg_slot)
{
	return (cfg_slot < 32u) ? s_crc_latched[cfg_slot] : 0u;
}

extern "C" uint8_t ConfigMonitor_GetFoundLatchedCount(void)
{
	uint8_t n = 0u;
	for (uint8_t i = 0u; i < s_found_latched_count; i++) {
		if (s_found_db[i].latched != 0u) {
			n++;
		}
	}
	return n;
}

extern "C" uint8_t ConfigMonitor_GetFoundLatchedKey(uint8_t index, Device *mcu_out,
						    uint8_t *v_l_adr, uint8_t *v_d_type)
{
	uint8_t seen = 0u;
	for (uint8_t i = 0u; i < s_found_latched_count; i++) {
		if (s_found_db[i].latched == 0u) {
			continue;
		}
		if (seen == index) {
			if (mcu_out != nullptr) {
				mcu_out->zone = s_found_db[i].key.zone;
				mcu_out->h_adr = s_found_db[i].key.h_adr;
				mcu_out->l_adr = s_found_db[i].key.l_adr;
				mcu_out->d_type = s_found_db[i].key.d_type;
			}
			if (v_l_adr != nullptr) {
				*v_l_adr = s_found_db[i].key.v_l_adr;
			}
			if (v_d_type != nullptr) {
				*v_d_type = s_found_db[i].key.v_d_type;
			}
			return 1u;
		}
		seen++;
	}
	return 0u;
}

extern "C" const Device *ConfigMonitor_GetCfgDevice(uint8_t cfg_slot)
{
	if (cfg_slot >= 32u) {
		return nullptr;
	}
	return &PPKYConfig.CfgDevices[cfg_slot].UId.devId;
}
