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
    /* Ordinary EVT ACK / dedupe */
    uint8_t pending_ack_seq;
    uint8_t pending_ack_result;
    uint8_t has_pending_ack;
    uint8_t last_event_seq;
    /* Critical EVT ACK / dedupe (START_SP/STOP/START_ALL/FIRE_RESET) */
    uint8_t pending_ack_crit_seq;
    uint8_t pending_ack_crit_result;
    uint8_t has_pending_ack_crit;
    uint8_t last_event_seq_crit;
    uint16_t expected_zones_crc;
    uint16_t expected_devices_crc;
    uint16_t expected_faults_crc;
    uint8_t stream_active;
    RsV3StreamPhase stream_phase;
    uint16_t stream_idx;
    uint16_t fault_piggy_idx; /* SET неисправностей во время стрима зон/устройств */
    uint32_t last_catalog_resync_ms; /* анти-флап catalog CRC rewrite */
    uint32_t catalog_tx_done_ms; /* когда ZONE/DEV stream закончил TX; 0 = нет */
    uint8_t stream_need_devices; /* 1 = после ZONE продолжить DEV */
    uint8_t stream_need_zones; /* 1 = ZONE ещё не дослан (resume после FAULT) */
    uint8_t catalog_crc_ok_latched; /* 1 = уже учли skip при совпадении CRC */
    uint8_t catalog_after_faults; /* 1 = после FAULT_SET продолжить ZONE/DEV */
    uint8_t fault_after_catalog; /* 1 = FAULT отложен, пока идёт ZONE/DEV */
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
/* Мин. пауза между begin_catalog (CLEAR). 5с мало, если FAULT прерывает DEV. */
#define RS_V3_CATALOG_RESYNC_MIN_MS 10000u
/* После последнего DEV/ZONE TLV не слать CLEAR: RSP отстаёт на 1 POLL, CRC «догоняет». */
#define RS_V3_CATALOG_TX_GRACE_MS 5000u
/* EVT из UART IRQ → main: Fire_Transition ~1.2 КБ стека; при пожаре main уже
 * в Fire_Transition — вложенный вызов из IRQ ломал START_SP (BP не срабатывал). */
#define RS_V3_DEFER_EVT_Q 16u

extern uint8_t RsPanelV3Proto_DispatchDataEvent(uint8_t panel_idx,
                                                const RsPanelV3Event *ev,
                                                RsPanelV3EventReply *reply);

typedef struct {
    uint8_t panel_idx;
    uint8_t is_crit;
    RsPanelV3Event ev;
} RsV3DeferredEvt;

static RsV3DeferredEvt s_defer_evt[RS_V3_DEFER_EVT_Q];
static volatile uint8_t s_defer_evt_n;
static uint8_t s_defer_queued_seq_ord[RS_PANEL_MAX_PANELS];
static uint8_t s_defer_queued_seq_crit[RS_PANEL_MAX_PANELS];

static void rs_v3_handle_event(uint8_t panel_idx, const RsPanelV3Event *ev,
                               uint32_t now_ms, uint8_t is_crit);

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

/* Снять оба pending ACK в POLL (ordinary → ack, critical → ack_crit). */
static void rs_v3_poll_take_acks(RsPanelV3Poll *out, RsPanelV3PanelCtx *ctx)
{
    if (out == 0 || ctx == 0) {
        return;
    }
    if (ctx->has_pending_ack != 0u) {
        out->ack.seq = ctx->pending_ack_seq;
        out->ack.result = ctx->pending_ack_result;
        ctx->has_pending_ack = 0u;
    }
    if (ctx->has_pending_ack_crit != 0u) {
        out->ack_crit.seq = ctx->pending_ack_crit_seq;
        out->ack_crit.result = ctx->pending_ack_crit_result;
        ctx->has_pending_ack_crit = 0u;
    }
}

static uint8_t rs_v3_ctx_has_any_pending_ack(const RsPanelV3PanelCtx *ctx)
{
    if (ctx == 0) {
        return 0u;
    }
    return (ctx->has_pending_ack != 0u || ctx->has_pending_ack_crit != 0u) ? 1u : 0u;
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
    ctx->stream_need_zones = (need_zones != 0u) ? 1u : 0u;
    ctx->catalog_crc_ok_latched = 0u;
    ctx->catalog_after_faults = 0u;
    ctx->catalog_tx_done_ms = 0u;
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
    memset(s_defer_evt, 0, sizeof(s_defer_evt));
    memset(s_defer_queued_seq_ord, 0, sizeof(s_defer_queued_seq_ord));
    memset(s_defer_queued_seq_crit, 0, sizeof(s_defer_queued_seq_crit));
    s_defer_evt_n = 0u;
    s_fault_item_count = 0u;
    s_fault_snapshot_dirty = 1u;
    s_boot_valid = 0u;
    //rs_v3_refresh_catalog_crcs(); - не тсмысла, т.к панели ещё не запущены
    //rs_v3_update_expected_fault_crc();
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
    /* SYS_READY позже process-паузы (~20 с): сначала Warning/Commit и поток
     * ZONE/MCU/FAULT (панель ещё «ПРОВЕРКА»), затем READY.
     * Каталог/FAULT_EVT в BuildPoll не зависят от этого флага. */
    if (Warning_IsSysReadyDelayActive() != 0u) {
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

/* FAULT_CLEAR/SET. ZONE/DEV не прерываем — иначе devices_crc никогда не
 * догоняет expected (CLEAR→ADD→FAULT→CLEAR…). FAULT после конца каталога. */
static void rs_v3_panel_schedule_fault_resync(uint8_t panel_idx, uint8_t need_clear)
{
    RsPanelV3PanelCtx *ctx;
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    if (ctx->stream_active != 0u && rs_v3_phase_is_catalog(ctx->stream_phase) != 0u) {
        ctx->fault_after_catalog = 1u;
        (void)need_clear;
        return;
    }
    /* Уже в FAULT_*: не сбрасывать idx на каждый Commit (10 мс) — иначе
     * вечный CLEAR и стрим SET не заканчивается. */
    if (ctx->stream_active != 0u &&
        (ctx->stream_phase == RS_V3_STREAM_FAULT_CLEAR ||
         ctx->stream_phase == RS_V3_STREAM_FAULT_SET)) {
        if (need_clear != 0u && ctx->stream_phase == RS_V3_STREAM_FAULT_SET) {
            ctx->stream_phase = RS_V3_STREAM_FAULT_CLEAR;
            ctx->stream_idx = 0u;
            ctx->fault_piggy_idx = 0u;
        }
        return;
    }
    ctx->stream_active = 1u;
    ctx->stream_phase = (need_clear != 0u) ? RS_V3_STREAM_FAULT_CLEAR
                                            : RS_V3_STREAM_FAULT_SET;
    ctx->stream_idx = 0u;
    ctx->fault_piggy_idx = 0u;
    ctx->catalog_after_faults = 0u;
}

/* Конец ZONE/DEV: stamp TX done; если ждали FAULT — сразу FAULT_CLEAR. */
static void rs_v3_catalog_stream_finished(RsPanelV3PanelCtx *ctx)
{
    if (ctx == 0) {
        return;
    }
    ctx->stream_idx = 0u;
    ctx->stream_need_devices = 0u;
    ctx->stream_need_zones = 0u;
    ctx->last_time_ms = 0u;
    ctx->catalog_tx_done_ms = 1u;
    if (ctx->fault_after_catalog != 0u) {
        ctx->fault_after_catalog = 0u;
        ctx->stream_active = 1u;
        ctx->stream_phase = RS_V3_STREAM_FAULT_CLEAR;
        ctx->fault_piggy_idx = 0u;
        return;
    }
    ctx->stream_active = 0u;
    ctx->stream_phase = RS_V3_STREAM_IDLE;
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

/* TIME в POLL: сразу (last_time_ms==0) или по периоду.
 * НЕ на каждый POLL во время stream: EncodePollToBuf при нехватке места
 * вытеснял ZONE/DEV/FAULT ради TIME → стрим голодал, шина дёргалась. */
static void rs_v3_maybe_attach_time(RsPanelV3Poll *out, RsPanelV3PanelCtx *ctx,
                                   uint32_t now_ms)
{
    if (out == 0 || ctx == 0) {
        return;
    }
    if (ctx->last_time_ms == 0u ||
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
        ctx->stream_need_zones = 0u;
        if (ctx->stream_need_devices == 0u) {
            rs_v3_catalog_stream_finished(ctx);
            if (ctx->stream_active != 0u) {
                return rs_v3_emit_stream_op(out, ctx);
            }
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
        rs_v3_catalog_stream_finished(ctx);
        if (ctx->stream_active != 0u) {
            return rs_v3_emit_stream_op(out, ctx);
        }
        return 0u;
    case RS_V3_STREAM_FAULT_CLEAR:
        out->has_fault_evt = 1u;
        out->fault_evt.op = (uint8_t)RS_PANEL_V3_FAULT_EVT_CLEAR_ALL;
        ctx->stream_phase = RS_V3_STREAM_FAULT_SET;
        ctx->stream_idx = 0u;
        return 1u;
    case RS_V3_STREAM_FAULT_SET:
        if (ctx->stream_idx < s_fault_item_count) {
            out->has_fault_evt = 1u;
            out->fault_evt = s_fault_items[ctx->stream_idx];
            out->fault_evt.op = (uint8_t)RS_PANEL_V3_FAULT_EVT_SET;
            ctx->stream_idx++;
            return 1u;
        }
        if (ctx->catalog_after_faults != 0u) {
            ctx->catalog_after_faults = 0u;
            if (ctx->stream_need_zones != 0u) {
                ctx->stream_phase = RS_V3_STREAM_ZONE_CLEAR;
            } else if (ctx->stream_need_devices != 0u) {
                ctx->stream_phase = RS_V3_STREAM_DEV_CLEAR;
            } else {
                ctx->stream_active = 0u;
                ctx->stream_phase = RS_V3_STREAM_IDLE;
                return 0u;
            }
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

/* Последний УСПЕШНО УШЕДШИЙ на провод снимок ZONES (commit только после Encode≠0).
 * Раньше писали сюда в put_* до TX: Encode не влез → SysOnly → дельта
 * COUNTDOWN→EXTINGUISHING потеряна навсегда → панель «ДО ПУСКА» после
 * внешней кнопки / ПУСК ОБЩИЙ. */
static RsPanelV3Zones s_last_tx_zones[RS_PANEL_MAX_PANELS];
static uint8_t s_last_tx_zones_valid[RS_PANEL_MAX_PANELS];

static void rs_v3_commit_fire_zones_tx(uint8_t panel_idx, const RsPanelV3Zones *z)
{
    if (panel_idx >= RS_PANEL_MAX_PANELS || z == 0) {
        return;
    }
    s_last_tx_zones[panel_idx] = *z;
    s_last_tx_zones_valid[panel_idx] = 1u;
}

void RsPanelV3Master_InvalidateFireZonesTx(void)
{
    uint8_t i;
    for (i = 0u; i < RS_PANEL_MAX_PANELS; i++) {
        s_last_tx_zones_valid[i] = 0u;
    }
}

/* Encode + commit ZONES только если кадр реально собрался. */
static uint16_t rs_v3_encode_poll_commit_zones(uint8_t *dst, uint16_t dst_size,
                                              const RsPanelV3Poll *poll,
                                              uint8_t panel_idx)
{
    uint16_t len;
    if (poll == 0) {
        return 0u;
    }
    len = RsPanelV3_EncodePoll(dst, dst_size, poll);
    if (len != 0u && poll->has_zones != 0u) {
        rs_v3_commit_fire_zones_tx(panel_idx, &poll->zones);
    }
    return len;
}

/* ZONES при пожаре — только если снимок изменился (status/remaining/…).
 * Иначе POLL без TLV зон; панель держит кэш. UI/таймер обновятся при дельте.
 * last_tx НЕ трогаем — иначе потеря дельты при неудачном Encode. */
static uint8_t rs_v3_put_fire_zones_if_changed(RsPanelV3Poll *out,
                                               uint8_t panel_idx,
                                               uint32_t now_ms)
{
    RsPanelV3Zones z;
    uint8_t n;
    if (out == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return 0u;
    }
    if (Fire_IsActive() == 0u) {
        s_last_tx_zones_valid[panel_idx] = 0u;
        return 0u;
    }
    Fire_FillV3Zones(&z, now_ms);
    n = z.count;
    if (n > RS_PANEL_V3_MAX_ZONES) {
        n = RS_PANEL_V3_MAX_ZONES;
    }
    if (s_last_tx_zones_valid[panel_idx] != 0u &&
        s_last_tx_zones[panel_idx].count == z.count &&
        (n == 0u ||
         memcmp(s_last_tx_zones[panel_idx].items, z.items,
                (size_t)n * sizeof(RsPanelV3ZoneItem)) == 0)) {
        return 0u;
    }
    out->zones = z;
    out->has_zones = (z.count > 0u) ? 1u : 0u;
    return 1u;
}

static void rs_v3_attach_fire_zones(RsPanelV3Poll *out, uint8_t panel_idx,
                                    uint32_t now_ms)
{
    (void)rs_v3_put_fire_zones_if_changed(out, panel_idx, now_ms);
}

void RsPanelV3Master_BuildPoll(RsPanelV3Poll *out, uint8_t panel_idx, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t need_reply;
    uint8_t fire_active;
    if (out == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    need_reply = (ctx->has_pending_reply != 0u) ? 1u : 0u;
    fire_active = (Fire_IsActive() != 0u) ? 1u : 0u;

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
    {
        const uint8_t addr = RsPanelMaster_GetPanelAddr(panel_idx);
        if (Beeper_IsPanelSoundOn(addr) != 0u) {
            out->sys.flags |= RS_PANEL_V3_SYS_SOUND_ON;
        }
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
     * Журнал/данные: пока reply pending — обычно ACK+reply.
     * FAULT_* важнее prefetch (JOURNAL_GET_N) — иначе неисправности не доходят.
     * ZONE/DEV: панель теперь применяет stream в IRQ; на мастере не держим
     * journal дольше yield только ради DEV (раньше затопляло шину/очередь).
     * Не дольше RS_V3_REPLY_YIELD_MS.
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
            rs_v3_poll_take_acks(out, ctx);
            (void)rs_v3_emit_stream_op(out, ctx);
            rs_v3_maybe_attach_time(out, ctx, now_ms);
            rs_v3_attach_fire_zones(out, panel_idx, now_ms);
            (void)s_fault_snapshot_dirty;
            return;
        }
        /* yield → отдать reply; FAULT со следующего POLL. */
    }

    /* При пожаре: ZONES (+ACK) важнее journal reply — иначе UI/звук ждут
     * пока prefetch журнала занимает POLL (задержка растёт от попытки к попытке).
     * Но не дольше RS_V3_REPLY_YIELD_MS: иначе reply latched на весь пожар,
     * каждый POLL уходит в этот early-return (лишняя нагрузка на кадр). */
    if (fire_active != 0u && need_reply != 0u) {
        uint32_t reply_age = 0u;
        if (ctx->pending_reply_since_ms != 0u) {
            reply_age = now_ms - ctx->pending_reply_since_ms;
        }
        if (reply_age < RS_V3_REPLY_YIELD_MS) {
            out->has_config = 0u;
            memset(&out->config, 0, sizeof(out->config));
            rs_v3_poll_take_acks(out, ctx);
            rs_v3_maybe_attach_time(out, ctx, now_ms);
            rs_v3_attach_fire_zones(out, panel_idx, now_ms);
            /* reply остаётся latched — следующим POLL без starvation ZONES. */
            (void)s_fault_snapshot_dirty;
            return;
        }
        /* yield → отдать reply; ZONES снова со следующего POLL. */
    }

    /* ACK и reply не в одном кадре: вместе часто > wire (251) → ACK теряется,
     * слот EVT на панели занят, fire-ctrl ждёт с «рандомной» задержкой.
     * Сначала только ACK (оба lane); reply — следующим POLL. */
    if (need_reply != 0u && rs_v3_ctx_has_any_pending_ack(ctx) != 0u) {
        out->has_config = 0u;
        memset(&out->config, 0, sizeof(out->config));
        rs_v3_poll_take_acks(out, ctx);
        rs_v3_maybe_attach_time(out, ctx, now_ms);
        rs_v3_attach_fire_zones(out, panel_idx, now_ms);
        (void)s_fault_snapshot_dirty;
        return;
    }

    if (need_reply != 0u) {
        out->has_config = 0u;
        memset(&out->config, 0, sizeof(out->config));
        rs_v3_poll_take_acks(out, ctx);
        out->has_event_reply = 1u;
        out->event_reply = ctx->pending_reply;
        ctx->has_pending_reply = 0u;
        ctx->pending_reply_since_ms = 0u;
        memset(&ctx->pending_reply, 0, sizeof(ctx->pending_reply));
        rs_v3_maybe_attach_time(out, ctx, now_ms);
        rs_v3_attach_fire_zones(out, panel_idx, now_ms);
        (void)s_fault_snapshot_dirty;
        return;
    }

    rs_v3_poll_take_acks(out, ctx);

    /* При пожаре не гоняем ZONE/DEV — Encode срезает ZONES ради каталога.
     * FAULT_CLEAR/SET оставляем: иначе faults_crc=FFFF, UI «НЕТ СВЯЗИ»/WARN
     * не обновляется до конца пожара (clear WARN зависит от FAULT_EVT). */
    {
        const uint8_t cat_before =
            (ctx->stream_active != 0u &&
             rs_v3_phase_is_catalog(ctx->stream_phase) != 0u)
                ? 1u
                : 0u;
        if (fire_active != 0u && cat_before != 0u &&
            ctx->fault_after_catalog != 0u) {
            /* Каталог на пожаре не TX — не держать FAULT в очереди. */
            ctx->catalog_after_faults = 1u;
            if (ctx->stream_phase == RS_V3_STREAM_ZONE_CLEAR ||
                ctx->stream_phase == RS_V3_STREAM_ZONE_ADD) {
                ctx->stream_need_zones = 1u;
            }
            if (ctx->stream_phase == RS_V3_STREAM_DEV_CLEAR ||
                ctx->stream_phase == RS_V3_STREAM_DEV_ADD) {
                ctx->stream_need_devices = 1u;
            }
            ctx->fault_after_catalog = 0u;
            ctx->stream_phase = RS_V3_STREAM_FAULT_CLEAR;
            ctx->stream_idx = 0u;
            ctx->fault_piggy_idx = 0u;
        }
        if (fire_active == 0u) {
            (void)rs_v3_emit_stream_op(out, ctx);
        } else if (ctx->stream_active != 0u &&
                   (ctx->stream_phase == RS_V3_STREAM_FAULT_CLEAR ||
                    ctx->stream_phase == RS_V3_STREAM_FAULT_SET)) {
            (void)rs_v3_emit_stream_op(out, ctx);
        }
        if (cat_before != 0u && ctx->stream_active == 0u) {
            ctx->catalog_tx_done_ms = (now_ms != 0u) ? now_ms : 1u;
        } else if (ctx->catalog_tx_done_ms == 1u) {
            ctx->catalog_tx_done_ms = (now_ms != 0u) ? now_ms : 1u;
        }
    }

    /* TIME — с первых POLL (last_time_ms==0) и далее по периоду, в т.ч. во
     * время каталога. EncodePollToBuf при нехватке места приоритерует TIME над
     * стримом (не срезает TIME в пользу zone/MCU TLV).
     * ZONES fire — только при изменении снимка. TIME — per-panel. */
    {
        rs_v3_maybe_attach_time(out, ctx, now_ms);

        if (fire_active != 0u) {
            if (out->has_zones == 0u) {
                (void)rs_v3_put_fire_zones_if_changed(out, panel_idx, now_ms);
            }
        } else if (out->has_zone_names == 0u && out->has_devices == 0u &&
                   out->has_fault_evt == 0u) {
            Fire_FillV3Zones(&out->zones, now_ms);
            out->has_zones = (out->zones.count > 0u) ? 1u : 0u;
        }
    }

    (void)s_fault_snapshot_dirty;
}

/* Минимальный POLL без RsPanelV3Poll на стеке повторно — только SYS flags. */
uint16_t RsPanelV3Master_EncodeSysOnlyPoll(uint8_t *dst, uint16_t dst_size,
                                           uint8_t panel_idx)
{
    uint16_t flags = 0u;
    uint8_t sound_on;
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
    if (panel_idx < RS_PANEL_MAX_PANELS) {
        sound_on = Beeper_IsPanelSoundOn(RsPanelMaster_GetPanelAddr(panel_idx));
    } else {
        sound_on = Beeper_IsSoundEnabled();
    }
    if (sound_on != 0u) {
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

/* SYS+ACK(+ACK_CRIT) без прочих TLV — последний шанс доставить ACK в этом POLL. */
static uint16_t rs_v3_encode_sys_ack(uint8_t *dst, uint16_t dst_size,
                                     const RsPanelV3Sys *sys,
                                     const RsPanelV3Ack *ack,
                                     const RsPanelV3Ack *ack_crit)
{
    RsPanelV3Poll fpoll;
    uint8_t any = 0u;
    if (dst == 0 || sys == 0) {
        return 0u;
    }
    memset(&fpoll, 0, sizeof(fpoll));
    fpoll.sys = *sys;
    if (ack != 0 && ack->seq != 0u &&
        ack->result != (uint8_t)RS_PANEL_V3_ACK_NONE) {
        fpoll.ack = *ack;
        any = 1u;
    }
    if (ack_crit != 0 && ack_crit->seq != 0u &&
        ack_crit->result != (uint8_t)RS_PANEL_V3_ACK_NONE) {
        fpoll.ack_crit = *ack_crit;
        any = 1u;
    }
    if (any == 0u) {
        return 0u;
    }
    return RsPanelV3_EncodePoll(dst, dst_size, &fpoll);
}

/* SYS(+ACK)+ZONES — минимальный fire-POLL, если полный кадр не влез. */
static uint16_t rs_v3_encode_sys_fire_zones(uint8_t *dst, uint16_t dst_size,
                                            const RsPanelV3Sys *sys,
                                            const RsPanelV3Ack *ack,
                                            const RsPanelV3Ack *ack_crit,
                                            uint8_t panel_idx,
                                            uint32_t now_ms)
{
    RsPanelV3Poll fpoll;
    if (dst == 0 || sys == 0 || Fire_IsActive() == 0u) {
        return 0u;
    }
    memset(&fpoll, 0, sizeof(fpoll));
    fpoll.sys = *sys;
    if (ack != 0 && ack->seq != 0u &&
        ack->result != (uint8_t)RS_PANEL_V3_ACK_NONE) {
        fpoll.ack = *ack;
    }
    if (ack_crit != 0 && ack_crit->seq != 0u &&
        ack_crit->result != (uint8_t)RS_PANEL_V3_ACK_NONE) {
        fpoll.ack_crit = *ack_crit;
    }
    if (rs_v3_put_fire_zones_if_changed(&fpoll, panel_idx, now_ms) == 0u) {
        return 0u;
    }
    return rs_v3_encode_poll_commit_zones(dst, dst_size, &fpoll, panel_idx);
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
    RsPanelV3Ack poll_ack;
    RsPanelV3Ack poll_ack_crit;
    uint8_t have_any_ack;

    if (dst == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return 0u;
    }
    ctx = &s_ctx[panel_idx];
    saved_ctx = *ctx;

    RsPanelV3Master_BuildPoll(&poll, panel_idx, now_ms);
    want_time = poll.has_time;
    poll_ack = poll.ack;
    poll_ack_crit = poll.ack_crit;
    have_any_ack = ((poll_ack.seq != 0u &&
                     poll_ack.result != (uint8_t)RS_PANEL_V3_ACK_NONE) ||
                    (poll_ack_crit.seq != 0u &&
                     poll_ack_crit.result != (uint8_t)RS_PANEL_V3_ACK_NONE))
                       ? 1u
                       : 0u;
    len = rs_v3_encode_poll_commit_zones(dst, dst_size, &poll, panel_idx);
    if (len != 0u) {
        if (want_time != 0u && panel_idx < 8u) {
            g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
        }
        return len;
    }

    /* Reply не влез — отложить reply, но ACK (+ZONES при пожаре) отдать. */
    if (poll.has_event_reply != 0u) {
        RsPanelV3Poll fpoll;
        *ctx = saved_ctx;
        ctx->pending_reply = poll.event_reply;
        rs_v3_latch_pending_reply(ctx, now_ms);

        memset(&fpoll, 0, sizeof(fpoll));
        fpoll.sys = poll.sys;
        fpoll.ack = poll_ack;
        fpoll.ack_crit = poll_ack_crit;
        ctx->has_pending_ack = 0u;
        ctx->has_pending_ack_crit = 0u;
        if (want_time != 0u) {
            fpoll.has_time = poll.has_time;
            fpoll.time = poll.time;
        }
        if (Fire_IsActive() != 0u) {
            (void)rs_v3_put_fire_zones_if_changed(&fpoll, panel_idx, now_ms);
        }
        len = rs_v3_encode_poll_commit_zones(dst, dst_size, &fpoll, panel_idx);
        if (len != 0u) {
            if (want_time != 0u && panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
        if (Fire_IsActive() != 0u) {
            len = rs_v3_encode_sys_fire_zones(dst, dst_size, &poll.sys,
                                              &poll_ack, &poll_ack_crit,
                                              panel_idx, now_ms);
            if (len != 0u) {
                return len;
            }
        }
        if (have_any_ack != 0u) {
            len = rs_v3_encode_sys_ack(dst, dst_size, &poll.sys, &poll_ack,
                                       &poll_ack_crit);
            if (len != 0u) {
                return len;
            }
            *ctx = saved_ctx;
            ctx->pending_reply = poll.event_reply;
            rs_v3_latch_pending_reply(ctx, now_ms);
        }
        return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size, panel_idx);
    }

    has_stream_tlv = (poll.has_zone_names != 0u || poll.has_devices != 0u ||
                      poll.has_fault_evt != 0u) ? 1u : 0u;

    /* 1) CONFIG всегда можно срезать. ZONES при пожаре — нет (ППКУ1: имена зон). */
    if (poll.has_config != 0u) {
        poll.has_config = 0u;
        memset(&poll.config, 0, sizeof(poll.config));
        len = rs_v3_encode_poll_commit_zones(dst, dst_size, &poll, panel_idx);
        if (len != 0u) {
            if (want_time != 0u && panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
    }
    if (poll.has_zones != 0u && Fire_IsActive() == 0u) {
        poll.has_zones = 0u;
        memset(&poll.zones, 0, sizeof(poll.zones));
        len = rs_v3_encode_poll_commit_zones(dst, dst_size, &poll, panel_idx);
        if (len != 0u) {
            if (want_time != 0u && panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
    }

    /* 2) TIME vs stream: FAULT_CLEAR/SET не откатываем ради TIME — иначе
     * @250 кбод + mismatch CRC мастер залипает на CLEAR, WARN/журнал голодают.
     * ZONE/DEV по-прежнему уступают TIME (часы на NORMA). */
    if (want_time != 0u && has_stream_tlv != 0u && poll.has_fault_evt == 0u) {
        ctx->stream_active = saved_ctx.stream_active;
        ctx->stream_phase = saved_ctx.stream_phase;
        ctx->stream_idx = saved_ctx.stream_idx;
        ctx->fault_piggy_idx = saved_ctx.fault_piggy_idx;
        ctx->stream_need_devices = saved_ctx.stream_need_devices;
        ctx->stream_need_zones = saved_ctx.stream_need_zones;
        ctx->catalog_after_faults = saved_ctx.catalog_after_faults;
        ctx->fault_after_catalog = saved_ctx.fault_after_catalog;
        ctx->catalog_tx_done_ms = saved_ctx.catalog_tx_done_ms;

        poll.has_zone_names = 0u;
        poll.has_devices = 0u;
        poll.has_fault_evt = 0u;
        poll.has_config = 0u;
        memset(&poll.zone_names, 0, sizeof(poll.zone_names));
        memset(&poll.device, 0, sizeof(poll.device));
        memset(&poll.fault_evt, 0, sizeof(poll.fault_evt));
        memset(&poll.config, 0, sizeof(poll.config));
        if (Fire_IsActive() == 0u) {
            poll.has_zones = 0u;
            memset(&poll.zones, 0, sizeof(poll.zones));
        } else if (poll.has_zones == 0u) {
            (void)rs_v3_put_fire_zones_if_changed(&poll, panel_idx, now_ms);
        }

        len = rs_v3_encode_poll_commit_zones(dst, dst_size, &poll, panel_idx);
        if (len != 0u) {
            if (panel_idx < 8u) {
                g_rs_master_dbg.time_tlv_tx_by_panel[panel_idx]++;
            }
            return len;
        }
        /* Не влезло: ACK важнее TIME/стрима. */
        if (have_any_ack != 0u) {
            *ctx = saved_ctx;
            ctx->has_pending_ack = 0u;
            ctx->has_pending_ack_crit = 0u;
            len = rs_v3_encode_sys_ack(dst, dst_size, &poll.sys, &poll_ack,
                                       &poll_ack_crit);
            if (len != 0u) {
                return len;
            }
            *ctx = saved_ctx;
        } else {
            *ctx = saved_ctx;
        }
        return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size, panel_idx);
    }

    /* 3) Срезать TIME, оставить стрим (FAULT/каталог resync). */
    if (want_time != 0u) {
        ctx->last_time_ms = saved_ctx.last_time_ms;
        poll.has_time = 0u;
        poll.sys.flags = (uint16_t)(poll.sys.flags &
                                   (uint16_t)(~RS_PANEL_V3_SYS_TIME_VALID));
        len = rs_v3_encode_poll_commit_zones(dst, dst_size, &poll, panel_idx);
        if (len != 0u) {
            return len;
        }
    }

    /* При пожаре: SYS+ACK(+ZONES если снимок изменился). */
    if (Fire_IsActive() != 0u) {
        ctx->has_pending_ack = 0u;
        ctx->has_pending_ack_crit = 0u;
        len = rs_v3_encode_sys_fire_zones(dst, dst_size, &poll.sys,
                                          &poll_ack, &poll_ack_crit,
                                          panel_idx, now_ms);
        if (len != 0u) {
            return len;
        }
        *ctx = saved_ctx;
    }

    /* Совсем не влезло — ACK всё равно, иначе слот EVT на панели залипает. */
    *ctx = saved_ctx;
    if (have_any_ack != 0u) {
        ctx->has_pending_ack = 0u;
        ctx->has_pending_ack_crit = 0u;
        len = rs_v3_encode_sys_ack(dst, dst_size, &poll.sys, &poll_ack,
                                   &poll_ack_crit);
        if (len != 0u) {
            return len;
        }
        *ctx = saved_ctx;
    }
    return RsPanelV3Master_EncodeSysOnlyPoll(dst, dst_size, panel_idx);
}

/* IRQ: только очередь / повтор ACK. Fire_Transition и journal SPI — в ProcessDeferred. */
static void rs_v3_defer_event(uint8_t panel_idx, const RsPanelV3Event *ev,
                              uint8_t is_crit)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t last_seq;
    uint8_t *queued_seq;
    uint32_t primask;

    if (ev == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    if (ev->seq == 0u || ev->type == (uint8_t)RS_PANEL_V3_EVT_NONE) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    last_seq = (is_crit != 0u) ? ctx->last_event_seq_crit : ctx->last_event_seq;
    queued_seq = (is_crit != 0u) ? &s_defer_queued_seq_crit[panel_idx]
                                 : &s_defer_queued_seq_ord[panel_idx];

    /* Уже обработан в main — сразу pending ACK для следующего POLL. */
    if (last_seq == ev->seq) {
        if (is_crit != 0u) {
            ctx->pending_ack_crit_seq = ev->seq;
            ctx->pending_ack_crit_result = (uint8_t)RS_PANEL_V3_ACK_OK;
            ctx->has_pending_ack_crit = 1u;
        } else {
            ctx->pending_ack_seq = ev->seq;
            ctx->pending_ack_result = (uint8_t)RS_PANEL_V3_ACK_OK;
            ctx->has_pending_ack = 1u;
        }
        return;
    }
    /* Уже в очереди. Если n==0 — флаг «осиротел» (HardFault/сброс n) → поставить снова. */
    if (*queued_seq == ev->seq && s_defer_evt_n != 0u) {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_defer_evt_n >= RS_V3_DEFER_EVT_Q) {
        if (primask == 0u) {
            __enable_irq();
        }
        return;
    }
    s_defer_evt[s_defer_evt_n].panel_idx = panel_idx;
    s_defer_evt[s_defer_evt_n].is_crit = is_crit;
    s_defer_evt[s_defer_evt_n].ev = *ev;
    s_defer_evt_n++;
    *queued_seq = ev->seq;
    if (primask == 0u) {
        __enable_irq();
    }
}

void RsPanelV3Master_ProcessDeferred(uint32_t now_ms)
{
    RsV3DeferredEvt local[RS_V3_DEFER_EVT_Q];
    uint8_t n;
    uint8_t i;
    uint32_t primask;

    primask = __get_PRIMASK();
    __disable_irq();
    n = s_defer_evt_n;
    if (n > RS_V3_DEFER_EVT_Q) {
        n = RS_V3_DEFER_EVT_Q;
    }
    for (i = 0u; i < n; i++) {
        local[i] = s_defer_evt[i];
    }
    s_defer_evt_n = 0u;
    if (primask == 0u) {
        __enable_irq();
    }

    for (i = 0u; i < n; i++) {
        const uint8_t p = local[i].panel_idx;
        const uint8_t is_crit = local[i].is_crit;
        rs_v3_handle_event(p, &local[i].ev, now_ms, is_crit);
        if (p < RS_PANEL_MAX_PANELS) {
            if (is_crit != 0u) {
                if (s_defer_queued_seq_crit[p] == local[i].ev.seq) {
                    s_defer_queued_seq_crit[p] = 0u;
                }
            } else if (s_defer_queued_seq_ord[p] == local[i].ev.seq) {
                s_defer_queued_seq_ord[p] = 0u;
            }
        }
    }
}

/* is_crit: 0 = ordinary lane (ack/last_event_seq), 1 = critical (ack_crit). */
static void rs_v3_handle_event(uint8_t panel_idx, const RsPanelV3Event *ev,
                               uint32_t now_ms, uint8_t is_crit)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t result = (uint8_t)RS_PANEL_V3_ACK_OK;
    uint8_t last_seq;
    if (ev == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    if (ev->seq == 0u || ev->type == (uint8_t)RS_PANEL_V3_EVT_NONE) {
        return;
    }
    last_seq = (is_crit != 0u) ? ctx->last_event_seq_crit : ctx->last_event_seq;
    if (last_seq == ev->seq) {
        /*
         * Панель ещё шлёт тот же seq (ACK/reply не дошли).
         * Не вызывать Dispatch снова (JOURNAL_GET_N = до 3× SPI) — отдать кэш.
         */
        if (is_crit != 0u) {
            ctx->pending_ack_crit_seq = ev->seq;
            ctx->pending_ack_crit_result = (uint8_t)RS_PANEL_V3_ACK_OK;
            ctx->has_pending_ack_crit = 1u;
            return;
        }
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
            const uint8_t addr = RsPanelMaster_GetPanelAddr(panel_idx);
            const uint8_t prev = Beeper_IsPanelSoundOn(addr);
            if (prev != on) {
                Beeper_SetPanelSound(addr, on);
                EventLog_LogSoundToggle(on, 0u); /* source: panel */
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

    if (is_crit != 0u) {
        ctx->pending_ack_crit_seq = ev->seq;
        ctx->pending_ack_crit_result = result;
        ctx->has_pending_ack_crit = 1u;
        ctx->last_event_seq_crit = ev->seq;
    } else {
        ctx->pending_ack_seq = ev->seq;
        ctx->pending_ack_result = result;
        ctx->has_pending_ack = 1u;
        ctx->last_event_seq = ev->seq;
    }
}

static volatile uint32_t rsp_cnt = 0;

void RsPanelV3Master_OnRsp(uint8_t panel_idx, const RsPanelV3Rsp *rsp, uint32_t now_ms)
{
    RsPanelV3PanelCtx *ctx;
    uint8_t had_crit = 0u;
    if (rsp == 0 || panel_idx >= RS_PANEL_MAX_PANELS) {
        return;
    }
    ctx = &s_ctx[panel_idx];
    rsp_cnt++;
    /*
     * 1) Сначала EVT (crit → ordinary). Каталог/FAULT CRC — после: иначе
     * begin_catalog на каждом RSP отвлекает от START_SP и раздувает IRQ.
     * Без пожара crit — сразу; при пожаре — очередь (Fire_Transition ~1.2 КБ).
     * В одном RSP: не больше одного «тяжёлого» handle — если был crit,
     * ordinary только в defer (journal SPI не в том же заходе, что Fire).
     */
    {
        const uint8_t defer_fire = (Fire_IsActive() != 0u) ? 1u : 0u;
        if (rsp->event_crit.seq != 0u) {
            had_crit = 1u;
            if (defer_fire != 0u) {
                rs_v3_defer_event(panel_idx, &rsp->event_crit, 1u);
            } else {
                rs_v3_handle_event(panel_idx, &rsp->event_crit, now_ms, 1u);
            }
        }
        if (rsp->event.seq != 0u) {
            if (rsp->event_crit.seq != 0u &&
                rsp->event.seq == rsp->event_crit.seq) {
                /* Зеркало того же fire-ctrl — уже обработан в crit. */
            } else if (rsp->event.type == (uint8_t)RS_PANEL_V3_EVT_START_SP ||
                       rsp->event.type == (uint8_t)RS_PANEL_V3_EVT_STOP_LAUNCH ||
                       rsp->event.type == (uint8_t)RS_PANEL_V3_EVT_START_ALL_COMMIT ||
                       rsp->event.type == (uint8_t)RS_PANEL_V3_EVT_FIRE_RESET) {
                if (had_crit != 0u || defer_fire != 0u) {
                    rs_v3_defer_event(panel_idx, &rsp->event, 1u);
                } else {
                    rs_v3_handle_event(panel_idx, &rsp->event, now_ms, 1u);
                }
            } else if (had_crit != 0u || defer_fire != 0u) {
                /* Crit уже занял этот заход / пожар — ordinary в очередь. */
                rs_v3_defer_event(panel_idx, &rsp->event, 0u);
            } else {
                rs_v3_handle_event(panel_idx, &rsp->event, now_ms, 0u);
            }
        }
    }

    /*
     * 2) Каталог/FAULT — после EVT.
     * FAULT во время ZONE/DEV не прерывает каталог (fault_after_catalog).
     * Пока FAULT_CLEAR/SET — не schedule снова (иначе idx→0 / FFFF).
     */
    if (rsp->faults_crc != ctx->expected_faults_crc) {
        const uint8_t fault_stream_busy =
            (ctx->stream_active != 0u &&
             (ctx->stream_phase == RS_V3_STREAM_FAULT_CLEAR ||
              ctx->stream_phase == RS_V3_STREAM_FAULT_SET))
                ? 1u
                : 0u;
        if (fault_stream_busy == 0u) {
            rs_v3_panel_schedule_fault_resync(panel_idx, 1u);
        }
    } else if (ctx->stream_active == 0u) {
        uint8_t need_zones;
        uint8_t need_devices;
        if ((rsp->flags & RS_PANEL_V3_RSP_FLAG_NEED_CATALOG) != 0u) {
            need_zones = 1u;
            need_devices = 1u;
        } else {
            need_zones = (rsp->zones_crc != ctx->expected_zones_crc) ? 1u : 0u;
            need_devices = (rsp->devices_crc != ctx->expected_devices_crc) ? 1u : 0u;
        }
        if (need_zones != 0u || need_devices != 0u) {
            ctx->catalog_crc_ok_latched = 0u;
            /* После полного TX каталога RSP с CRC отстаёт ≥1 POLL — не CLEAR сразу. */
            if (ctx->catalog_tx_done_ms != 0u &&
                (now_ms - ctx->catalog_tx_done_ms) < RS_V3_CATALOG_TX_GRACE_MS) {
                /* ждём */
            } else if (ctx->last_catalog_resync_ms == 0u ||
                       (now_ms - ctx->last_catalog_resync_ms) >=
                           RS_V3_CATALOG_RESYNC_MIN_MS) {
                ctx->last_catalog_resync_ms = now_ms;
                rs_v3_panel_begin_catalog(panel_idx, need_zones, need_devices);
            }
        } else if (ctx->catalog_crc_ok_latched == 0u) {
            ctx->catalog_crc_ok_latched = 1u;
            ctx->catalog_tx_done_ms = 0u;
            g_rs_master_dbg.catalog_skip_zones++;
            g_rs_master_dbg.catalog_skip_devices++;
        }
    }
}
