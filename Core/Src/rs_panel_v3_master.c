#include "rs_panel_v3_master.h"

#include "fire.h"
#include "menu_ui.h"
#include "beeper.h"
#include "esp_manager.h"
#include "device_config.h"
#include "backend.h"
#include "warning.h"
#include "event_log.h"
#include "rs_panel_proto.h"
#include "rs_panel_master_debug.h"
#include "main.h"
#include <string.h>

extern PPKYCfg PPKYConfig;

typedef enum {
    RS_V3_STREAM_IDLE = 0u,
    RS_V3_STREAM_ZONE_CLEAR,
    RS_V3_STREAM_ZONE_ADD,
    RS_V3_STREAM_DEV_CLEAR,
    RS_V3_STREAM_DEV_ADD,
    RS_V3_STREAM_FAULT_CLEAR,
    RS_V3_STREAM_FAULT_SET
} RsV3StreamPhase;

typedef struct {
    uint8_t pending_ack_seq;
    uint8_t pending_ack_result;
    uint8_t has_pending_ack;
    uint8_t last_event_seq;
    uint16_t expected_zones_crc;
    uint16_t expected_devices_crc;
    uint16_t expected_faults_crc;
    uint8_t stream_active;
    RsV3StreamPhase stream_phase;
    uint16_t stream_idx;
    uint16_t fault_piggy_idx; /* SET неисправностей во время стрима зон/устройств */
    uint32_t last_catalog_resync_ms; /* анти-флап catalog CRC rewrite */
    uint8_t stream_need_devices; /* 1 = после ZONE продолжить DEV */
    uint8_t catalog_crc_ok_latched; /* 1 = уже учли skip при совпадении CRC */
    uint8_t catalog_after_faults; /* 1 = после FAULT_SET продолжить ZONE/DEV */
    /* TIME TLV — per-panel (раньше один s_last_time_ms → вторая панель без часов). */
    uint32_t last_time_ms;
    /* Reply/journal — per-panel (глобальный pending затирал ответ соседней панели). */
    RsPanelV3EventReply pending_reply;
    uint8_t has_pending_reply;
    uint32_t pending_reply_since_ms; /* для yield journal поверх длинного FAULT stream */
    RsPanelV3EventReply cached_data_reply;
    uint8_t cached_data_reply_seq;
    uint8_t has_cached_data_reply;
} RsPanelV3PanelCtx;

/* FAULT важнее prefetch журнала, но не дольше этого — иначе GET зависает на весь SET. */
#define RS_V3_REPLY_YIELD_MS 120u

extern uint8_t RsPanelV3Proto_DispatchDataEvent(uint8_t panel_idx,
                                                const RsPanelV3Event *ev,
                                                RsPanelV3EventReply *reply);

static void rs_v3_latch_pending_reply(RsPanelV3PanelCtx *ctx, uint32_t now_ms)
{
    if (ctx == 0) {
        return;
    }
    ctx->has_pending_reply = 1u;
    if (ctx->pending_reply_since_ms == 0u) {
        ctx->pending_reply_since_ms = (now_ms != 0u) ? now_ms : 1u;
    }
}

static uint32_t s_boot_ms;
static uint8_t s_boot_valid;
static RsPanelV3PanelCtx s_ctx[RS_PANEL_MAX_PANELS];

static RsPanelV3FaultEvtItem s_fault_items[RS_PANEL_V3_MAX_FAULTS];
static uint16_t s_fault_item_count;
static uint8_t s_fault_snapshot_dirty;

static uint16_t s_devices_catalog_crc;
static uint16_t s_zones_catalog_crc;

/* need_zones / need_devices — раздельный rewrite по CRC; FAULT отдельно. */
static void rs_v3_panel_begin_catalog(uint8_t panel_idx,
                                     uint8_t need_zones,
                                     uint8_t need_devices)
{
    RsPanelV3PanelCtx *ctx;
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    if (need_zones == 0u && need_devices == 0u) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    ctx->stream_active = 1u;
    ctx->stream_idx = 0u;
    ctx->fault_piggy_idx = 0u;
    ctx->stream_need_devices = (need_devices != 0u) ? 1u : 0u;
    ctx->catalog_crc_ok_latched = 0u;
    ctx->catalog_after_faults = 0u;
    ctx->stream_phase = (need_zones != 0u) ? RS_V3_STREAM_ZONE_CLEAR
                                           : RS_V3_STREAM_DEV_CLEAR;
    /* NEED_CATALOG / reboot: панель могла потерять RTC — не ждать PERIOD. */
    if (need_zones != 0u && need_devices != 0u) {
        ctx->last_time_ms = 0u;
    }
    if (need_zones != 0u) {
        g_rs_master_dbg.catalog_rewrite_zones++;
    }
    if (need_devices != 0u) {
        g_rs_master_dbg.catalog_rewrite_devices++;
    }
}

static uint8_t rs_v3_is_mcu_type(uint8_t d_type)
{
    return (d_type == DEVICE_MCU_IGN_TYPE || d_type == DEVICE_MCU_TC_TYPE ||
            d_type == DEVICE_MCU_K1 || d_type == DEVICE_MCU_K2 ||
            d_type == DEVICE_MCU_K3 || d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

static uint16_t rs_v3_strnlen_local(const char *s, uint16_t max_len)
{
    uint16_t n = 0u;
    if (s == 0) {
        return 0u;
    }
    /* 0xFF — стёртая Flash; не считаем частью имени. */
    while (n < max_len && s[n] != '\0' && (uint8_t)s[n] != 0xFFu) {
        n++;
    }
    return n;
}

static void rs_v3_copy_zone_name(char *dst, const int8_t *src)
{
    uint16_t n;
    if (dst == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == 0) {
        return;
    }
    n = rs_v3_strnlen_local((const char *)src, ZONE_NAME_SIZE);
    while (n > 0u && ((const char *)src)[n - 1u] == ' ') {
        n--;
    }
    if (n >= RS_PANEL_V3_ZONE_NAME_LEN) {
        n = (uint16_t)(RS_PANEL_V3_ZONE_NAME_LEN - 1u);
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint8_t rs_v3_zone_name_nonempty(uint8_t zone_1based)
{
    const char *name;
    uint16_t n;
    uint16_t i;
    uint8_t any_printable = 0u;
    if (zone_1based == 0u || zone_1based > ZONE_NUMBER) {
        return 0u;
    }
    name = (const char *)PPKYConfig.zone_name[zone_1based - 1u];
    if ((uint8_t)name[0] == 0xFFu || name[0] == '\0') {
        return 0u;
    }
    n = rs_v3_strnlen_local(name, ZONE_NAME_SIZE);
    while (n > 0u && (name[n - 1u] == ' ' || name[n - 1u] == '\0')) {
        n--;
    }
    for (i = 0u; i < n; i++) {
        const uint8_t c = (uint8_t)name[i];
        if (c >= 0x20u && c != 0xFFu) {
            any_printable = 1u;
            break;
        }
    }
    return (n > 0u && any_printable != 0u) ? 1u : 0u;
}

static void rs_v3_fill_device_from_slot(uint8_t slot, RsPanelV3DeviceItem *d)
{
    const MKUCfg *mku;
    const Device *dev;
    uint8_t vi;
    if (d == 0 || slot >= MAX_MCU_IN_BUS) {
        return;
    }
    mku = &PPKYConfig.CfgDevices[slot];
    dev = &mku->UId.devId;
    memset(d, 0, sizeof(*d));
    d->op = (uint8_t)RS_PANEL_V3_STREAM_OP_ADD;
    d->d_type = dev->d_type;
    d->h_adr = dev->h_adr;
    d->l_adr = dev->l_adr;
    d->zone = dev->zone;
    d->uid0 = mku->UId.UId0;
    d->uid1 = mku->UId.UId1;
    d->uid2 = mku->UId.UId2;
    d->n_ch = 0u;
    for (vi = 0u; vi < RS_PANEL_V3_MAX_MCU_CHANNELS; vi++) {
        uint8_t ch_type = (uint8_t)(mku->VDtype[vi] & 0xFFu);
        if (ch_type == 0u) {
            continue;
        }
        if (d->n_ch >= RS_PANEL_V3_MAX_MCU_CHANNELS) {
            break;
        }
        d->ch[d->n_ch].ch_type = ch_type;
        d->ch[d->n_ch].ch_l_adr = (uint8_t)(vi + 1u);
        d->n_ch++;
    }
}

static void rs_v3_refresh_catalog_crcs(void)
{
    uint8_t zi;
    uint8_t slot;
    char name[RS_PANEL_V3_ZONE_NAME_LEN];
    RsPanelV3DeviceItem tmp;

    s_zones_catalog_crc = 0xFFFFu;
    for (zi = 1u; zi <= ZONE_NUMBER; zi++) {
        if (rs_v3_zone_name_nonempty(zi) == 0u) {
            continue;
        }
        rs_v3_copy_zone_name(name, PPKYConfig.zone_name[zi - 1u]);
        s_zones_catalog_crc = RsPanelV3_ZoneCatalogCrc_Add(s_zones_catalog_crc, zi, name);
    }

    s_devices_catalog_crc = 0xFFFFu;
    for (slot = 0u; slot < MAX_MCU_IN_BUS; slot++) {
        if (rs_v3_is_mcu_type(PPKYConfig.CfgDevices[slot].UId.devId.d_type) == 0u) {
            continue;
        }
        rs_v3_fill_device_from_slot(slot, &tmp);
        s_devices_catalog_crc = RsPanelV3_DeviceCatalogCrc_Add(s_devices_catalog_crc, &tmp);
    }
}

static void rs_v3_update_expected_fault_crc(void)
{
    uint8_t i;
    s_ctx[0].expected_faults_crc = RsPanelV3_FaultEvtCrc(s_fault_items, s_fault_item_count);
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        s_ctx[i].expected_zones_crc = s_zones_catalog_crc;
        s_ctx[i].expected_devices_crc = s_devices_catalog_crc;
        s_ctx[i].expected_faults_crc = s_ctx[0].expected_faults_crc;
    }
}

void RsPanelV3Master_Init(void)
{
    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_fault_items, 0, sizeof(s_fault_items));
    s_fault_item_count = 0u;
    s_fault_snapshot_dirty = 1u;
    s_boot_valid = 0u;
    rs_v3_refresh_catalog_crcs();
    rs_v3_update_expected_fault_crc();
    /* Каталог на панели — по CRC в RSP при опросе, не force stream на Init. */
}

void RsPanelV3Master_OnBoot(uint32_t boot_ms)
{
    s_boot_ms = boot_ms;
    s_boot_valid = 1u;
}

void RsPanelV3Master_RequestCatalogResync(void)
{
    uint8_t i;
    rs_v3_refresh_catalog_crcs();
    rs_v3_update_expected_fault_crc();
    /* Сброс анти-флапа: следующий RSP сравнит CRC и перепишет только mismatch. */
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        s_ctx[i].last_catalog_resync_ms = 0u;
    }
}

void RsPanelV3Master_RequestCatalogResyncPanel(uint8_t panel_idx)
{
    rs_v3_refresh_catalog_crcs();
    rs_v3_update_expected_fault_crc();
    if (panel_idx < RS_PANEL_MAX_PANELS) {
        s_ctx[panel_idx].last_catalog_resync_ms = 0u;
    }
}

void RsPanelV3Master_ForceTimeSyncPanel(uint8_t panel_idx)
{
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    s_ctx[panel_idx].last_time_ms = 0u;
}

void RsPanelV3Master_RemovePanelSlot(uint8_t panel_idx)
{
    uint8_t j;
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    for (j = panel_idx; (uint8_t)(j + 1u) < RS_PANEL_MAX_PANELS; j++) {
        s_ctx[j] = s_ctx[j + 1u];
    }
    memset(&s_ctx[RS_PANEL_MAX_PANELS - 1u], 0, sizeof(s_ctx[0]));
}

uint8_t RsPanelV3Master_IsV3PollActive(void)
{
    return 1u;
}

uint8_t RsPanelV3Master_IsSysReady(void)
{
    if (s_boot_valid == 0u) {
        return 0u;
    }
    /* SYS_READY: пауза ~10 с окончена И был ≥1 WarningProcess+CommitFaultSnapshot.
     * Иначе панель снимает «ПРОВЕРКА» → «НОРМА», а FAULT_SET ещё не ушёл. */
    if (Warning_IsProcessDelayActive() != 0u) {
        return 0u;
    }
    return Warning_IsFaultPipelinePrimed();
}

uint32_t RsPanelV3Master_GetBootAgeMs(void)
{
    if (s_boot_valid == 0u) {
        return 0u;
    }
    return (uint32_t)(HAL_GetTick() - s_boot_ms);
}

RsPanelV3FaultEvtItem *RsPanelV3Master_FaultItemsWritable(uint16_t *max_count)
{
    if (max_count != 0) {
        *max_count = RS_PANEL_V3_MAX_FAULTS;
    }
    return s_fault_items;
}

static uint8_t rs_v3_phase_is_catalog(RsV3StreamPhase phase)
{
    return (phase == RS_V3_STREAM_ZONE_CLEAR ||
            phase == RS_V3_STREAM_ZONE_ADD ||
            phase == RS_V3_STREAM_DEV_CLEAR ||
            phase == RS_V3_STREAM_DEV_ADD) ? 1u : 0u;
}

/* FAULT_CLEAR/SET для слота; ZONE/DEV прерываем и возобновляем после faults. */
static void rs_v3_panel_schedule_fault_resync(uint8_t panel_idx, uint8_t need_clear)
{
    RsPanelV3PanelCtx *ctx;
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    if (ctx->stream_active != 0u && rs_v3_phase_is_catalog(ctx->stream_phase) != 0u) {
        /* Каталог больше НЕ заканчивается FAULT_SET — нельзя skip'ать forever.
         * Прерываем: после FAULT_* catalog_after_faults вернёт ZONE_CLEAR. */
        ctx->catalog_after_faults = 1u;
        if (ctx->stream_phase == RS_V3_STREAM_DEV_CLEAR ||
            ctx->stream_phase == RS_V3_STREAM_DEV_ADD) {
            ctx->stream_need_devices = 1u;
        }
    }
    ctx->stream_active = 1u;
    ctx->stream_phase = (need_clear != 0u) ? RS_V3_STREAM_FAULT_CLEAR
                                            : RS_V3_STREAM_FAULT_SET;
    ctx->stream_idx = 0u;
    ctx->fault_piggy_idx = 0u;
}

void RsPanelV3Master_CommitFaultSnapshot(uint16_t count)
{
    uint16_t n = count;
    uint16_t new_crc;
    uint16_t old_n;
    uint8_t i;
    uint8_t need_clear;
    if (n > RS_PANEL_V3_MAX_FAULTS) {
        n = RS_PANEL_V3_MAX_FAULTS;
    }
    /* Warning пишет прямо в s_fault_items — второй буфер не влезает в RAM. */
    new_crc = RsPanelV3_FaultEvtCrc(s_fault_items, n);
    if (n == s_fault_item_count && new_crc == s_ctx[0].expected_faults_crc) {
        return;
    }
    old_n = s_fault_item_count;
    /*
     * SET на панели только добавляет / игнорит дубликаты — не удаляет.
     * Укорочение / замена при том же n → CLEAR_ALL+SET.
     * Рост n → только SET (быстро; панель вставит новую в начало списка).
     * n==0 → CLEAR_ALL каждой панели (иначе sticky UI при CRC уже empty).
     */
    need_clear = (n <= old_n) ? 1u : 0u;
    s_fault_item_count = n;
    s_fault_snapshot_dirty = 1u;
    rs_v3_update_expected_fault_crc();
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        rs_v3_panel_schedule_fault_resync(i, need_clear);
    }
}

void RsPanelV3Master_SetFaultSnapshot(const RsPanelV3FaultEvtItem *items, uint16_t count)
{
    uint16_t i;
    uint16_t n = count;
    if (items == 0) {
        n = 0u;
    }
    if (n > RS_PANEL_V3_MAX_FAULTS) {
        n = RS_PANEL_V3_MAX_FAULTS;
    }
    if (items == s_fault_items) {
        RsPanelV3Master_CommitFaultSnapshot(n);
        return;
    }
    for (i = 0u; i < n; i++) {
        s_fault_items[i] = items[i];
    }
    RsPanelV3Master_CommitFaultSnapshot(n);
}

static void rs_v3_fill_time(RsPanelV3Time *t)
{
    RTC_TimeTypeDef time = {0};
    RTC_DateTypeDef date = {0};
    if (t == 0) {
        return;
    }
    (void)HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(&hrtc, &date, RTC_FORMAT_BIN);
    t->hour = time.Hours;
    t->min = time.Minutes;
    t->sec = time.Seconds;
    t->day = date.Date;
    t->month = date.Month;
    t->year = date.Year;
}

/* TIME в POLL: сразу (last_time_ms==0) / период / каждый POLL во время
 * каталога (панель могла пропустить ранние кадры, пока поднималась). */
static void rs_v3_maybe_attach_time(RsPanelV3Poll *out, RsPanelV3PanelCtx *ctx,
                                   uint32_t now_ms)
{
    if (out == 0 || ctx == 0) {
        return;
    }
    if (ctx->stream_active != 0u ||
        ctx->last_time_ms == 0u ||
        (now_ms - ctx->last_time_ms) >= RS_PANEL_V3_TIME_SYNC_PERIOD_MS) {
        ctx->last_time_ms = now_ms;
        out->has_time = 1u;
        out->sys.flags |= RS_PANEL_V3_SYS_TIME_VALID;
        rs_v3_fill_time(&out->time);
    }
}

static uint8_t rs_v3_emit_stream_op(RsPanelV3Poll *out, RsPanelV3PanelCtx *ctx)
{
    if (out == 0 || ctx == 0 || ctx->stream_active == 0u) {
        return 0u;
    }

    switch (ctx->stream_phase) {
    case RS_V3_STREAM_ZONE_CLEAR:
        out->has_zone_names = 1u;
        out->zone_names.op = (uint8_t)RS_PANEL_V3_STREAM_OP_CLEAR;
        ctx->stream_phase = RS_V3_STREAM_ZONE_ADD;
        ctx->stream_idx = 0u;
        return 1u;
    case RS_V3_STREAM_ZONE_ADD:
        while (ctx->stream_idx < ZONE_NUMBER) {
            uint8_t zone = (uint8_t)(ctx->stream_idx + 1u);
            ctx->stream_idx++;
            if (rs_v3_zone_name_nonempty(zone) == 0u) {
                continue;
            }
            out->has_zone_names = 1u;
            out->zone_names.op = (uint8_t)RS_PANEL_V3_STREAM_OP_ADD;
            out->zone_names.zone = zone;
            rs_v3_copy_zone_name(out->zone_names.name, PPKYConfig.zone_name[zone - 1u]);
            return 1u;
        }
        if (ctx->stream_need_devices == 0u) {
            /* Только зоны: FAULT синхронизируется отдельно по faults_crc. */
            ctx->stream_active = 0u;
            ctx->stream_phase = RS_V3_STREAM_IDLE;
            ctx->stream_idx = 0u;
            /* TIME снова: кадры с TIME во время stream могли потеряться на шине. */
            ctx->last_time_ms = 0u;
            return 0u;
        }
        ctx->stream_phase = RS_V3_STREAM_DEV_CLEAR;
        ctx->stream_idx = 0u;
        /* fallthrough */
    case RS_V3_STREAM_DEV_CLEAR:
        out->has_devices = 1u;
        out->device.op = (uint8_t)RS_PANEL_V3_STREAM_OP_CLEAR;
        ctx->stream_phase = RS_V3_STREAM_DEV_ADD;
        ctx->stream_idx = 0u;
        return 1u;
    case RS_V3_STREAM_DEV_ADD:
        while (ctx->stream_idx < MAX_MCU_IN_BUS) {
            uint8_t slot = (uint8_t)ctx->stream_idx;
            ctx->stream_idx++;
            if (rs_v3_is_mcu_type(PPKYConfig.CfgDevices[slot].UId.devId.d_type) == 0u) {
                continue;
            }
            out->has_devices = 1u;
            rs_v3_fill_device_from_slot(slot, &out->device);
            out->device.op = (uint8_t)RS_PANEL_V3_STREAM_OP_ADD;
            return 1u;
        }
        /* Каталог ZONE/DEV завершён. FAULT не форсируем здесь: Commit/OnRsp
         * ставят FAULT_* (в т.ч. прерывая каталог). Idle — ок при совпадении CRC. */
        ctx->stream_active = 0u;
        ctx->stream_phase = RS_V3_STREAM_IDLE;
        ctx->stream_idx = 0u;
        ctx->stream_need_devices = 0u;
        ctx->last_time_ms = 0u;
        return 0u;
    case RS_V3_STREAM_FAULT_CLEAR:
        out->has_fault_evt = 1u;
        out->fault_evt.op = (uint8_t)RS_PANEL_V3_FAULT_EVT_CLEAR_ALL;
        ctx->stream_phase = RS_V3_STREAM_FAULT_SET;
        ctx->stream_idx = 0u;
        return 1u;
    case RS_V3_STREAM_FAULT_SET:
    rs_v3_stream_fault_set:
        if (ctx->stream_idx < s_fault_item_count) {
            out->has_fault_evt = 1u;
            out->fault_evt = s_fault_items[ctx->stream_idx];
            out->fault_evt.op = (uint8_t)RS_PANEL_V3_FAULT_EVT_SET;
            ctx->stream_idx++;
            return 1u;
        }
        if (ctx->catalog_after_faults != 0u) {
            ctx->catalog_after_faults = 0u;
            ctx->stream_phase = RS_V3_STREAM_ZONE_CLEAR;
            ctx->stream_idx = 0u;
            return rs_v3_emit_stream_op(out, ctx);
        }
        ctx->stream_active = 0u;
        ctx->stream_phase = RS_V3_STREAM_IDLE;
        ctx->stream_idx = 0u;
        ctx->last_time_ms = 0u;
        return 0u;
    default:
        ctx->stream_active = 0u;
        return 0u;
    }
}

void RsPanelV3Master_BuildPoll(RsPanelV3Poll *out, uint8_t panel_idx, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t need_reply;
    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    need_reply = (ctx->has_pending_reply != 0u) ? 1u : 0u;

    if (RsPanelV3Master_IsSysReady() != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_READY;
    }
    if (Fire_IsActive() != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_FIRE_ACTIVE;
    }
    if (MenuUi_IsConfigSessionActive() != 0u) {
        MenuCfgState st = MenuConfig_GetState();
        out->sys.flags |= RS_PANEL_V3_SYS_CONFIG_ACTIVE;
        out->has_config = 1u;
        out->config.percent = MenuConfig_GetPercent();
        if (st == MENU_CFG_STATE_SUCCESS) {
            out->config.phase = (uint8_t)RS_PANEL_V3_CFG_SUCCESS;
        } else if (st == MENU_CFG_STATE_APPLYING) {
            out->config.phase = (uint8_t)RS_PANEL_V3_CFG_RUNNING;
        } else if (st == MENU_CFG_STATE_RECEIVING) {
            out->config.phase = (uint8_t)RS_PANEL_V3_CFG_RUNNING;
        } else {
            out->config.phase = (uint8_t)RS_PANEL_V3_CFG_IDLE;
        }
    }
    if (s_fault_item_count > 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_HAS_FAULTS;
    }
    if (Warning_GetPpkuInputFaultMask() != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_POWER_INPUT_FAULT;
    }
    if (PPKYConfig.beep != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_SOUND_ON;
    }
    if (PPKYConfig.beep_block != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_SOUND_BLOCKED;
    }
    if (EspManager_IsUserWifiOn() != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_WIFI_ON;
    }
    if (EspManager_IsLinkActive() != 0u) {
        out->sys.flags |= RS_PANEL_V3_SYS_WIFI_LINK;
    }

    /*
     * Журнал/данные: ACK без event_reply освобождает PostEvent на панели —
     * следующее нажатие снова двигает selected, а reply ещё в очереди → UI
     * прыгает на 2. Пока reply pending — обычно отдаём ACK+reply и не шлём
     * каталог. Исключение: FAULT_CLEAR/SET важнее prefetch журнала — иначе
     * «лишняя» неисправность на панели залипает, пока идёт JOURNAL_GET_N.
     * Но не дольше RS_V3_REPLY_YIELD_MS: иначе при 2 панелях + длинном FAULT
     * JOURNAL_GET не получает ACK → пустой журнал до конца stream.
     */
    if (need_reply != 0u &&
        ctx->stream_active != 0u &&
        (ctx->stream_phase == RS_V3_STREAM_FAULT_CLEAR ||
         ctx->stream_phase == RS_V3_STREAM_FAULT_SET)) {
        uint32_t reply_age = 0u;
        if (ctx->pending_reply_since_ms != 0u) {
            reply_age = now_ms - ctx->pending_reply_since_ms;
        }
        if (reply_age < RS_V3_REPLY_YIELD_MS) {
            /* Не трогаем ACK/reply: ACK без reply освободит слот на панели. */
            (void)rs_v3_emit_stream_op(out, ctx);
            rs_v3_maybe_attach_time(out, ctx, now_ms);
            (void)s_fault_snapshot_dirty;
            return;
        }
        /* yield → отдать reply; FAULT продолжится со следующего POLL. */
    }

    if (need_reply != 0u) {
        out->has_config = 0u;
        memset(&out->config, 0, sizeof(out->config));
        if (ctx->has_pending_ack != 0u) {
            out->ack.seq = ctx->pending_ack_seq;
            out->ack.result = ctx->pending_ack_result;
            ctx->has_pending_ack = 0u;
        }
        out->has_event_reply = 1u;
        out->event_reply = ctx->pending_reply;
        ctx->has_pending_reply = 0u;
        ctx->pending_reply_since_ms = 0u;
        memset(&ctx->pending_reply, 0, sizeof(ctx->pending_reply));
        rs_v3_maybe_attach_time(out, ctx, now_ms);
        (void)s_fault_snapshot_dirty;
        return;
    }

    if (ctx->has_pending_ack != 0u) {
        out->ack.seq = ctx->pending_ack_seq;
        out->ack.result = ctx->pending_ack_result;
        ctx->has_pending_ack = 0u;
    }

    (void)rs_v3_emit_stream_op(out, ctx);

    /* TIME — с первых POLL (last_time_ms==0) и далее по периоду, в т.ч. во
     * время каталога. EncodePollToBuf при нехватке места优先рует TIME над
     * стримом (не срезает TIME в пользу zone/MCU TLV).
     * ZONES fire — только без stream TLV. TIME — per-panel. */
    {
        const uint8_t fire_active = (Fire_IsActive() != 0u) ? 1u : 0u;

        if (fire_active != 0u) {
            out->has_zone_names = 0u;
            out->has_devices = 0u;
        }

        rs_v3_maybe_attach_time(out, ctx, now_ms);

        if (out->has_zone_names == 0u && out->has_devices == 0u &&
            out->has_fault_evt == 0u) {
            Fire_FillV3Zones(&out->zones, now_ms);
            out->has_zones = (out->zones.count > 0u) ? 1u : 0u;
        }
    }

    (void)s_fault_snapshot_dirty;
}

/* Минимальный POLL без RsPanelV3Poll на стеке повторно — только SYS flags. */
uint16_t RsPanelV3Master_EncodeSysOnlyPoll(uint8_t *dst, uint16_t dst_size)
{
    uint16_t flags = 0u;
    if (dst == 0 || dst_size < 5u) {
        return 0u;
    }
    if (RsPanelV3Master_IsSysReady() != 0u) {
        flags |= RS_PANEL_V3_SYS_READY;
    }
    if (Fire_IsActive() != 0u) {
        flags |= RS_PANEL_V3_SYS_FIRE_ACTIVE;
    }
    if (s_fault_item_count > 0u) {
        flags |= RS_PANEL_V3_SYS_HAS_FAULTS;
    }
    if (Warning_GetPpkuInputFaultMask() != 0u) {
        flags |= RS_PANEL_V3_SYS_POWER_INPUT_FAULT;
    }
    if (PPKYConfig.beep != 0u) {
        flags |= RS_PANEL_V3_SYS_SOUND_ON;
    }
    if (PPKYConfig.beep_block != 0u) {
        flags |= RS_PANEL_V3_SYS_SOUND_BLOCKED;
    }
    if (EspManager_IsUserWifiOn() != 0u) {
        flags |= RS_PANEL_V3_SYS_WIFI_ON;
    }
    if (EspManager_IsLinkActive() != 0u) {
        flags |= RS_PANEL_V3_SYS_WIFI_LINK;
    }
    dst[0] = RS_PANEL_V3_VERSION;
    dst[1] = (uint8_t)RS_PANEL_V3_TAG_SYS;
    dst[2] = 2u;
    dst[3] = (uint8_t)(flags & 0xFFu);
    dst[4] = (uint8_t)(flags >> 8);
    return 5u;
}

uint16_t RsPanelV3Master_EncodePollToBuf(uint8_t *dst, uint16_t dst_size,
                                        uint8_t panel_idx, uint32_t now_ms)
{
    RsPanelV3Poll poll;
    RsPanelV3PanelCtx *ctx;
    RsPanelV3PanelCtx saved_ctx;
    uint16_t len;
    uint8_t want_time;
    uint8_t has_stream_tlv;

    if (dst == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return 0u;
    }
    ctx = &s_ctx[panel_idx];
    saved_ctx = *ctx;

    RsPanelV3Master_BuildPoll(&poll, panel_idx, now_ms);
    want_time = poll.has_time;
    len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
    if (len != 0u) {
        if (want_time != 0u && panel_idx < 8u) {
            g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
        }
        return len;
    }

    /* Reply не влез — не отдавать кадр без него (иначе следующий дубликат
     * события раньше слал только ACK → журнал пустой). Откат + SYS, retry. */
    if (poll.has_event_reply != 0u) {
        *ctx = saved_ctx;
        ctx->pending_reply = poll.event_reply;
        rs_v3_latch_pending_reply(ctx, now_ms);
        return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size);
    }

    has_stream_tlv = (poll.has_zone_names != 0u || poll.has_devices != 0u ||
                      poll.has_fault_evt != 0u) ? 1u : 0u;

    /* 1) Срезать ZONES/CONFIG, TIME+стрим оставить. */
    if (poll.has_zones != 0u || poll.has_config != 0u) {
        poll.has_zones = 0u;
        poll.has_config = 0u;
        memset(&poll.zones, 0, sizeof(poll.zones));
        memset(&poll.config, 0, sizeof(poll.config));
        len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
        if (len != 0u) {
            if (want_time != 0u && panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
    }

    /* 2) TIME важнее каталога: откат фазы стрима, кадр SYS+TIME (+ACK).
     * Иначе TIME срезался на весь ZONE/MCU stream → часы 00:00 при NORMA.
     * Следующий POLL продолжит каталог с той же позиции. */
    if (want_time != 0u && has_stream_tlv != 0u) {
        ctx->stream_active = saved_ctx.stream_active;
        ctx->stream_phase = saved_ctx.stream_phase;
        ctx->stream_idx = saved_ctx.stream_idx;
        ctx->fault_piggy_idx = saved_ctx.fault_piggy_idx;
        ctx->stream_need_devices = saved_ctx.stream_need_devices;
        ctx->catalog_after_faults = saved_ctx.catalog_after_faults;

        poll.has_zone_names = 0u;
        poll.has_devices = 0u;
        poll.has_fault_evt = 0u;
        poll.has_zones = 0u;
        poll.has_config = 0u;
        memset(&poll.zone_names, 0, sizeof(poll.zone_names));
        memset(&poll.device, 0, sizeof(poll.device));
        memset(&poll.fault_evt, 0, sizeof(poll.fault_evt));
        memset(&poll.zones, 0, sizeof(poll.zones));
        memset(&poll.config, 0, sizeof(poll.config));

        len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
        if (len != 0u) {
            if (panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
        /* TIME не влез — полный откат (в т.ч. last_time_ms / ACK). */
        *ctx = saved_ctx;
        return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size);
    }

    /* 3) Без TIME или без стрима: срезать TIME, оставить стрим (resync). */
    if (want_time != 0u) {
        ctx->last_time_ms = saved_ctx.last_time_ms;
        poll.has_time = 0u;
        poll.sys.flags = (uint16_t)(poll.sys.flags &
                                   (uint16_t)(~RS_PANEL_V3_SYS_TIME_VALID));
        len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
        if (len != 0u) {
            return len;
        }
    }

    /* Совсем не влезло — откатить фазу и отдать SYS. */
    *ctx = saved_ctx;
    return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size);
}

static void rs_v3_handle_event(uint8_t panel_idx, const RsPanelV3Event *ev, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t result = (uint8_t)RS_PANEL_V3_ACK_OK;
    if (ev == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    if (ev->seq == 0u || ev->type == (uint8_t)RS_PANEL_V3_EVT_NONE) {
        return;
    }
    if (ctx->last_event_seq == ev->seq) {
        /*
         * Панель ещё шлёт тот же seq (ACK/reply не дошли).
         * Не вызывать Dispatch снова (JOURNAL_GET_N = до 3× SPI) — отдать кэш.
         */
        ctx->pending_ack_seq = ev->seq;
        ctx->pending_ack_result = (uint8_t)RS_PANEL_V3_ACK_OK;
        ctx->has_pending_ack = 1u;
        if (ctx->has_pending_reply != 0u) {
            return;
        }
        if (ctx->has_cached_data_reply != 0u &&
            ctx->cached_data_reply_seq == ev->seq) {
            ctx->pending_reply = ctx->cached_data_reply;
            rs_v3_latch_pending_reply(ctx, now_ms);
            return;
        }
        switch (ev->type) {
        case RS_PANEL_V3_EVT_JOURNAL_COUNT:
        case RS_PANEL_V3_EVT_JOURNAL_GET:
        case RS_PANEL_V3_EVT_JOURNAL_GET_N:
        case RS_PANEL_V3_EVT_ZONE_BLOCK_LIST:
        case RS_PANEL_V3_EVT_ZONE_BLOCK_SET:
        case RS_PANEL_V3_EVT_DEVICES_COUNT:
        case RS_PANEL_V3_EVT_DEVICES_GET:
        case RS_PANEL_V3_EVT_DEVICES_GET_N:
        case RS_PANEL_V3_EVT_EXT_CAN_SET: {
            RsPanelV3Event refresh = *ev;
            if (ev->type == (uint8_t)RS_PANEL_V3_EVT_JOURNAL_GET) {
                refresh.u8_a = 3u;
            }
            if (RsPanelV3Proto_DispatchDataEvent(panel_idx, &refresh,
                                                 &ctx->pending_reply) != 0u) {
                ctx->pending_reply.seq = ev->seq;
                ctx->pending_reply.type = ev->type;
                ctx->pending_reply.result = (uint8_t)RS_PANEL_V3_ACK_OK;
                rs_v3_latch_pending_reply(ctx, now_ms);
                ctx->cached_data_reply = ctx->pending_reply;
                ctx->cached_data_reply_seq = ev->seq;
                ctx->has_cached_data_reply = 1u;
            }
            break;
        }
        default:
            break;
        }
        return;
    }

    switch (ev->type) {
    case RS_PANEL_V3_EVT_START_ALL_COMMIT:
        Fire_OnPanelStartAllCommit();
        break;
    case RS_PANEL_V3_EVT_START_SP:
        Fire_OnPanelStartSp(ev->zone);
        break;
    case RS_PANEL_V3_EVT_STOP_LAUNCH:
        Fire_OnPanelStopLaunch(ev->zone);
        break;
    case RS_PANEL_V3_EVT_FIRE_RESET:
        Fire_OnPanelFireReset(ev->zone);
        break;
    case RS_PANEL_V3_EVT_SOUND_SET:
        if (PPKYConfig.beep_block != 0u) {
            result = (uint8_t)RS_PANEL_V3_ACK_DENIED;
        } else {
            const uint8_t on = (ev->u8_a != 0u) ? 1u : 0u;
            if (PPKYConfig.beep != on) {
                PPKYConfig.beep = on;
                Beeper_SoundOnOff(on != 0u);
                EventLog_LogSoundToggle(on, 0u); /* source: panel */
                /* Legacy CMD_SOUND + MENU_TOGGLE; в v3 панели берут SYS flags. */
                RsPanelMaster_PushSound();
                RsPanelMaster_PushMenuSoundState();
            }
        }
        break;
    case RS_PANEL_V3_EVT_WIFI_SET:
        if (ev->u8_a != 0u) {
            EspManager_RequestWifiEnable();
        } else {
            EspManager_RequestWifiDisable();
        }
        /* Панель рисует «Вкл/Выкл» из кэша CONNECTION_STATUS — сразу подтвердить. */
        RsPanelMaster_PushConnectionStatus();
        break;
    case RS_PANEL_V3_EVT_JOURNAL_COUNT:
    case RS_PANEL_V3_EVT_JOURNAL_GET:
    case RS_PANEL_V3_EVT_JOURNAL_GET_N:
    case RS_PANEL_V3_EVT_ZONE_BLOCK_LIST:
    case RS_PANEL_V3_EVT_ZONE_BLOCK_SET:
    case RS_PANEL_V3_EVT_DEVICES_COUNT:
    case RS_PANEL_V3_EVT_DEVICES_GET:
    case RS_PANEL_V3_EVT_DEVICES_GET_N:
    case RS_PANEL_V3_EVT_EXT_CAN_SET:
        if (RsPanelV3Proto_DispatchDataEvent(panel_idx, ev, &ctx->pending_reply) != 0u) {
            ctx->pending_reply.seq = ev->seq;
            ctx->pending_reply.type = ev->type;
            ctx->pending_reply.result = (uint8_t)RS_PANEL_V3_ACK_OK;
            rs_v3_latch_pending_reply(ctx, now_ms);
            ctx->cached_data_reply = ctx->pending_reply;
            ctx->cached_data_reply_seq = ev->seq;
            ctx->has_cached_data_reply = 1u;
            result = (uint8_t)RS_PANEL_V3_ACK_OK;
        } else {
            result = (uint8_t)RS_PANEL_V3_ACK_IGNORED;
        }
        break;
    default:
        result = (uint8_t)RS_PANEL_V3_ACK_IGNORED;
        break;
    }

    ctx->pending_ack_seq = ev->seq;
    ctx->pending_ack_result = result;
    ctx->has_pending_ack = 1u;
    ctx->last_event_seq = ev->seq;
}

void RsPanelV3Master_OnRsp(uint8_t panel_idx, const RsPanelV3Rsp *rsp, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    (void)now_ms;
    if (rsp == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];

    /*
     * faults_crc mismatch → CLEAR_ALL+SET. ZONE/DEV прерываем (каталог сам
     * FAULT больше не шлёт). Уже идущий FAULT_CLEAR/SET не перезапускаем.
     */
    if (rsp->faults_crc != ctx->expected_faults_crc) {
        if (ctx->stream_active == 0u ||
            rs_v3_phase_is_catalog(ctx->stream_phase) != 0u) {
            rs_v3_panel_schedule_fault_resync(panel_idx, 1u);
        }
    } else if (ctx->stream_active == 0u) {
        uint8_t need_zones;
        uint8_t need_devices;
        /* NEED_CATALOG = нет валидного каталога → оба потока.
         * Иначе раздельно zones_crc / devices_crc (пауза 2 с). */
        if ((rsp->flags & RS_PANEL_V3_RSP_FLAG_NEED_CATALOG) != 0u) {
            need_zones = 1u;
            need_devices = 1u;
        } else {
            need_zones = (rsp->zones_crc != ctx->expected_zones_crc) ? 1u : 0u;
            need_devices = (rsp->devices_crc != ctx->expected_devices_crc) ? 1u : 0u;
        }
        if (need_zones != 0u || need_devices != 0u) {
            ctx->catalog_crc_ok_latched = 0u;
            if (ctx->last_catalog_resync_ms == 0u ||
                (now_ms - ctx->last_catalog_resync_ms) >= 2000u) {
                ctx->last_catalog_resync_ms = now_ms;
                rs_v3_panel_begin_catalog(panel_idx, need_zones, need_devices);
            }
        } else if (ctx->catalog_crc_ok_latched == 0u) {
            /* One-shot: оба CRC совпали — rewrite пропущен. */
            ctx->catalog_crc_ok_latched = 1u;
            g_rs_master_dbg.catalog_skip_zones++;
            g_rs_master_dbg.catalog_skip_devices++;
        }
    }

    if (rsp->event.seq != 0u) {
        rs_v3_handle_event(panel_idx, &rsp->event, now_ms);
    }
}
