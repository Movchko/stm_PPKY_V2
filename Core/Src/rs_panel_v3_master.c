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
    uint16_t expected_devices_crc;
    uint16_t expected_faults_crc;
    uint8_t stream_active;
    RsV3StreamPhase stream_phase;
    uint16_t stream_idx;
    uint16_t fault_piggy_idx; /* SET неисправностей во время стрима зон/устройств */
    uint32_t last_full_resync_ms; /* анти-флап полного каталога */
} RsPanelV3PanelCtx;

extern uint8_t RsPanelV3Proto_DispatchDataEvent(uint8_t panel_idx,
                                                const RsPanelV3Event *ev,
                                                RsPanelV3EventReply *reply);

static uint32_t s_boot_ms;
static uint8_t s_boot_valid;
static uint32_t s_last_time_ms;
static RsPanelV3PanelCtx s_ctx[RS_PANEL_MAX_PANELS];

static RsPanelV3EventReply s_pending_reply;
static uint8_t s_pending_reply_panel;
static uint8_t s_has_pending_reply;
/* После отдачи POLL pending очищается; при дубле seq не читать SPI снова. */
static RsPanelV3EventReply s_cached_data_reply;
static uint8_t s_cached_data_reply_seq;
static uint8_t s_cached_data_reply_panel;
static uint8_t s_has_cached_data_reply;

static RsPanelV3FaultEvtItem s_fault_items[RS_PANEL_V3_MAX_FAULTS];
static uint16_t s_fault_item_count;
static uint8_t s_fault_snapshot_dirty;

static uint16_t s_devices_catalog_crc;
static uint16_t s_zones_catalog_crc;

static void rs_v3_panel_begin_stream(uint8_t panel_idx)
{
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    s_ctx[panel_idx].stream_active = 1u;
    s_ctx[panel_idx].stream_phase = RS_V3_STREAM_ZONE_CLEAR;
    s_ctx[panel_idx].stream_idx = 0u;
    s_ctx[panel_idx].fault_piggy_idx = 0u;
}

static void rs_v3_all_panels_begin_stream(void)
{
    uint8_t i;
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        rs_v3_panel_begin_stream(i);
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
        s_ctx[i].expected_devices_crc = s_devices_catalog_crc;
        s_ctx[i].expected_faults_crc = s_ctx[0].expected_faults_crc;
    }
}

void RsPanelV3Master_Init(void)
{
    memset(s_ctx, 0, sizeof(s_ctx));
    memset(s_fault_items, 0, sizeof(s_fault_items));
    memset(&s_pending_reply, 0, sizeof(s_pending_reply));
    s_fault_item_count = 0u;
    s_fault_snapshot_dirty = 1u;
    s_boot_valid = 0u;
    s_last_time_ms = 0u;
    s_has_pending_reply = 0u;
    s_pending_reply_panel = 0xFFu;
    rs_v3_refresh_catalog_crcs();
    rs_v3_update_expected_fault_crc();
    rs_v3_all_panels_begin_stream();
}

void RsPanelV3Master_OnBoot(uint32_t boot_ms)
{
    s_boot_ms = boot_ms;
    s_boot_valid = 1u;
}

void RsPanelV3Master_RequestCatalogResync(void)
{
    rs_v3_refresh_catalog_crcs();
    rs_v3_all_panels_begin_stream();
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
    return ((int32_t)(HAL_GetTick() - s_boot_ms) >=
            (int32_t)RS_PANEL_V3_SYS_READY_DELAY_MS) ? 1u : 0u;
}

RsPanelV3FaultEvtItem *RsPanelV3Master_FaultItemsWritable(uint16_t *max_count)
{
    if (max_count != 0) {
        *max_count = RS_PANEL_V3_MAX_FAULTS;
    }
    return s_fault_items;
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
    new_crc = RsPanelV3_FaultEvtCrc(s_fault_items, n);
    if (n == s_fault_item_count && new_crc == s_ctx[0].expected_faults_crc) {
        return;
    }
    old_n = s_fault_item_count;
    /* CLEAR_ALL только если список реально укоротился. При том же n / росте —
     * только SET (дубликаты на панели игнорируются), без обнуления UI. */
    need_clear = (n < old_n) ? 1u : 0u;
    s_fault_item_count = n;
    s_fault_snapshot_dirty = 1u;
    rs_v3_update_expected_fault_crc();
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        if (s_ctx[i].stream_active != 0u) {
            continue;
        }
        s_ctx[i].stream_active = 1u;
        s_ctx[i].stream_phase = (need_clear != 0u) ? RS_V3_STREAM_FAULT_CLEAR
                                                    : RS_V3_STREAM_FAULT_SET;
        s_ctx[i].stream_idx = 0u;
        s_ctx[i].fault_piggy_idx = 0u;
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
    /* Если пишут прямо в наш буфер — только commit. */
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
        /* CLEAR_ALL только если неисправностей нет (сброс панели).
         * Иначе сразу SET — дубликаты на панели игнорируются, UI не скачет 0→1→n. */
        ctx->stream_idx = 0u;
        if (s_fault_item_count == 0u) {
            out->has_fault_evt = 1u;
            out->fault_evt.op = (uint8_t)RS_PANEL_V3_FAULT_EVT_CLEAR_ALL;
            ctx->stream_phase = RS_V3_STREAM_FAULT_SET;
            return 1u;
        }
        ctx->stream_phase = RS_V3_STREAM_FAULT_SET;
        goto rs_v3_stream_fault_set;
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
        ctx->stream_active = 0u;
        ctx->stream_phase = RS_V3_STREAM_IDLE;
        ctx->stream_idx = 0u;
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
    need_reply = (s_has_pending_reply != 0u && s_pending_reply_panel == panel_idx) ? 1u : 0u;

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

    /*
     * Журнал/данные: ACK без event_reply освобождает PostEvent на панели —
     * следующее нажатие снова двигает selected, а reply ещё в очереди → UI
     * прыгает на 2. Пока reply pending — не шлём стрим/config и отдаём ACK+reply вместе.
     */
    if (need_reply != 0u) {
        out->has_config = 0u;
        memset(&out->config, 0, sizeof(out->config));
        if (ctx->has_pending_ack != 0u) {
            out->ack.seq = ctx->pending_ack_seq;
            out->ack.result = ctx->pending_ack_result;
            ctx->has_pending_ack = 0u;
        }
        out->has_event_reply = 1u;
        out->event_reply = s_pending_reply;
        s_has_pending_reply = 0u;
        s_pending_reply_panel = 0xFFu;
        memset(&s_pending_reply, 0, sizeof(s_pending_reply));
        (void)s_fault_snapshot_dirty;
        (void)now_ms;
        return;
    }

    if (ctx->has_pending_ack != 0u) {
        out->ack.seq = ctx->pending_ack_seq;
        out->ack.result = ctx->pending_ack_result;
        ctx->has_pending_ack = 0u;
    }

    (void)rs_v3_emit_stream_op(out, ctx);

    /* Пока в кадре TLV стрима — не добавлять time/zones (лимит 251). */
    {
        const uint8_t stream_tlv =
            (out->has_zone_names != 0u || out->has_devices != 0u ||
             out->has_fault_evt != 0u) ? 1u : 0u;

        if (stream_tlv == 0u) {
            if (s_last_time_ms == 0u ||
                (now_ms - s_last_time_ms) >= RS_PANEL_V3_TIME_SYNC_PERIOD_MS) {
                s_last_time_ms = now_ms;
                out->has_time = 1u;
                out->sys.flags |= RS_PANEL_V3_SYS_TIME_VALID;
                rs_v3_fill_time(&out->time);
            }

            Fire_FillV3Zones(&out->zones, now_ms);
            out->has_zones = (out->zones.count > 0u) ? 1u : 0u;
        }
    }

    (void)s_fault_snapshot_dirty;
}

/* Минимальный POLL без RsPanelV3Poll на стеке повторно — только SYS flags. */
static uint16_t rs_v3_encode_sys_only(uint8_t *dst, uint16_t dst_size)
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
    uint32_t saved_time_ms;
    uint16_t len;

    if (dst == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return 0u;
    }
    ctx = &s_ctx[panel_idx];
    saved_ctx = *ctx;
    saved_time_ms = s_last_time_ms;

    RsPanelV3Master_BuildPoll(&poll, panel_idx, now_ms);
    len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
    if (len != 0u) {
        return len;
    }

    /* Reply не влез — не отдавать кадр без него (иначе следующий дубликат
     * события раньше слал только ACK → журнал пустой). Откат + SYS, retry. */
    if (poll.has_event_reply != 0u) {
        *ctx = saved_ctx;
        s_last_time_ms = saved_time_ms;
        s_has_pending_reply = 1u;
        s_pending_reply = poll.event_reply;
        s_pending_reply_panel = panel_idx;
        return rs_v3_encode_sys_only(dst, dst_size);
    }

    /* Стрим уже сдвинут в BuildPoll — сначала срезать опциональные TLV и
     * повторить Encode, не откатывая фазу каталога. */
    if (poll.has_zones != 0u || poll.has_time != 0u || poll.has_config != 0u) {
        if (poll.has_time != 0u) {
            s_last_time_ms = saved_time_ms;
            poll.has_time = 0u;
            poll.sys.flags = (uint16_t)(poll.sys.flags &
                                       (uint16_t)(~RS_PANEL_V3_SYS_TIME_VALID));
        }
        poll.has_zones = 0u;
        poll.has_config = 0u;
        memset(&poll.zones, 0, sizeof(poll.zones));
        memset(&poll.config, 0, sizeof(poll.config));
        len = RsPanelV3_EncodePoll(dst, dst_size, &poll);
        if (len != 0u) {
            return len;
        }
    }

    /* Совсем не влезло (даже стрим) — откатить фазу и отдать SYS. */
    *ctx = saved_ctx;
    s_last_time_ms = saved_time_ms;
    return rs_v3_encode_sys_only(dst, dst_size);
}

static void rs_v3_handle_event(uint8_t panel_idx, const RsPanelV3Event *ev, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t result = (uint8_t)RS_PANEL_V3_ACK_OK;
    (void)now_ms;
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
        if (s_has_pending_reply != 0u && s_pending_reply_panel == panel_idx) {
            return;
        }
        if (s_has_cached_data_reply != 0u &&
            s_cached_data_reply_panel == panel_idx &&
            s_cached_data_reply_seq == ev->seq) {
            s_pending_reply = s_cached_data_reply;
            s_pending_reply_panel = panel_idx;
            s_has_pending_reply = 1u;
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
                                                 &s_pending_reply) != 0u) {
                s_pending_reply.seq = ev->seq;
                s_pending_reply.type = ev->type;
                s_pending_reply.result = (uint8_t)RS_PANEL_V3_ACK_OK;
                s_pending_reply_panel = panel_idx;
                s_has_pending_reply = 1u;
                s_cached_data_reply = s_pending_reply;
                s_cached_data_reply_seq = ev->seq;
                s_cached_data_reply_panel = panel_idx;
                s_has_cached_data_reply = 1u;
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
        if (RsPanelV3Proto_DispatchDataEvent(panel_idx, ev, &s_pending_reply) != 0u) {
            s_pending_reply.seq = ev->seq;
            s_pending_reply.type = ev->type;
            s_pending_reply.result = (uint8_t)RS_PANEL_V3_ACK_OK;
            s_pending_reply_panel = panel_idx;
            s_has_pending_reply = 1u;
            s_cached_data_reply = s_pending_reply;
            s_cached_data_reply_seq = ev->seq;
            s_cached_data_reply_panel = panel_idx;
            s_has_cached_data_reply = 1u;
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

    /* Пока стрим уже идёт — не начинать заново.
     * Полный каталог — NEED_CATALOG / devices_crc (пауза 2 с).
     * faults_crc — только досылка SET, без CLEAR_ALL (иначе список скачет). */
    if (ctx->stream_active == 0u) {
        if ((rsp->flags & RS_PANEL_V3_RSP_FLAG_NEED_CATALOG) != 0u ||
            rsp->devices_crc != ctx->expected_devices_crc) {
            if (ctx->last_full_resync_ms == 0u ||
                (now_ms - ctx->last_full_resync_ms) >= 2000u) {
                ctx->last_full_resync_ms = now_ms;
                rs_v3_panel_begin_stream(panel_idx);
            }
        } else if (rsp->faults_crc != ctx->expected_faults_crc) {
            ctx->stream_active = 1u;
            ctx->stream_phase = (s_fault_item_count > 0u) ? RS_V3_STREAM_FAULT_SET
                                                          : RS_V3_STREAM_FAULT_CLEAR;
            ctx->stream_idx = 0u;
            ctx->fault_piggy_idx = 0u;
        }
    }

    if (rsp->event.seq != 0u) {
        rs_v3_handle_event(panel_idx, &rsp->event, now_ms);
    }
}
