#include "rs_panel_proto.h"
#include "rs_panel_master_debug.h"
#include "rs_panel_v3_master.h"
#include "rs_panel_protocol_v3.h"

#include <stdio.h>
#include <string.h>

#include "device_config.h"
#include "config_zone_block.h"
#include "event_log.h"
#include "event_log_reader.h"
#include "event_logger.h"
#include "event_log_ui.h"
#include "log_stream.h"
#include "esp_manager.h"
#include "esp_protocol.h"
#include "can_bus.h"
#include "menu_ui.h" /* Esp32_IsEnabled */
#include "warning.h"
#include "fire.h"

extern PPKYCfg PPKYConfig;

static RsPanelMaster *g_active_master = 0;
static uint8_t s_v3_poll_seen[RS_PANEL_MAX_PANELS];
static uint16_t g_esp_uart_fwd_seq = 1u;
static uint32_t g_journal_total = 0u;
static uint32_t g_journal_selected = 0u;
/* v3 JOURNAL_GET cursor — независимо на каждую панель. */
static uint32_t s_v3_journal_selected[RS_PANEL_MAX_PANELS];
/* UI screen id — только dbg / logo deadline (навигацию ведёт панель v3). */
static uint16_t g_ui_current_screen_id = RS_PANEL_SCREEN_LOGO;
#define RS_PANEL_LOGO_MAIN_DELAY_MS 4000u
static uint32_t s_logo_main_nav_deadline_ms = 0u;
static uint8_t s_panel_ui_resync_pending = 0u;

static uint16_t g_menu_selected = 0u; /* dbg mirror */

#define RS_PANEL_CAPS_RETRY_MS 50u
/* Окно > max PanelCfg_DiscoverDelayMs (40..1040) + запас на TX (только коллизии). */
#define RS_PANEL_ADDR_DISCOVER_WINDOW_MS 1500u
#define RS_PANEL_ADDR_ASSIGN_GAP_MS 80u
#define RS_PANEL_ADDR_SETTLE_MS 500u
/* Не крутить DISCOVER по ACTIVITY-коллизии чаще: иначе POLL/WiFi ACTIVITY глушатся. */
#define RS_PANEL_ADDR_COLLISION_COOLDOWN_MS 8000u
/* Скан 1..MAX: шаг между probe. После первого ADD — периодический rescan
 * неизвестных (hot-plug); иначе done навсегда → новая панель «НЕТ СВЯЗИ». */
#define RS_PANEL_ADDR_SCAN_INTERVAL_MS 500u
#define RS_PANEL_ADDR_RESCAN_MS 5000u
/* ПО (bus_monitor) держит панель только по ESP_UART ACTIVITY (~60 с таймаут).
 * Панель больше не шлёт ACTIVITY сама — мастер запрашивает ACTIVITY_REQ (unicast)
 * и синтезирует presence в WiFi из слотов с свежим last_rx. */
#define RS_PANEL_WIFI_PRESENCE_PERIOD_MS 1500u
#define RS_PANEL_WIFI_PRESENCE_ALIVE_MS  5000u
/* Unicast ACTIVITY_REQ: 1 кадр/с RR по слотам (не голодать POLL). */
#define RS_PANEL_ACTIVITY_REQ_PERIOD_MS 1000u
/*
 * Half-duplex: после TX не слать новый кадр, пока не придёт любой DIR=1
 * или не истечёт окно. Без RecoverFromError/Abort в таймауте — только
 * следующий SendFrame AbortReceive перед своим TX.
 */
/* @250 кбод: символ ≈40 мкс, max wire ~260 B ≈11 мс; stagger addr×2 мс до 16 мс.
 * Quiet должен покрыть stagger+RSP (+запас). */
#define RS_PANEL_BUS_TURNAROUND_MS 50u
#define RS_PANEL_BUS_TURNAROUND_FIRE_MS 80u
#define RS_PANEL_BUS_TURNAROUND_SCAN_MS 120u
/*
 * Если у любого слота gap POLL ≥ этого — addr_scan/ACTIVITY/can_mirror откладываем.
 */
#define RS_PANEL_POLL_MAX_GAP_MS 150u
/* При 2+ панелях мин. период POLL на слот (wall). */
#define RS_PANEL_MULTI_POLL_MS 50u

static uint32_t s_last_caps_req_ms = 0u;
/* До этого tick не слать новый master TX (ждём RSP / turnaround). */
static uint32_t s_bus_quiet_until_ms = 0u;
static void rs_panel_master_arm_bus_quiet(uint32_t now_ms);
static void rs_panel_master_clear_bus_quiet_from_rx(void);
static uint8_t rs_panel_master_bus_is_quiet(RsPanelMaster *master, uint32_t now_ms);
static void rs_panel_master_dbg_sync_rx_decode(RsPanelMaster *master);
static uint8_t rs_panel_master_any_poll_due(RsPanelMaster *master, uint32_t now_ms);
static uint8_t rs_panel_master_poll_starving(RsPanelMaster *master, uint32_t now_ms);
/* CAPS retry per slot — глобальный s_last_caps_req_ms голодал 2-ю панель. */
static uint32_t s_caps_req_ms[RS_PANEL_MAX_PANELS];
/* ESP_UART inject тоже нельзя слать из UART2 RX IRQ (can_bus): тот же Abort+TX.
 * Кадры копятся здесь и уходят из Process10ms (не больше 1 TX за тик). */
#define RS_PANEL_INJECT_Q_DEPTH 8u
typedef struct {
    uint16_t len;
    uint8_t data[ESP_UART_BODY_MAX];
} RsPanelInjectQItem;
static RsPanelInjectQItem s_inject_q[RS_PANEL_INJECT_Q_DEPTH];
static volatile uint8_t s_inject_q_head = 0u;
static volatile uint8_t s_inject_q_tail = 0u;
static volatile uint8_t s_inject_q_count = 0u;
static uint32_t s_inject_pause_until_ms = 0u;
/* Скан RS-адресов 1..MAX: один проход (s_addr_scan_done=1 → стоп). */
static uint8_t s_addr_scan_next = 1u;
static uint32_t s_addr_scan_next_ms = 0u;
static uint8_t s_addr_scan_probe_addr = 0u;
static uint8_t s_addr_scan_done = 0u;
/* Запрос WiFi с RS (sniffer): не вызывать EspManager из UART RX IRQ. */
static volatile uint8_t s_wifi_enable_req = 0u;
static uint32_t s_wifi_presence_next_ms = 0u;
static uint8_t s_wifi_presence_seq = 1u;
/* ACTIVITY_REQ RR + срочный запрос UID после POLL-scan ADD без UID. */
static uint32_t s_activity_req_next_ms = 0u;
static uint8_t s_activity_req_rr_idx = 0u;
static uint8_t s_activity_req_urgent_idx = 0xFFu;

/* CAN→RS485 mirror (default OFF). Очередь полных BSU CAN (22 байта). */
#define RS_PANEL_CAN_MIRROR_Q_DEPTH 64u
static volatile uint8_t s_can_mirror_enable = 0u;
static uint8_t s_can_mirror_q[RS_PANEL_CAN_MIRROR_Q_DEPTH][BSU_PKT_CAN_SIZE];
static volatile uint8_t s_can_mirror_q_head = 0u;
static volatile uint8_t s_can_mirror_q_tail = 0u;
static volatile uint8_t s_can_mirror_q_count = 0u;

static uint8_t rs_panel_inject_cmd_allowed(uint8_t cmd)
{
    /* Команды прошивки/бутлоадера и PROFILE_SET с ПК. Иначе мусор ESP при WiFi
     * забивает очередь и Process10ms намертво крутит SendRaw. */
    return (cmd == RS_PANEL_CMD_ENTER_BOOTLOADER ||
            cmd == RS_PANEL_CMD_BOOT_RESET_MCU ||
            cmd == RS_PANEL_CMD_BOOT_SET_UPD_WORD ||
            cmd == RS_PANEL_CMD_BOOT_UPD_TRANSMIT ||
            cmd == RS_PANEL_CMD_BOOT_GET_VERSION ||
            cmd == RS_PANEL_CMD_PROFILE_SET) ? 1u : 0u;
}

/*
 * PPCU-side: передаём “нажатые” кнопки с панели в общий Button FSM.
 * В `button.c` эта функция объявлена как weak, поэтому сильная реализация здесь
 * автоматически перехватывает remote-кнопки.
 */
/* FORCE / STOP / FIRE — биты пусковых кнопок. */
#define RS_PANEL_LAUNCH_BTN_MASK \
    ((uint8_t)((1u << 4) | (1u << 5) | (1u << 6)))

uint8_t Button_FetchRemotePressedMask(uint8_t *mask_out)
{
    if (mask_out != 0u) {
        *mask_out = 0u;
    }
    if (g_active_master == 0) {
        return 1u;
    }

    uint8_t mask = 0u;
    for (uint8_t i = 0u; i < g_active_master->panel_count; i++) {
        PanelState *panel = &g_active_master->panels[i];
        if (panel->cfg.enabled == 0u) {
            continue;
        }
        if (PanelState_IsReady(panel) == 0u) {
            /* Не READY — не держим залипшие пусковые биты от прошлой сессии. */
            panel->remote_btn_mask =
                (uint8_t)(panel->remote_btn_mask & (uint8_t)~RS_PANEL_LAUNCH_BTN_MASK);
            continue;
        }
        mask |= panel->remote_btn_mask;
    }

    if (mask_out != 0u) {
        *mask_out = mask;
    }
    return 1u;
}

/* WARNING_TITLE_LEN задаётся в app.cpp; чтобы сигнатура совпадала с extern-weak коллбеком,
 * фиксируем то же значение и здесь. */
#ifndef WARNING_TITLE_LEN
#define WARNING_TITLE_LEN 24u
#endif

#define RS_PANEL_ACK_TIMEOUT_MS 120u
#define RS_PANEL_ACK_RETRIES    3u

/* WARN/FIRE не должны срывать меню. После логотипа панель сама уходит на MAIN;
 * повторный UI_NAV MAIN сдвигает виджеты, поэтому здесь только синхронизируем
 * локальный id. */
/* forward decl: используется внутри MENU_LIST */
static uint8_t rs_panel_master_is_mcu_type(uint8_t d_type)
{
    return (d_type == DEVICE_MCU_IGN_TYPE ||
            d_type == DEVICE_MCU_TC_TYPE ||
            d_type == DEVICE_MCU_K1 ||
            d_type == DEVICE_MCU_K2 ||
            d_type == DEVICE_MCU_K3 ||
            d_type == DEVICE_MCU_KR) ? 1u : 0u;
}

static uint8_t rs_panel_master_collect_mcu_slots(uint8_t *slots, uint8_t max_slots)
{
    uint8_t count = 0u;
    for (uint8_t i = 0u; i < MAX_MCU_IN_BUS && count < max_slots; i++) {
        const Device *dev = &PPKYConfig.CfgDevices[i].UId.devId;
        if (rs_panel_master_is_mcu_type(dev->d_type) != 0u) {
            slots[count++] = i;
        }
    }
    return count;
}

static uint16_t rs_put_u32le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
    return 4u;
}

/* OLED как ППКУ 1: одна запись = header + title + detail. Лимиты под кадр ≤251. */
#define RS_JOURNAL_HDR_MAX     31u
#define RS_JOURNAL_TITLE_MAX   23u
#define RS_JOURNAL_DETAIL_MAX  120u

static void rs_panel_master_format_journal_lines(const EventLogRecord_t *rec,
                                                 uint32_t rec_idx,
                                                 EventLogUiLines_t *out)
{
    if (out == 0) {
        return;
    }
    if (rec == 0) {
        EventLogUi_FormatEmpty(out);
        return;
    }
    EventLogUi_FormatRecord(rec, rec_idx + 1u, g_journal_total, out);
}

static uint16_t rs_journal_put_counted_str(uint8_t *dst,
                                           uint16_t pos,
                                           uint16_t dst_size,
                                           const char *src,
                                           uint8_t max_len)
{
    uint8_t n;
    uint8_t i;

    if (dst == 0 || (uint16_t)(pos + 1u) > dst_size) {
        return pos;
    }
    n = 0u;
    if (src != 0) {
        while (n < max_len && src[n] != '\0') {
            n++;
        }
    }
    if ((uint16_t)(pos + 1u + (uint16_t)n) > dst_size) {
        if (dst_size <= (uint16_t)(pos + 1u)) {
            return pos;
        }
        n = (uint8_t)(dst_size - pos - 1u);
    }
    dst[pos++] = n;
    for (i = 0u; i < n; i++) {
        dst[pos++] = (uint8_t)src[i];
    }
    return pos;
}

static uint8_t rs_panel_master_any_panel_ready(const RsPanelMaster *master)
{
    uint8_t i;

    if (master == 0) {
        return 0u;
    }
    for (i = 0u; i < master->panel_count; i++) {
        const PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled != 0u && PanelState_IsReady(panel) != 0u) {
            return 1u;
        }
    }
    return 0u;
}

/* Панель стала READY: TIME в v3 POLL + отложенный UI-resync флаг. */
static void rs_panel_master_on_panel_became_ready(RsPanelMaster *master)
{
    uint8_t pi;
    if (master == 0) {
        return;
    }
    if (g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO) {
        s_logo_main_nav_deadline_ms = HAL_GetTick() + RS_PANEL_LOGO_MAIN_DELAY_MS;
    } else {
        s_logo_main_nav_deadline_ms = 0u;
    }
    s_panel_ui_resync_pending = 1u;
    for (pi = 0u; pi < master->panel_count && pi < RS_PANEL_MAX_PANELS; pi++) {
        RsPanelV3Master_ForceTimeSyncPanel(pi);
    }
}


uint8_t App_OnWarningUiUpdate(uint8_t active,
                            uint8_t n_items,
                            char (*big_titles)[WARNING_TITLE_LEN],
                            char (*details)[ZONE_NAME_SIZE + 1])
{
    /* v3: неисправности через FAULT_EVT в POLL, не UI_DATA MAIN_WARN. */
    (void)active;
    (void)n_items;
    (void)big_titles;
    (void)details;
    return 1u;
}


static uint16_t rs_put_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)(value >> 8);
    return 2u;
}

static uint16_t rs_get_u16le(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

/* Если панель по ошибке прислала btn_event вместо ui_event — всё равно
 * обработать навигацию меню (только когда мастер точно в MENU_*). */
uint16_t RsPanel_EncodeCaps(uint8_t *dst, uint16_t dst_size, const RsPanelCaps *caps)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (dst == 0 || caps == 0 || dst_size < 13u) {
        return 0u;
    }

    pos += rs_put_u16le(&dst[pos], caps->fw_ver);
    pos += rs_put_u16le(&dst[pos], caps->hw_id);
    dst[pos++] = caps->ui_profile;
    dst[pos++] = caps->orientation;
    pos += rs_put_u16le(&dst[pos], caps->disp_w);
    pos += rs_put_u16le(&dst[pos], caps->disp_h);
    dst[pos++] = caps->journal_lines;
    dst[pos++] = caps->btn_count;
    for (i = 0u; i < caps->btn_count && i < RS_PANEL_MAX_CAPS_BUTTONS; i++) {
        if ((uint16_t)(pos + 1u) > dst_size) {
            return 0u;
        }
        dst[pos++] = caps->btn_list[i];
    }
    if ((uint16_t)(pos + 1u) > dst_size) {
        return 0u;
    }
    dst[pos++] = caps->led_count;
    for (i = 0u; i < caps->led_count && i < RS_PANEL_MAX_CAPS_LEDS; i++) {
        if ((uint16_t)(pos + 1u) > dst_size) {
            return 0u;
        }
        dst[pos++] = caps->led_list[i];
    }
    if ((uint16_t)(pos + 2u) > dst_size) {
        return 0u;
    }
    dst[pos++] = caps->flags;
    dst[pos++] = caps->status;
    return pos;
}

uint8_t RsPanel_DecodeCaps(const uint8_t *src, uint16_t src_len, RsPanelCaps *out_caps)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (src == 0 || out_caps == 0 || src_len < 13u) {
        return 0u;
    }
    memset(out_caps, 0, sizeof(*out_caps));
    out_caps->fw_ver = rs_get_u16le(&src[pos]); pos += 2u;
    out_caps->hw_id = rs_get_u16le(&src[pos]); pos += 2u;
    out_caps->ui_profile = src[pos++];
    out_caps->orientation = src[pos++];
    out_caps->disp_w = rs_get_u16le(&src[pos]); pos += 2u;
    out_caps->disp_h = rs_get_u16le(&src[pos]); pos += 2u;
    out_caps->journal_lines = src[pos++];
    out_caps->btn_count = src[pos++];
    if (out_caps->btn_count > RS_PANEL_MAX_CAPS_BUTTONS) {
        return 0u;
    }
    for (i = 0u; i < out_caps->btn_count; i++) {
        if ((uint16_t)(pos + 1u) > src_len) {
            return 0u;
        }
        out_caps->btn_list[i] = src[pos++];
    }
    if ((uint16_t)(pos + 1u) > src_len) {
        return 0u;
    }
    out_caps->led_count = src[pos++];
    if (out_caps->led_count > RS_PANEL_MAX_CAPS_LEDS) {
        return 0u;
    }
    for (i = 0u; i < out_caps->led_count; i++) {
        if ((uint16_t)(pos + 1u) > src_len) {
            return 0u;
        }
        out_caps->led_list[i] = src[pos++];
    }
    if ((uint16_t)(pos + 2u) > src_len) {
        return 0u;
    }
    out_caps->flags = src[pos++];
    out_caps->status = src[pos++];
    return 1u;
}

uint16_t RsPanel_EncodeActivity(uint8_t *dst, uint16_t dst_size, const RsPanelActivity *act)
{
    uint16_t pos = 0u;

    if (dst == 0 || act == 0 || dst_size < RS_PANEL_ACTIVITY_PAYLOAD_SIZE) {
        return 0u;
    }
    dst[pos++] = act->dev_type;
    pos = (uint16_t)(pos + rs_put_u16le(&dst[pos], act->fw_ver));
    pos = (uint16_t)(pos + rs_put_u16le(&dst[pos], act->hw_id));
    dst[pos++] = act->status;
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], act->uptime_sec));
    if (act->uid_valid != 0u && dst_size >= RS_PANEL_ACTIVITY_PAYLOAD_SIZE_UID) {
        pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], act->uid0));
        pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], act->uid1));
        pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], act->uid2));
    }
    return pos;
}

uint8_t RsPanel_DecodeActivity(const uint8_t *src, uint16_t src_len, RsPanelActivity *out_act)
{
    if (src == 0 || out_act == 0 || src_len < RS_PANEL_ACTIVITY_PAYLOAD_SIZE) {
        return 0u;
    }
    memset(out_act, 0, sizeof(*out_act));
    out_act->dev_type = src[0];
    out_act->fw_ver = rs_get_u16le(&src[1]);
    out_act->hw_id = rs_get_u16le(&src[3]);
    out_act->status = src[5];
    out_act->uptime_sec = (uint32_t)src[6] |
                          ((uint32_t)src[7] << 8) |
                          ((uint32_t)src[8] << 16) |
                          ((uint32_t)src[9] << 24);
    if (src_len >= RS_PANEL_ACTIVITY_PAYLOAD_SIZE_UID) {
        out_act->uid0 = (uint32_t)src[10] |
                        ((uint32_t)src[11] << 8) |
                        ((uint32_t)src[12] << 16) |
                        ((uint32_t)src[13] << 24);
        out_act->uid1 = (uint32_t)src[14] |
                        ((uint32_t)src[15] << 8) |
                        ((uint32_t)src[16] << 16) |
                        ((uint32_t)src[17] << 24);
        out_act->uid2 = (uint32_t)src[18] |
                        ((uint32_t)src[19] << 8) |
                        ((uint32_t)src[20] << 16) |
                        ((uint32_t)src[21] << 24);
        out_act->uid_valid = 1u;
    }
    return 1u;
}

uint16_t RsPanel_EncodeDiscoverRsp(uint8_t *dst, uint16_t dst_size, const RsPanelDiscoverRsp *rsp)
{
    uint16_t pos = 0u;

    if (dst == 0 || rsp == 0 || dst_size < RS_PANEL_DISCOVER_RSP_SIZE) {
        return 0u;
    }
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], rsp->uid0));
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], rsp->uid1));
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], rsp->uid2));
    dst[pos++] = rsp->current_addr;
    dst[pos++] = rsp->flags;
    return pos;
}

uint8_t RsPanel_DecodeDiscoverRsp(const uint8_t *src, uint16_t src_len, RsPanelDiscoverRsp *out_rsp)
{
    if (src == 0 || out_rsp == 0 || src_len < RS_PANEL_DISCOVER_RSP_SIZE) {
        return 0u;
    }
    out_rsp->uid0 = (uint32_t)src[0] |
                    ((uint32_t)src[1] << 8) |
                    ((uint32_t)src[2] << 16) |
                    ((uint32_t)src[3] << 24);
    out_rsp->uid1 = (uint32_t)src[4] |
                    ((uint32_t)src[5] << 8) |
                    ((uint32_t)src[6] << 16) |
                    ((uint32_t)src[7] << 24);
    out_rsp->uid2 = (uint32_t)src[8] |
                    ((uint32_t)src[9] << 8) |
                    ((uint32_t)src[10] << 16) |
                    ((uint32_t)src[11] << 24);
    out_rsp->current_addr = src[12];
    out_rsp->flags = src[13];
    return 1u;
}

uint16_t RsPanel_EncodeAssignByUid(uint8_t *dst, uint16_t dst_size, const RsPanelAssignByUidCmd *cmd)
{
    uint16_t pos = 0u;

    if (dst == 0 || cmd == 0 || dst_size < RS_PANEL_ASSIGN_BY_UID_SIZE) {
        return 0u;
    }
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], cmd->uid0));
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], cmd->uid1));
    pos = (uint16_t)(pos + rs_put_u32le(&dst[pos], cmd->uid2));
    dst[pos++] = cmd->new_addr;
    return pos;
}

uint8_t RsPanel_DecodeAssignByUid(const uint8_t *src, uint16_t src_len, RsPanelAssignByUidCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < RS_PANEL_ASSIGN_BY_UID_SIZE) {
        return 0u;
    }
    out_cmd->uid0 = (uint32_t)src[0] |
                    ((uint32_t)src[1] << 8) |
                    ((uint32_t)src[2] << 16) |
                    ((uint32_t)src[3] << 24);
    out_cmd->uid1 = (uint32_t)src[4] |
                    ((uint32_t)src[5] << 8) |
                    ((uint32_t)src[6] << 16) |
                    ((uint32_t)src[7] << 24);
    out_cmd->uid2 = (uint32_t)src[8] |
                    ((uint32_t)src[9] << 8) |
                    ((uint32_t)src[10] << 16) |
                    ((uint32_t)src[11] << 24);
    out_cmd->new_addr = src[12];
    return 1u;
}

uint8_t RsPanel_DecodeProfileSetCmd(const uint8_t *src, uint16_t src_len, RsPanelProfileSetCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < 1u) {
        return 0u;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->sub = src[0];
    switch (out_cmd->sub) {
    case RS_PANEL_PROFILE_SET_ORIENTATION:
    case RS_PANEL_PROFILE_SET_BTN_MASK:
    case RS_PANEL_PROFILE_SET_JOURNAL_LINES:
    case RS_PANEL_PROFILE_SET_RS_ADDR:
    case RS_PANEL_PROFILE_SET_PANEL_TYPE:
        if (src_len < 2u) {
            return 0u;
        }
        out_cmd->value.orientation = src[1];
        break;
    case RS_PANEL_PROFILE_SET_LED_MASK:
        if (src_len < 3u) {
            return 0u;
        }
        out_cmd->value.led_enable = rs_get_u16le(&src[1]);
        break;
    case RS_PANEL_PROFILE_SET_FACTORY_RESET:
        break;
    default:
        return 0u;
    }
    return 1u;
}

static uint8_t rs_panel_should_forward_to_host(const RsBusFrameView *frame)
{
    if (frame == 0) {
        return 0u;
    }
    /* Инвентаризация панелей на WiFi/ПО: addr в заголовке RS, тип в payload. */
    if (frame->cmd == RS_PANEL_RSP_ACTIVITY) {
        return 1u; /* payload[0]=DEVICE_PANEL_TYPE(30) / BOOTLOADER(31) */
    }
    if (frame->cmd == RS_PANEL_RSP_CAPS) {
        return 1u; /* ui_profile = PANEL_TYPE_1/2/3 */
    }
    if (frame->cmd == RS_PANEL_RSP_DISCOVER) {
        return 1u; /* uid + current_addr */
    }
    if (frame->cmd == RS_PANEL_RSP_ACK) {
        return 1u;
    }
    if (frame->cmd == RS_PANEL_CMD_BOOT_RESET_MCU ||
        frame->cmd == RS_PANEL_CMD_BOOT_SET_UPD_WORD ||
        frame->cmd == RS_PANEL_CMD_BOOT_UPD_TRANSMIT ||
        frame->cmd == RS_PANEL_CMD_BOOT_GET_VERSION) {
        return 1u;
    }
    return 0u;
}

static void rs_panel_forward_frame_to_esp(const RsBusFrameView *frame)
{
    uint8_t raw[ESP_UART_BODY_MAX];
    uint16_t len;

    /* ESP под питанием (сессия WiFi/сниффер). Не ждём TCP — иначе панель
     * не видна на этапе поднятия линка. */
    if (frame == 0 || Esp32_IsEnabled() == 0u) {
        return;
    }
    len = RsBus_FrameEncode(raw,
                            (uint16_t)sizeof(raw),
                            frame->addr,
                            frame->seq,
                            frame->flags,
                            frame->cmd,
                            frame->payload,
                            frame->payload_len);
    if (len == 0u || len > ESP_UART_BODY_MAX) {
        return;
    }
    (void)UartBridge_SendBsuPacket(BSU_PKT_TYPE_ESP_UART, g_esp_uart_fwd_seq++, raw, len);
}

/* Только WiFi/ПО — на RS не кладём (иначе лишний трафик/коллизии). */
static void rs_panel_master_wifi_presence_tick(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t i;

    if (master == 0 || Esp32_IsEnabled() == 0u || master->panel_count == 0u) {
        return;
    }
    if (s_wifi_presence_next_ms != 0u &&
        (int32_t)(now_ms - s_wifi_presence_next_ms) < 0) {
        return;
    }
    s_wifi_presence_next_ms = now_ms + RS_PANEL_WIFI_PRESENCE_PERIOD_MS;

    for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        RsPanelActivity act;
        uint8_t payload[RS_PANEL_ACTIVITY_PAYLOAD_SIZE_UID];
        uint16_t plen;
        RsBusFrameView view;

        if (panel->cfg.enabled == 0u || panel->cfg.addr == 0u) {
            continue;
        }
        if (panel->last_rx_ms == 0u ||
            (now_ms - panel->last_rx_ms) > RS_PANEL_WIFI_PRESENCE_ALIVE_MS) {
            continue;
        }
        /* Без свежего CAPS (панель в бутлоадере) — только реальный ACTIVITY с шины. */
        if (panel->caps_valid == 0u) {
            continue;
        }

        memset(&act, 0, sizeof(act));
        act.dev_type = (uint8_t)DEVICE_PANEL_TYPE;
        if (panel->caps_valid != 0u) {
            act.fw_ver = panel->caps.fw_ver;
            /* Как панель: ACTIVITY.hw_id = формфактор PANEL_TYPE_1/2/3. */
            act.hw_id = (uint16_t)panel->caps.ui_profile;
            if (act.hw_id == 0u) {
                act.hw_id = panel->caps.hw_id;
            }
            act.status = panel->caps.status;
        }
        if (panel->uptime_valid != 0u) {
            act.uptime_sec = panel->last_uptime_sec;
        }
        if (panel->uid_valid != 0u) {
            act.uid0 = panel->uid0;
            act.uid1 = panel->uid1;
            act.uid2 = panel->uid2;
            act.uid_valid = 1u;
        }

        plen = RsPanel_EncodeActivity(payload, (uint16_t)sizeof(payload), &act);
        if (plen < RS_PANEL_ACTIVITY_PAYLOAD_SIZE) {
            continue;
        }

        view.addr = panel->cfg.addr;
        view.seq = s_wifi_presence_seq++;
        if (s_wifi_presence_seq == 0u) {
            s_wifi_presence_seq = 1u;
        }
        view.flags = (uint8_t)RS_BUS_FLAG_DIR;
        view.cmd = (uint8_t)RS_PANEL_RSP_ACTIVITY;
        view.payload_len = plen;
        view.payload = payload;
        rs_panel_forward_frame_to_esp(&view);
    }
}

static uint8_t rs_panel_uid_eq(uint32_t a0, uint32_t a1, uint32_t a2,
                               uint32_t b0, uint32_t b1, uint32_t b2)
{
    return (a0 == b0 && a1 == b1 && a2 == b2) ? 1u : 0u;
}

static void rs_panel_master_discover_reset(RsPanelMaster *master)
{
    if (master == 0) {
        return;
    }
    master->discover_count = 0u;
    master->addr_assign_idx = 0u;
    memset(master->discover, 0, sizeof(master->discover));
}

static void rs_panel_master_discover_add(RsPanelMaster *master, const RsPanelDiscoverRsp *rsp)
{
    uint8_t i;

    if (master == 0 || rsp == 0) {
        return;
    }
    for (i = 0u; i < master->discover_count; i++) {
        if (rs_panel_uid_eq(master->discover[i].uid0, master->discover[i].uid1, master->discover[i].uid2,
                            rsp->uid0, rsp->uid1, rsp->uid2) != 0u) {
            master->discover[i].current_addr = rsp->current_addr;
            master->discover[i].flags = rsp->flags;
            master->discover[i].seen = 1u;
            return;
        }
    }
    if (master->discover_count >= RS_PANEL_MAX_PANELS) {
        return;
    }
    i = master->discover_count++;
    master->discover[i].uid0 = rsp->uid0;
    master->discover[i].uid1 = rsp->uid1;
    master->discover[i].uid2 = rsp->uid2;
    master->discover[i].current_addr = rsp->current_addr;
    master->discover[i].flags = rsp->flags;
    master->discover[i].assigned_addr = 0u;
    master->discover[i].seen = 1u;
}

static uint8_t rs_panel_master_addr_in_use(const RsPanelMaster *master, uint8_t addr, uint8_t skip_idx)
{
    uint8_t i;

    if (master == 0) {
        return 0u;
    }
    for (i = 0u; i < master->discover_count; i++) {
        if (i == skip_idx) {
            continue;
        }
        if (master->discover[i].assigned_addr == addr) {
            return 1u;
        }
    }
    return 0u;
}

static uint8_t rs_panel_master_pick_free_addr(const RsPanelMaster *master, uint8_t preferred, uint8_t skip_idx)
{
    uint8_t a;

    if (preferred >= 1u && preferred <= RS_PANEL_MAX_PANELS &&
        rs_panel_master_addr_in_use(master, preferred, skip_idx) == 0u) {
        return preferred;
    }
    for (a = 1u; a <= RS_PANEL_MAX_PANELS; a++) {
        if (rs_panel_master_addr_in_use(master, a, skip_idx) == 0u) {
            return a;
        }
    }
    return 0u;
}

static void rs_panel_master_plan_assigns(RsPanelMaster *master)
{
    uint8_t i;
    uint8_t used[RS_PANEL_MAX_PANELS + 1u];

    if (master == 0) {
        return;
    }
    memset(used, 0, sizeof(used));

    /* Сначала оставляем уже уникальные assigned адреса. */
    for (i = 0u; i < master->discover_count; i++) {
        uint8_t cur = master->discover[i].current_addr;
        uint8_t assigned = (master->discover[i].flags & RS_PANEL_DISCOVER_FLAG_ASSIGNED) != 0u;
        uint8_t clash = 0u;
        uint8_t j;

        master->discover[i].assigned_addr = 0u;
        if (assigned == 0u || cur < 1u || cur > RS_PANEL_MAX_PANELS) {
            continue;
        }
        for (j = 0u; j < i; j++) {
            if (master->discover[j].assigned_addr == cur) {
                clash = 1u;
                break;
            }
        }
        if (clash == 0u && used[cur] == 0u) {
            master->discover[i].assigned_addr = cur;
            used[cur] = 1u;
        }
    }

    /* Остальным — свободные 1..MAX. Virgin с current=1: первый оставить 1, остальным новые. */
    for (i = 0u; i < master->discover_count; i++) {
        uint8_t prefer;

        if (master->discover[i].assigned_addr != 0u) {
            continue;
        }
        prefer = master->discover[i].current_addr;
        if (prefer < 1u || prefer > RS_PANEL_MAX_PANELS || used[prefer] != 0u) {
            prefer = 0u;
        }
        master->discover[i].assigned_addr =
            rs_panel_master_pick_free_addr(master, prefer, i);
        if (master->discover[i].assigned_addr != 0u) {
            used[master->discover[i].assigned_addr] = 1u;
        }
    }
}

static void rs_panel_master_prune_dead_slots(RsPanelMaster *master);

/* ADD-only: новый слот или привязка UID к пустому слоту того же addr. */
static uint8_t rs_panel_master_add_or_bind_panel(RsPanelMaster *master,
                                                uint8_t addr,
                                                uint32_t uid0,
                                                uint32_t uid1,
                                                uint32_t uid2,
                                                uint8_t uid_valid)
{
    uint8_t i;
    uint8_t addr_slot = 0xFFu;

    if (master == 0 || addr < 1u || addr > RS_PANEL_MAX_PANELS) {
        return 0xFFu;
    }
    for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (uid_valid != 0u && panel->uid_valid != 0u &&
            rs_panel_uid_eq(panel->uid0, panel->uid1, panel->uid2,
                            uid0, uid1, uid2) != 0u) {
            if (panel->cfg.addr != addr) {
                panel->cfg.addr = addr;
                RsPanelV3Master_RequestCatalogResyncPanel(i);
            }
            return i;
        }
        if (panel->cfg.addr == addr) {
            addr_slot = i;
        }
    }
    if (addr_slot != 0xFFu) {
        PanelState *slot = &master->panels[addr_slot];
        if (slot->uid_valid == 0u && uid_valid != 0u) {
            slot->uid0 = uid0;
            slot->uid1 = uid1;
            slot->uid2 = uid2;
            slot->uid_valid = 1u;
            RsPanelV3Master_RequestCatalogResyncPanel(addr_slot);
        }
        return addr_slot;
    }
    if (master->panel_count >= RS_PANEL_MAX_PANELS) {
        return 0xFFu;
    }
    {
        PanelConfig cfg;
        uint8_t n = master->panel_count;
        memset(&cfg, 0, sizeof(cfg));
        cfg.enabled = 1u;
        cfg.addr = addr;
        cfg.role = (n == 0u) ? RS_PANEL_ROLE_PRIMARY : RS_PANEL_ROLE_NORMAL;
        /* 1 панель: poll_ms=1; 2+: 40 мс — иначе RR+turnaround всё равно ~50+ мс. */
        /* Даже 1 панель: MULTI_POLL — иначе poll_ms=1 вечно due и душит scan. */
        cfg.poll_ms = RS_PANEL_MULTI_POLL_MS;
        cfg.expected_hw_id = 0u;
        PanelState_Reset(&master->panels[n]);
        PanelState_BindConfig(&master->panels[n], &cfg);
        if (uid_valid != 0u) {
            master->panels[n].uid0 = uid0;
            master->panels[n].uid1 = uid1;
            master->panels[n].uid2 = uid2;
            master->panels[n].uid_valid = 1u;
        }
        master->panel_count = (uint8_t)(n + 1u);
        if (master->panel_count >= 2u) {
            uint8_t pi;
            for (pi = 0u; pi < master->panel_count; pi++) {
                if (master->panels[pi].cfg.poll_ms < RS_PANEL_MULTI_POLL_MS) {
                    master->panels[pi].cfg.poll_ms = RS_PANEL_MULTI_POLL_MS;
                }
            }
        }
        RsPanelV3Master_RequestCatalogResyncPanel(n);
        /* TIME в ближайшем RR POLL — иначе панель сидит на 00:00 после ADD. */
        RsPanelV3Master_ForceTimeSyncPanel(n);
        g_rs_master_dbg.addr_scan_found++;
        return n;
    }
}

/* После collision ASSIGN: обновить addr по UID / ADD — без PanelState_Reset известных. */
static void rs_panel_master_apply_discovered_panels(RsPanelMaster *master)
{
    uint8_t i;

    if (master == 0) {
        return;
    }
    for (i = 0u; i < master->discover_count; i++) {
        RsPanelDiscoverEntry *e = &master->discover[i];
        if (e->assigned_addr == 0u) {
            continue;
        }
        (void)rs_panel_master_add_or_bind_panel(master,
                                                e->assigned_addr,
                                                e->uid0, e->uid1, e->uid2,
                                                1u);
    }
    rs_panel_master_prune_dead_slots(master);
}

static void rs_panel_master_start_discover(RsPanelMaster *master, uint32_t now_ms)
{
    if (master == 0) {
        return;
    }
    rs_panel_master_discover_reset(master);
    master->addr_fsm = RS_PANEL_ADDR_FSM_DISCOVER;
    master->addr_fsm_deadline_ms = now_ms + RS_PANEL_ADDR_DISCOVER_WINDOW_MS;
    master->collision_pending = 0u;
    (void)RsBus_SendFrame(&master->bus,
                          RS_BUS_BROADCAST_ADDR,
                          master->next_seq++,
                          0u,
                          RS_PANEL_CMD_DISCOVER,
                          0,
                          0u);
}

/* Слоты только по RSP (ADD). Пустые addr в таблице не держим — нечего prune.
 * Известный addr (даже offline) оставляем в RR: reconnect без ожидания scan. */
static void rs_panel_master_prune_dead_slots(RsPanelMaster *master)
{
    (void)master;
}

/*
 * Скан адресов 1..MAX: один проход (~1.6 с) после появления ≥1 панели —
 * добить остальных; при 0 панелей проход повторяется (иначе brick после 8).
 * Известный addr — skip в том же тике. Неизвестный — SYS POLL + quiet.
 * done не ставим в момент TX последнего addr — ждём следующий тик (RSP/ADD).
 */
static uint8_t rs_panel_master_addr_scan_tick(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t addr;
    uint8_t i;
    uint8_t try_n;
    uint8_t payload[8];

    if (master == 0 /*|| s_addr_scan_done != 0u**/) {
        return 0u;
    }
    if (master->addr_fsm != RS_PANEL_ADDR_FSM_IDLE ||
        master->collision_pending != 0u) {
        return 0u;
    }
    if (master->panel_count >= RS_PANEL_MAX_PANELS) {
        s_addr_scan_done = 1u;
        return 0u;
    }
    if (s_addr_scan_next_ms != 0u &&
        (int32_t)(now_ms - s_addr_scan_next_ms) < 0) {
        return 0u;
    }

    /* Проход закончен (next ушёл за MAX) — после quiet/RSP последнего probe. */
    if (s_addr_scan_next > RS_PANEL_MAX_PANELS) {
        s_addr_scan_probe_addr = 0u;
        if (master->panel_count >= RS_PANEL_MAX_PANELS) {
            s_addr_scan_done = 1u;
            return 0u;
        }
        /* 0 панелей или есть свободные слоты: новый проход (hot-plug). */
        s_addr_scan_done = 0u;
        s_addr_scan_next = 1u;
        s_addr_scan_next_ms = now_ms +
            ((master->panel_count == 0u)
                 ? RS_PANEL_ADDR_SCAN_INTERVAL_MS
                 : RS_PANEL_ADDR_RESCAN_MS);
        g_rs_master_dbg.addr_scan_next = s_addr_scan_next;
        return 0u;
    }

    /* Пропуск известных в том же тике; один probe на вызов. */
    for (try_n = 0u; try_n < RS_PANEL_MAX_PANELS; try_n++) {
        if (s_addr_scan_next < 1u || s_addr_scan_next > RS_PANEL_MAX_PANELS) {
            break;
        }
        addr = s_addr_scan_next;
        s_addr_scan_next = (uint8_t)(addr + 1u);
        g_rs_master_dbg.addr_scan_next = s_addr_scan_next;

        {
            uint8_t known = 0u;
            for (i = 0u; i < master->panel_count; i++) {
                if (master->panels[i].cfg.addr == addr) {
                    known = 1u;
                    break;
                }
            }
            if (known != 0u) {
                continue;
            }
        }

        {
            uint16_t plen = RsPanelV3Master_EncodeSysOnlyPoll(payload, sizeof(payload),
                                                             0xFFu);
            if (plen == 0u) {
                continue;
            }
            s_addr_scan_next_ms = now_ms + RS_PANEL_ADDR_SCAN_INTERVAL_MS;
            s_addr_scan_probe_addr = addr;
            g_rs_master_dbg.addr_scan_probe_addr = addr;
            g_rs_master_dbg.addr_scan_tx++;
            (void)RsBus_SendFrame(&master->bus,
                                  addr,
                                  master->next_seq++,
                                  0u,
                                  RS_PANEL_CMD_POLL,
                                  payload,
                                  plen);
        }
        rs_panel_master_arm_bus_quiet(now_ms);
        return 1u;
    }

    /* Все addr в этом проходе уже известны / next за пределами. */
    if (s_addr_scan_next > RS_PANEL_MAX_PANELS) {
        if (master->panel_count >= RS_PANEL_MAX_PANELS) {
            s_addr_scan_done = 1u;
        } else {
            s_addr_scan_done = 0u;
            s_addr_scan_next = 1u;
            s_addr_scan_next_ms = now_ms +
                ((master->panel_count == 0u)
                     ? RS_PANEL_ADDR_SCAN_INTERVAL_MS
                     : RS_PANEL_ADDR_RESCAN_MS);
            g_rs_master_dbg.addr_scan_next = s_addr_scan_next;
        }
    }
    return 0u;
}

/*
 * Unicast ACTIVITY_REQ: 1 кадр/~1 с RR по известным слотам.
 * Приоритет — слоты без UID (hot-plug после POLL-scan ADD).
 * Не broadcast. Возвращает 1, если TX ушёл в этом тике.
 */
static uint8_t rs_panel_master_activity_req_tick(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t i;
    uint8_t chosen = 0xFFu;
    PanelState *panel;

    if (master == 0 || master->panel_count == 0u) {
        return 0u;
    }

    /* Срочный bind UID после ADD без UID — один кадр, не шторм. */
    if (s_activity_req_urgent_idx < master->panel_count) {
        chosen = s_activity_req_urgent_idx;
        s_activity_req_urgent_idx = 0xFFu;
    } else {
        if (s_activity_req_next_ms != 0u &&
            (int32_t)(now_ms - s_activity_req_next_ms) < 0) {
            return 0u;
        }
        /* Сначала слоты без UID. */
        for (i = 0u; i < master->panel_count; i++) {
            uint8_t idx =
                (uint8_t)((s_activity_req_rr_idx + i) % master->panel_count);
            PanelState *cand = &master->panels[idx];
            if (cand->cfg.enabled == 0u || cand->cfg.addr == 0u) {
                continue;
            }
            if (cand->uid_valid == 0u) {
                chosen = idx;
                break;
            }
        }
        if (chosen == 0xFFu) {
            for (i = 0u; i < master->panel_count; i++) {
                uint8_t idx =
                    (uint8_t)((s_activity_req_rr_idx + i) % master->panel_count);
                PanelState *cand = &master->panels[idx];
                if (cand->cfg.enabled == 0u || cand->cfg.addr == 0u) {
                    continue;
                }
                chosen = idx;
                break;
            }
        }
        if (chosen == 0xFFu) {
            s_activity_req_next_ms = now_ms + RS_PANEL_ACTIVITY_REQ_PERIOD_MS;
            return 0u;
        }
        s_activity_req_rr_idx = (uint8_t)((chosen + 1u) % master->panel_count);
    }

    panel = &master->panels[chosen];
    if (panel->cfg.enabled == 0u || panel->cfg.addr == 0u) {
        return 0u;
    }
    /* Всегда ставить период (в т.ч. urgent). Иначе next_ms==0 → ACTIVITY каждый
     * тик Process10ms (~2 кГц) и POLL голодает → «НЕТ СВЯЗИ» на панели. */
    s_activity_req_next_ms = now_ms + RS_PANEL_ACTIVITY_REQ_PERIOD_MS;
    g_rs_master_dbg.activity_req_tx++;
    g_rs_master_dbg.activity_req_addr = panel->cfg.addr;
    (void)RsBus_SendFrame(&master->bus,
                          panel->cfg.addr,
                          master->next_seq++,
                          0u,
                          RS_PANEL_CMD_ACTIVITY_REQ,
                          0,
                          0u);
    rs_panel_master_arm_bus_quiet(now_ms);
    return 1u;
}

/* 1 = TX ASSIGN ушёл; 0 = только переход в SETTLE (без TX). */
static uint8_t rs_panel_master_send_next_assign(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t payload[RS_PANEL_ASSIGN_BY_UID_SIZE];
    uint16_t len;
    RsPanelAssignByUidCmd cmd;

    if (master == 0) {
        return 0u;
    }
    while (master->addr_assign_idx < master->discover_count) {
        RsPanelDiscoverEntry *e = &master->discover[master->addr_assign_idx];
        master->addr_assign_idx++;
        if (e->assigned_addr == 0u) {
            continue;
        }
        if (e->assigned_addr == e->current_addr &&
            (e->flags & RS_PANEL_DISCOVER_FLAG_ASSIGNED) != 0u) {
            /* Уже на нужном адресе и помечена assigned — пропускаем. */
            continue;
        }
        cmd.uid0 = e->uid0;
        cmd.uid1 = e->uid1;
        cmd.uid2 = e->uid2;
        cmd.new_addr = e->assigned_addr;
        len = RsPanel_EncodeAssignByUid(payload, sizeof(payload), &cmd);
        if (len == 0u) {
            continue;
        }
        (void)RsBus_SendFrame(&master->bus,
                              RS_BUS_BROADCAST_ADDR,
                              master->next_seq++,
                              0u,
                              RS_PANEL_CMD_ASSIGN_BY_UID,
                              payload,
                              len);
        master->addr_fsm_deadline_ms = now_ms + RS_PANEL_ADDR_ASSIGN_GAP_MS;
        return 1u;
    }
    rs_panel_master_apply_discovered_panels(master);
    master->addr_fsm = RS_PANEL_ADDR_FSM_SETTLE;
    master->addr_fsm_deadline_ms = now_ms + RS_PANEL_ADDR_SETTLE_MS;
    master->first_boot_discover_done = 1u;
    return 0u;
}

/* 1 = в этом тике ушёл TX (DISCOVER/ASSIGN) — не слать POLL в том же тике. */
static uint8_t rs_panel_master_addr_fsm_tick(RsPanelMaster *master, uint32_t now_ms)
{
    if (master == 0) {
        return 0u;
    }

    if (master->addr_fsm == RS_PANEL_ADDR_FSM_IDLE) {
        if (master->collision_pending != 0u) {
            if (master->last_addr_cycle_ms != 0u &&
                (int32_t)(now_ms - master->last_addr_cycle_ms) <
                    (int32_t)RS_PANEL_ADDR_COLLISION_COOLDOWN_MS) {
                /* Cooldown: POLL идёт, ACTIVITY видна по WiFi. */
                return 0u;
            }
            rs_panel_master_start_discover(master, now_ms);
            return 1u;
        }
        /* first_boot: Init ставит done=1 — WAIT_BOOT/DISCOVER не гейтят шину.
         * Новые панели: addr_scan 1..MAX + ACTIVITY ADD-only (не periodic DISCOVER). */
        return 0u;
    }

    if ((int32_t)(now_ms - master->addr_fsm_deadline_ms) < 0) {
        return 0u;
    }

    switch (master->addr_fsm) {
    case RS_PANEL_ADDR_FSM_WAIT_BOOT:
        rs_panel_master_start_discover(master, now_ms);
        return 1u;
    case RS_PANEL_ADDR_FSM_DISCOVER:
        if (master->discover_count == 0u) {
            /* Пустой DISCOVER — IDLE. Повтор только по collision_pending. */
            master->first_boot_discover_done = 1u;
            master->addr_fsm = RS_PANEL_ADDR_FSM_IDLE;
            master->last_addr_cycle_ms = now_ms;
            return 0u;
        }
        rs_panel_master_plan_assigns(master);
        master->addr_fsm = RS_PANEL_ADDR_FSM_ASSIGN;
        master->addr_assign_idx = 0u;
        return rs_panel_master_send_next_assign(master, now_ms);
    case RS_PANEL_ADDR_FSM_ASSIGN:
        return rs_panel_master_send_next_assign(master, now_ms);
    case RS_PANEL_ADDR_FSM_SETTLE:
        master->addr_fsm = RS_PANEL_ADDR_FSM_IDLE;
        master->last_addr_cycle_ms = now_ms;
        return 0u;
    default:
        master->addr_fsm = RS_PANEL_ADDR_FSM_IDLE;
        master->last_addr_cycle_ms = now_ms;
        return 0u;
    }
}

static void rs_panel_master_on_activity_uid(RsPanelMaster *master,
                                           uint8_t frame_addr,
                                           const RsPanelActivity *act)
{
    uint8_t i;
    PanelState *matched = 0;

    if (master == 0 || act == 0 || act->uid_valid == 0u) {
        return;
    }
    if (act->dev_type != DEVICE_PANEL_TYPE) {
        return;
    }

    for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.addr != frame_addr) {
            continue;
        }
        matched = panel;
        if (panel->uid_valid == 0u) {
            panel->uid0 = act->uid0;
            panel->uid1 = act->uid1;
            panel->uid2 = act->uid2;
            panel->uid_valid = 1u;
            return;
        }
        if (rs_panel_uid_eq(panel->uid0, panel->uid1, panel->uid2,
                            act->uid0, act->uid1, act->uid2) == 0u) {
            /* Тот же addr, другой UID — коллизия: сбросить UID слота и
             * перезапустить DISCOVER (v3: POLL не паузим — см. Process10ms). */
            panel->uid_valid = 0u;
            if (master->addr_fsm == RS_PANEL_ADDR_FSM_IDLE) {
                master->collision_pending = 1u;
            }
            return;
        }
        return;
    }

    /* ACTIVITY на addr, которого нет в слотах. */
    (void)matched;
    if (frame_addr < 1u || frame_addr > RS_PANEL_MAX_PANELS) {
        return;
    }
    /* PROFILE_SET сменил RS-addr: тот же UID, новый addr — перенести слот. */
    for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        uint8_t j;
        uint8_t addr_taken = 0u;

        if (panel->uid_valid == 0u ||
            rs_panel_uid_eq(panel->uid0, panel->uid1, panel->uid2,
                            act->uid0, act->uid1, act->uid2) == 0u) {
            continue;
        }
        if (panel->cfg.addr == frame_addr) {
            return;
        }
        for (j = 0u; j < master->panel_count; j++) {
            if (j != i && master->panels[j].cfg.addr == frame_addr) {
                addr_taken = 1u;
                break;
            }
        }
        if (addr_taken != 0u) {
            if (master->addr_fsm == RS_PANEL_ADDR_FSM_IDLE) {
                master->collision_pending = 1u;
            }
            return;
        }
        panel->cfg.addr = frame_addr;
        RsPanelV3Master_RequestCatalogResyncPanel(i);
        return;
    }

    /* Неизвестный addr / UID: ADD-only (скан + ACTIVITY hot-plug). */
    if (rs_panel_master_add_or_bind_panel(master, frame_addr,
                                         act->uid0, act->uid1, act->uid2,
                                         1u) != 0xFFu) {
        rs_panel_master_prune_dead_slots(master);
        return;
    }
    if (master->addr_fsm == RS_PANEL_ADDR_FSM_IDLE) {
        master->collision_pending = 1u;
    }
}

static void rs_panel_master_on_frame(const RsBusFrameView *frame, void *ctx)
{
    RsPanelMaster *master = (RsPanelMaster *)ctx;
    uint8_t i;

    if (master == 0 || frame == 0) {
        return;
    }
    /* Host/sniffer → ППКУ: включить WiFi (DIR=0). Панели эту cmd не обрабатывают. */
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u &&
        frame->cmd == RS_PANEL_CMD_PPKY_WIFI_ENABLE) {
        s_wifi_enable_req = 1u;
        return;
    }
    /* Host/sniffer → ППКУ: вкл/выкл зеркало CAN→RS. */
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u &&
        frame->cmd == RS_PANEL_CMD_PPKY_CAN_MIRROR_SET) {
        uint8_t en = 1u;
        if (frame->payload_len > 0u && frame->payload != 0) {
            en = (frame->payload[0] != 0u) ? 1u : 0u;
        }
        RsPanelMaster_SetCanMirrorEnable(en);
        return;
    }
    /* Host→ППКУ: BSU CAN в payload → на шину CAN. */
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u &&
        frame->cmd == RS_PANEL_CMD_CAN_TO_BUS) {
        (void)CanHostTxFromBsu(frame->payload, frame->payload_len);
        return;
    }
    /* Собственные/чужие кадры зеркала — не считать wrong_dir. */
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u &&
        frame->cmd == RS_PANEL_CMD_CAN_MIRROR) {
        return;
    }
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u) {
        g_rs_master_dbg.rx_frames_wrong_dir++;
        return;
    }
    g_rs_master_dbg.rx_frames_ok++;
    /* RSP ожидаемого addr — линия свободна раньше turnaround. */
    rs_panel_master_clear_bus_quiet_from_rx();

    /* Прокидка в WiFi/ПО: activity + ответы boot-команд (тот же addr панели). */
    if (rs_panel_should_forward_to_host(frame) != 0u) {
        rs_panel_forward_frame_to_esp(frame);
    }

    if (frame->cmd == RS_PANEL_RSP_DISCOVER) {
        RsPanelDiscoverRsp drsp;
        if (RsPanel_DecodeDiscoverRsp(frame->payload, frame->payload_len, &drsp) != 0u) {
            /* Только во время collision DISCOVER FSM. */
            if (master->addr_fsm == RS_PANEL_ADDR_FSM_DISCOVER) {
                rs_panel_master_discover_add(master, &drsp);
            }
        }
        return;
    }

    if (frame->cmd == RS_PANEL_RSP_ACTIVITY) {
        RsPanelActivity act;
        if (RsPanel_DecodeActivity(frame->payload, frame->payload_len, &act) != 0u) {
            rs_panel_master_on_activity_uid(master, frame->addr, &act);
        }
    }

    {
        uint8_t matched = 0u;

        for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.addr != frame->addr) {
            g_rs_master_dbg.rx_frames_wrong_addr++;
            continue;
        }
        matched = 1u;

        if (frame->cmd == RS_PANEL_RSP_CAPS) {
            RsPanelCaps caps;
            if (RsPanel_DecodeCaps(frame->payload, frame->payload_len, &caps)) {
                uint8_t was_ready = PanelState_IsReady(panel);
                g_rs_master_dbg.rsp_caps_rx++;
                PanelState_OnCaps(panel, &caps, HAL_GetTick());
                g_rs_master_dbg.panel_caps_valid = panel->caps_valid;
                g_rs_master_dbg.panel_link_state = (uint8_t)panel->link_state;
                if (was_ready == 0u && PanelState_IsReady(panel) != 0u) {
                    rs_panel_master_on_panel_became_ready(master);
                }
            } else {
                g_rs_master_dbg.rsp_caps_decode_fail++;
            }
        } else if (frame->cmd == RS_PANEL_RSP_ACK) {
            /* Legacy UI_DATA ACK — ignore (v3 uses POLL ack TLV). */
            panel->ack_wait_active = 0u;
            panel->pending_ack_seq = 0u;
            panel->ack_retries_left = 0u;
            panel->ack_deadline_ms = 0u;
            panel->pending_ui_stream_len = 0u;
        } else if (frame->cmd == RS_PANEL_RSP_POLL) {
            g_rs_master_dbg.rsp_poll_rx++;
            if (frame->payload_len > 0u && frame->payload[0] == RS_PANEL_V3_VERSION) {
                RsPanelV3Rsp v3rsp;
                /* last_rx_ms всегда: даже при битом DecodeRsp иначе watchdog → CAPS
                 * останавливает POLL и на минуты гаснут питание/неисправность. */
                panel->last_rx_ms = HAL_GetTick();
                s_v3_poll_seen[i] = 1u;
                if (panel->link_state == PANEL_LINK_CAPS_PENDING &&
                    panel->caps_valid != 0u) {
                    panel->link_state = PANEL_LINK_READY;
                }
                if (RsPanelV3_DecodeRsp(frame->payload, frame->payload_len, &v3rsp) != 0u) {
                    RsPanelV3Master_OnRsp(i, &v3rsp, HAL_GetTick());
                }
            }
            /* legacy POLL RSP (не v3) — игнор: только smart-panel v3. */
        } else if (frame->cmd == RS_PANEL_RSP_ACTIVITY) {
            RsPanelActivity act;
            panel->last_rx_ms = HAL_GetTick();
            /* Падение uptime_sec = панель перезапустилась (питание/reset).
             * Сбрасываем READY → снова CAPS → on_panel_became_ready (полный resync). */
            if (RsPanel_DecodeActivity(frame->payload, frame->payload_len, &act) != 0u) {
                if (act.dev_type == (uint8_t)DEVICE_PANEL_BOOTLOADER_TYPE) {
                    /* Бут: не кормить WiFi presence старым CAPS.fw_ver (fw=7 при boot). */
                    panel->link_state = PANEL_LINK_CAPS_PENDING;
                    panel->caps_valid = 0u;
                    panel->last_uptime_sec = act.uptime_sec;
                    panel->uptime_valid = 1u;
                } else if (act.dev_type == DEVICE_PANEL_TYPE) {
                    if (panel->uptime_valid != 0u &&
                        act.uptime_sec < panel->last_uptime_sec &&
                        panel->link_state == PANEL_LINK_READY) {
                        panel->link_state = PANEL_LINK_CAPS_PENDING;
                        panel->caps_valid = 0u;
                        /* Панель сбросила RTC — TIME в ближайших POLL, не ждать PERIOD. */
                        RsPanelV3Master_ForceTimeSyncPanel(i);
                    }
                    panel->last_uptime_sec = act.uptime_sec;
                    panel->uptime_valid = 1u;
                }
            }
        }
        break;
    }

        /* Скан: RSP_POLL с неизвестного addr 1..MAX → ADD слот + каталог. */
        if (matched == 0u && frame->cmd == RS_PANEL_RSP_POLL &&
            frame->addr >= 1u && frame->addr <= RS_PANEL_MAX_PANELS) {
            uint8_t idx = rs_panel_master_add_or_bind_panel(master, frame->addr,
                                                           0u, 0u, 0u, 0u);
            if (idx != 0xFFu) {
                PanelState *panel = &master->panels[idx];
                g_rs_master_dbg.rsp_poll_rx++;
                panel->last_rx_ms = HAL_GetTick();
                s_v3_poll_seen[idx] = 1u;
                /* POLL-scan ADD без UID — срочный ACTIVITY_REQ для bind UID. */
                if (panel->uid_valid == 0u) {
                    s_activity_req_urgent_idx = idx;
                }
                if (frame->payload_len > 0u &&
                    frame->payload[0] == RS_PANEL_V3_VERSION) {
                    RsPanelV3Rsp v3rsp;
                    if (panel->link_state == PANEL_LINK_CAPS_PENDING &&
                        panel->caps_valid != 0u) {
                        panel->link_state = PANEL_LINK_READY;
                    }
                    if (RsPanelV3_DecodeRsp(frame->payload, frame->payload_len,
                                            &v3rsp) != 0u) {
                        RsPanelV3Master_OnRsp(idx, &v3rsp, HAL_GetTick());
                    }
                }
                if (s_addr_scan_probe_addr == frame->addr) {
                    s_addr_scan_probe_addr = 0u;
                }
            }
        }
    }
}

void RsPanelMaster_LoadDefaultConfig(RsPanelMaster *master)
{
    if (master == 0) {
        return;
    }
    /*
     * Адреса не захардкожены: любой набор 1..MAX (до 8).
     * Слоты создаёт только RSP (addr_scan / ACTIVITY / DISCOVER) → ADD.
     * RR POLL — только живые слоты; пустые addr не занимают шину.
     */
    master->panel_count = 0u;
    master->round_robin_idx = 0u;
}

void RsPanelMaster_Init(RsPanelMaster *master,
                        UART_HandleTypeDef *uart,
                        GPIO_TypeDef *de_port,
                        uint16_t de_pin)
{
    if (master == 0) {
        return;
    }
    memset(master, 0, sizeof(*master));
    master->next_seq = 1u;
    master->addr_fsm = RS_PANEL_ADDR_FSM_IDLE;
    /* Скан адресов с первого тика; collision DISCOVER — только в фоне. */
    master->first_boot_discover_done = 1u;
    RsBus_Init(&master->bus, uart, de_port, de_pin, rs_panel_master_on_frame, master);
    RsPanelMaster_LoadDefaultConfig(master);
    g_active_master = master;
    memset(s_v3_poll_seen, 0, sizeof(s_v3_poll_seen));
    RsPanelV3Master_Init();
    RsPanelV3Master_OnBoot(HAL_GetTick());
    s_addr_scan_next = 1u;
    s_addr_scan_next_ms = 0u;
    s_addr_scan_probe_addr = 0u;
    s_addr_scan_done = 0u;
    s_wifi_presence_next_ms = 0u;
    s_wifi_presence_seq = 1u;
    s_activity_req_next_ms = 0u;
    s_activity_req_rr_idx = 0u;
    s_activity_req_urgent_idx = 0xFFu;
    s_bus_quiet_until_ms = 0u;
}

void RsPanelMaster_PushConnectionStatus(void)
{
    /* v3: wifi/rs485 в SYS flags POLL. */
}


uint8_t RsPanelMaster_IsSmallPanel(void)
{
    uint8_t i;
    if (g_active_master == 0) {
        return 0u;
    }
    for (i = 0u; i < g_active_master->panel_count; i++) {
        const PanelState *panel = &g_active_master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }
        if (panel->is_small_panel != 0u) {
            return 1u;
        }
    }
    return 0u;
}

uint8_t RsPanelMaster_GetPanelAddr(uint8_t panel_idx)
{
    if (g_active_master == 0 || panel_idx >= g_active_master->panel_count) {
        return 0xFFu;
    }
    return g_active_master->panels[panel_idx].cfg.addr;
}

void RsPanelMaster_SetCanMirrorEnable(uint8_t enable)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_can_mirror_enable = (enable != 0u) ? 1u : 0u;
    if (s_can_mirror_enable == 0u) {
        s_can_mirror_q_head = 0u;
        s_can_mirror_q_tail = 0u;
        s_can_mirror_q_count = 0u;
    }
    __set_PRIMASK(primask);
}

uint8_t RsPanelMaster_IsCanMirrorEnabled(void)
{
    return s_can_mirror_enable;
}

void RsPanelMaster_PushCanMirrorBsu(const uint8_t *bsu_pkt, uint16_t len)
{
    uint32_t primask;

    if (s_can_mirror_enable == 0u || bsu_pkt == 0 || len != BSU_PKT_CAN_SIZE) {
        return;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (s_can_mirror_q_count >= RS_PANEL_CAN_MIRROR_Q_DEPTH) {
        /* Drop oldest. */
        s_can_mirror_q_tail = (uint8_t)((s_can_mirror_q_tail + 1u) % RS_PANEL_CAN_MIRROR_Q_DEPTH);
        s_can_mirror_q_count--;
    }
    memcpy(s_can_mirror_q[s_can_mirror_q_head], bsu_pkt, BSU_PKT_CAN_SIZE);
    s_can_mirror_q_head = (uint8_t)((s_can_mirror_q_head + 1u) % RS_PANEL_CAN_MIRROR_Q_DEPTH);
    s_can_mirror_q_count++;
    __set_PRIMASK(primask);
}

static void rs_panel_master_arm_bus_quiet(uint32_t now_ms)
{
    uint32_t win = RS_PANEL_BUS_TURNAROUND_MS;
    if (Fire_IsActive() != 0u) {
        win = RS_PANEL_BUS_TURNAROUND_FIRE_MS;
    } else if (g_active_master != 0 && g_active_master->panel_count == 0u) {
        win = RS_PANEL_BUS_TURNAROUND_SCAN_MS;
    }
    s_bus_quiet_until_ms = now_ms + win;
}

static void rs_panel_master_clear_bus_quiet_from_rx(void)
{
    /* Любой DIR=1 (в т.ч. до match слота) — линия свободна. */
    if (s_bus_quiet_until_ms == 0u) {
        return;
    }
    s_bus_quiet_until_ms = 0u;
    g_rs_master_dbg.bus_quiet_cleared_rx++;
}

static uint8_t rs_panel_master_bus_is_quiet(RsPanelMaster *master, uint32_t now_ms)
{
    (void)master;
    if (s_bus_quiet_until_ms == 0u) {
        return 0u;
    }
    if ((int32_t)(now_ms - s_bus_quiet_until_ms) < 0) {
        return 1u;
    }
    /* Таймаут: НЕ Abort/Recover — только снять флаг. IRQ RX продолжает
     * копить кадр; следующий SendFrame AbortReceive только перед своим TX. */
    g_rs_master_dbg.bus_quiet_timeout_flush++;
    s_bus_quiet_until_ms = 0u;
    return 0u;
}

static void rs_panel_master_dbg_sync_rx_decode(RsPanelMaster *master)
{
    if (master == 0) {
        return;
    }
    g_rs_master_dbg.rx_decode_crc_fail = master->bus.rx_decode_crc_fail;
    g_rs_master_dbg.rx_decode_bad_hdr = master->bus.rx_decode_bad_hdr;
    g_rs_master_dbg.rx_decode_short_hold = master->bus.rx_decode_short_hold;
    g_rs_master_dbg.rx_resync_bytes = master->bus.rx_resync_bytes;
    g_rs_master_dbg.rx_err_count = master->bus.rx_err_count;
    g_rs_master_dbg.rx_last_err_code = master->bus.rx_last_err_code;
    g_rs_master_dbg.rx_rearm_wd = master->bus.rx_rearm_wd;
}

/* POLL due: все enabled (не только READY). Иначе CAPS_PENDING → ACTIVITY
 * съедает тики, пока v3-POLL ещё нужен для SYS/каталога. */
static uint8_t rs_panel_master_any_poll_due(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t i;
    if (master == 0) {
        return 0u;
    }
    for (i = 0u; i < master->panel_count; i++) {
        PanelState *p = &master->panels[i];
        if (p->cfg.enabled == 0u || p->cfg.addr == 0u) {
            continue;
        }
        if (p->last_poll_ms == 0u ||
            (now_ms - p->last_poll_ms) >= p->cfg.poll_ms) {
            return 1u;
        }
    }
    return 0u;
}

/* 1 = POLL голодает: нельзя отдавать тик скану/ACTIVITY/mirror. */
static uint8_t rs_panel_master_poll_starving(RsPanelMaster *master, uint32_t now_ms)
{
    uint8_t i;
    if (master == 0) {
        return 0u;
    }
    for (i = 0u; i < master->panel_count; i++) {
        PanelState *p = &master->panels[i];
        uint32_t gap;
        if (p->cfg.enabled == 0u || p->cfg.addr == 0u) {
            continue;
        }
        if (p->last_poll_ms == 0u) {
            return 1u;
        }
        gap = now_ms - p->last_poll_ms;
        if (i < 8u) {
            g_rs_master_dbg.poll_gap_ms_by_panel[i] = (uint16_t)((gap > 0xFFFFu) ? 0xFFFFu : gap);
            if (g_rs_master_dbg.poll_gap_ms_by_panel[i] >
                g_rs_master_dbg.poll_gap_max_ms_by_panel[i]) {
                g_rs_master_dbg.poll_gap_max_ms_by_panel[i] =
                    g_rs_master_dbg.poll_gap_ms_by_panel[i];
            }
        }
        if (gap >= RS_PANEL_POLL_MAX_GAP_MS) {
            return 1u;
        }
    }
    return 0u;
}

/* 1 кадр зеркала на RS. addr=0xFF — панели отсекают до switch. */
static uint8_t rs_panel_master_drain_can_mirror(RsPanelMaster *master)
{
    uint8_t bsu[BSU_PKT_CAN_SIZE];
    uint32_t primask;

    if (master == 0 || s_can_mirror_enable == 0u || s_can_mirror_q_count == 0u) {
        return 0u;
    }
    primask = __get_PRIMASK();
    __disable_irq();
    if (s_can_mirror_q_count == 0u) {
        __set_PRIMASK(primask);
        return 0u;
    }
    memcpy(bsu, s_can_mirror_q[s_can_mirror_q_tail], BSU_PKT_CAN_SIZE);
    s_can_mirror_q_tail = (uint8_t)((s_can_mirror_q_tail + 1u) % RS_PANEL_CAN_MIRROR_Q_DEPTH);
    s_can_mirror_q_count--;
    __set_PRIMASK(primask);

    (void)RsBus_SendFrame(&master->bus,
                          RS_BUS_ADDR_RESERVED,
                          master->next_seq++,
                          0u, /* DIR=0, без ACK_REQ */
                          RS_PANEL_CMD_CAN_MIRROR,
                          bsu,
                          BSU_PKT_CAN_SIZE);
    return 1u;
}

void RsPanelMaster_OnRxBytes(RsPanelMaster *master, const uint8_t *data, uint16_t len)
{
    if (master == 0) {
        return;
    }
    RsBus_ProcessRxBytes(&master->bus, data, len);
    rs_panel_master_dbg_sync_rx_decode(master);
}

static void rs_panel_master_inject_extend_poll_pause(uint32_t now_ms, uint8_t cmd,
                                                     const uint8_t *payload, uint16_t plen)
{
    uint32_t add_ms = 0u;

    /* Только команды прошивки/бутлоадера требуют тишины POLL.
     * РАНЬШЕ: любой InjectRawRsFrame ставил s_inject_pause_poll, а Process10ms
     * всегда вызывал extend(BOOT_SET_UPD_WORD) → 45 с без POLL даже для
     * PROFILE_SET / GET_VERSION → «нет связи» после проверки (~15 с AFTER_OK),
     * восстановление через ~45–90 с. */
    if (cmd == RS_PANEL_CMD_BOOT_SET_UPD_WORD) {
        uint32_t word_idx = 0u;
        if (payload != 0 && plen >= 3u) {
            word_idx = ((uint32_t)payload[0] << 16) |
                       ((uint32_t)payload[1] << 8) |
                       (uint32_t)payload[2];
        }
        /* Слово 0: EraseApp — десятки секунд. Дальше program/quad — короткая пауза.
         * Раньше каждый кадр 156 ставил 45 с → ретраи updater'а продлевали паузу,
         * а без RxHarvest ACK так и не уходил в WiFi. */
        add_ms = (word_idx == 0u) ? 45000u : 3000u;
    } else if (cmd == RS_PANEL_CMD_ENTER_BOOTLOADER) {
        add_ms = 5000u;
    } else if (cmd == RS_PANEL_CMD_BOOT_UPD_TRANSMIT) {
        add_ms = 10000u;
    } else if (cmd == RS_PANEL_CMD_BOOT_GET_VERSION ||
               cmd == RS_PANEL_CMD_BOOT_RESET_MCU) {
        add_ms = 2000u;
    } else {
        /* PROFILE_SET и пр.: только skip POLL в тике TX очереди, без паузы. */
        return;
    }
    {
        uint32_t until = now_ms + add_ms;
        if (s_inject_pause_until_ms == 0u ||
            (int32_t)(until - s_inject_pause_until_ms) > 0) {
            s_inject_pause_until_ms = until;
        }
    }
    g_rs_master_dbg.inject_pause_until_ms = s_inject_pause_until_ms;
    g_rs_master_dbg.inject_pause_armed_count++;
    g_rs_master_dbg.last_inject_pause_cmd = cmd;
}

static void rs_panel_master_inject_process_pending(RsPanelMaster *master, uint32_t now_ms)
{
    RsPanelInjectQItem item;
    uint32_t primask;
    RsBusFrameView view;
    uint16_t consumed = 0u;

    /* Не больше одного RS TX за 10 мс тик — иначе WiFi/ESP заливает main loop. */
    primask = __get_PRIMASK();
    __disable_irq();
    if (s_inject_q_count == 0u) {
        __set_PRIMASK(primask);
        return;
    }
    item = s_inject_q[s_inject_q_head];
    s_inject_q_head = (uint8_t)((s_inject_q_head + 1u) % RS_PANEL_INJECT_Q_DEPTH);
    s_inject_q_count--;
    __set_PRIMASK(primask);

    if (master == 0 || item.len == 0u) {
        return;
    }
    (void)RsBus_SendRaw(&master->bus, item.data, item.len);
    if (RsBus_FrameDecode(item.data, item.len, &view, &consumed) != 0u &&
        rs_panel_inject_cmd_allowed(view.cmd) != 0u) {
        rs_panel_master_inject_extend_poll_pause(now_ms, view.cmd,
                                                 view.payload, view.payload_len);
    }
}

void RsPanelMaster_Process10ms(RsPanelMaster *master, uint32_t now_ms)
{
    PanelState *panel;
    uint16_t payload_len;

    if (master == 0) {
        return;
    }

    /* До quiet/TX: Fire/ACK из RSP. Иначе при пожаре EVT из IRQ ломались. */
    RsPanelV3Master_ProcessDeferred(now_ms);

    if (s_wifi_enable_req != 0u) {
        s_wifi_enable_req = 0u;
        EspManager_RequestWifiEnable();
    }

    RsBus_RxWatchdog(&master->bus);

    /* Presence в ПО — до early-return (inject/addr_scan), не зависит от RS TX. */
    rs_panel_master_wifi_presence_tick(master, now_ms);

    /* Fallback: seed пуст — только POLL-scan 1..MAX. Один TX → quiet → ждём RSP.
     * IRQ RX не трогаем (нет Abort/Recover в quiet). can_mirror не режем RSP. */
    if (master->panel_count == 0u) {
        if (rs_panel_master_bus_is_quiet(master, now_ms) != 0u) {
            /* Не ждать IDLE: harvest DMA → on_frame → ADD (иначе forever scan). */
            RsBus_RxHarvest(&master->bus);
            RsPanelV3Master_ProcessDeferred(now_ms);
            if (master->panel_count != 0u) {
                rs_panel_master_dbg_sync_rx_decode(master);
                RsPanelMasterDebug_Timer10ms();
                g_rs_master_dbg.panel_count = master->panel_count;
                return;
            }
            g_rs_master_dbg.bus_quiet_skips++;
            rs_panel_master_dbg_sync_rx_decode(master);
            RsPanelMasterDebug_Timer10ms();
            g_rs_master_dbg.panel_count = 0u;
            return;
        }
        if (rs_panel_master_addr_scan_tick(master, now_ms) != 0u) {
            rs_panel_master_dbg_sync_rx_decode(master);
            RsPanelMasterDebug_Timer10ms();
            return;
        }
        if (master->addr_fsm != RS_PANEL_ADDR_FSM_IDLE ||
            master->collision_pending != 0u) {
            if (rs_panel_master_addr_fsm_tick(master, now_ms) != 0u) {
                rs_panel_master_arm_bus_quiet(now_ms);
                rs_panel_master_dbg_sync_rx_decode(master);
                RsPanelMasterDebug_Timer10ms();
                return;
            }
        }
        rs_panel_master_dbg_sync_rx_decode(master);
        RsPanelMasterDebug_Timer10ms();
        g_rs_master_dbg.panel_count = 0u;
        return;
    }

    /* Host→RS inject (в т.ч. 0xF3) — вне UART2 IRQ, максимум 1 кадр/тик.
     * Длинная пауза POLL — только после реальной boot-команды (см. extend). */
    if (s_inject_q_count != 0u) {
        /* Снять предыдущий ACK из DMA до TX следующего 156 — иначе при частых
         * ретраях updater'а путь inject всегда return'ит раньше quiet/harvest. */
        RsBus_RxHarvest(&master->bus);
        RsPanelV3Master_ProcessDeferred(now_ms);
        rs_panel_master_inject_process_pending(master, now_ms);
        rs_panel_master_arm_bus_quiet(now_ms);
        /* Не делаем POLL в том же тике (half-duplex). */
        RsPanelMasterDebug_Timer10ms();
        g_rs_master_dbg.menu_selected = g_menu_selected;
        g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
        g_rs_master_dbg.inject_pause_active =
            (s_inject_pause_until_ms != 0u &&
             (int32_t)(now_ms - s_inject_pause_until_ms) < 0) ? 1u : 0u;
        return;
    }

    if (s_inject_pause_until_ms != 0u) {
        if ((int32_t)(now_ms - s_inject_pause_until_ms) < 0) {
            /* Тишина TX/POLL, но RX обязателен: ACK 156/ACTIVITY бута → WiFi/ПО.
             * Без harvest DMA копит ответы, updater видит «нет ACK» после слова 0. */
            RsBus_RxHarvest(&master->bus);
            RsPanelV3Master_ProcessDeferred(now_ms);
            g_rs_master_dbg.inject_pause_active = 1u;
            g_rs_master_dbg.inject_pause_until_ms = s_inject_pause_until_ms;
            RsPanelMasterDebug_Timer10ms();
            return;
        }
        s_inject_pause_until_ms = 0u;
        g_rs_master_dbg.inject_pause_active = 0u;
        g_rs_master_dbg.inject_pause_until_ms = 0u;
    }

    /* Ждём RSP ожидаемого addr: иначе POLL соседа наезжает на ответ. */
    if (rs_panel_master_bus_is_quiet(master, now_ms) != 0u) {
        RsBus_RxHarvest(&master->bus);
        /* Harvest мог положить EVT в очередь (OnRsp) — снять в этом же тике. */
        RsPanelV3Master_ProcessDeferred(now_ms);
        g_rs_master_dbg.bus_quiet_skips++;
        (void)rs_panel_master_poll_starving(master, now_ms); /* обновить gap dbg */
        RsPanelMasterDebug_Timer10ms();
        g_rs_master_dbg.menu_selected = g_menu_selected;
        g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
        return;
    }

    /* Hot-plug scan / ACTIVITY / FSM — только если POLL не due и не голодает.
     * Раньше scan шёл даже при due → при 1 панели probe на пустые 2..8
     * + quiet 40 мс съедали шину (poll_tx≪ожидания, quiet_skips≈все тики). */
    if (rs_panel_master_poll_starving(master, now_ms) == 0u) {
        if (rs_panel_master_any_poll_due(master, now_ms) == 0u) {
            if (rs_panel_master_addr_scan_tick(master, now_ms) != 0u) {
                RsPanelMasterDebug_Timer10ms();
                g_rs_master_dbg.menu_selected = g_menu_selected;
                g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
                return;
            }
            if (master->addr_fsm != RS_PANEL_ADDR_FSM_IDLE ||
                master->collision_pending != 0u) {
                if (rs_panel_master_addr_fsm_tick(master, now_ms) != 0u) {
                    rs_panel_master_arm_bus_quiet(now_ms);
                    RsPanelMasterDebug_Timer10ms();
                    g_rs_master_dbg.menu_selected = g_menu_selected;
                    g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
                    return;
                }
            }
            if (rs_panel_master_activity_req_tick(master, now_ms) != 0u) {
                RsPanelMasterDebug_Timer10ms();
                g_rs_master_dbg.menu_selected = g_menu_selected;
                g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
                return;
            }
        }
    } else {
        g_rs_master_dbg.poll_dominate_defer++;
    }

    /* UI-очередь при force_poll обслуживается на следующем свободном тике. */

    rs_panel_master_prune_dead_slots(master);
    rs_panel_master_dbg_sync_rx_decode(master);
    RsPanelMasterDebug_Timer10ms();
    g_rs_master_dbg.panel_caps_valid = master->panels[0].caps_valid;
    g_rs_master_dbg.panel_link_state = (uint8_t)master->panels[0].link_state;
    g_rs_master_dbg.menu_selected = g_menu_selected;
    g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
    {
        uint8_t di;
        g_rs_master_dbg.panel_count = master->panel_count;
        for (di = 0u; di < 8u; di++) {
            g_rs_master_dbg.panel_addr[di] =
                (di < master->panel_count) ? master->panels[di].cfg.addr : 0u;
        }
    }

    if (s_logo_main_nav_deadline_ms != 0u &&
        rs_panel_master_any_panel_ready(master) != 0u &&
        (int32_t)(now_ms - s_logo_main_nav_deadline_ms) >= 0) {
        s_logo_main_nav_deadline_ms = 0u;
        /* Панель сама уходит с logo (400 тиков). Повторный UI_NAV сдвигает виджеты.
         * Не затирать MENU_*: пользователь мог уже открыть меню. */
        if (g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO) {
            g_ui_current_screen_id = RS_PANEL_SCREEN_MAIN;
        }
    }

    /* READY resync: v3 каталог/TIME уже в on_panel_became_ready / Init.
     * Legacy WARN/FIRE/LED UI_DATA не шлём. */
    if (s_panel_ui_resync_pending != 0u &&
        rs_panel_master_any_panel_ready(master) != 0u &&
        s_logo_main_nav_deadline_ms == 0u) {
        s_panel_ui_resync_pending = 0u;
    }


    /* Периодический сброс WARN-кэша раз в 1 с убран: он слал WARN+LED каждую
     * секунду, забивал RS и вместе с коротким watchdog ронял CAPS → duty 1 Гц.
     * Повторная доставка — через became_ready / deliver_fail (PushUiIfChanged). */

    /* CAN→RS mirror: только в свободные тики (POLL не due и не starving). */
    if (s_can_mirror_enable != 0u && s_can_mirror_q_count != 0u &&
        rs_panel_master_poll_starving(master, now_ms) == 0u &&
        rs_panel_master_any_poll_due(master, now_ms) == 0u) {
        if (rs_panel_master_drain_can_mirror(master) != 0u) {
            rs_panel_master_arm_bus_quiet(now_ms);
            return;
        }
    }

    /*
     * Fair RR: не return на панели «не due», пока не просмотрели остальных.
     * Иначе при 2+ панелях один слот голодает POLL → watchdog «авария связи»,
     * второй получает каталог/faults криво.
     */
    {
        uint8_t attempt;
        uint8_t chosen = 0xFFu;
        uint8_t chosen_need_caps = 0u;

        for (attempt = 0u; attempt < master->panel_count; attempt++) {
            uint8_t panel_rr =
                (uint8_t)((master->round_robin_idx + attempt) % master->panel_count);
            PanelState *cand = &master->panels[panel_rr];
            uint8_t poll_due;
            uint8_t need_caps;

            if (cand->cfg.enabled == 0u) {
                continue;
            }

            if ((now_ms - cand->last_rx_ms) > cand->watchdog_ms &&
                cand->last_rx_ms != 0u) {
                cand->link_state = PANEL_LINK_CAPS_PENDING;
                /* v3: не сбрасывать caps_valid после первого v3 POLL —
                 * иначе POLL останавливается на CAPS и SYS не доходит. */
                if (s_v3_poll_seen[panel_rr] == 0u) {
                    cand->caps_valid = 0u;
                }
            }

            /* last_poll_ms==0 (только что ADD) — сразу due, как any_poll_due. */
            poll_due = (cand->last_poll_ms == 0u ||
                        (now_ms - cand->last_poll_ms) >= cand->cfg.poll_ms)
                           ? 1u
                           : 0u;
            need_caps = (cand->link_state == PANEL_LINK_CAPS_PENDING ||
                         cand->caps_valid == 0u) ? 1u : 0u;

            /* v3: POLL даже при CAPS_PENDING (SYS/каталог). */
            if (poll_due != 0u) {
                chosen = panel_rr;
                chosen_need_caps = 0u;
                break;
            }
            if (need_caps != 0u && chosen == 0xFFu) {
                chosen = panel_rr;
                chosen_need_caps = 1u;
            }
        }

        if (chosen == 0xFFu) {
            return;
        }

        master->round_robin_idx =
            (uint8_t)((chosen + 1u) % master->panel_count);
        panel = &master->panels[chosen];

        if (chosen_need_caps != 0u) {
            /* Per-panel CAPS rate (не глобальный s_last_caps_req_ms). */
            if (chosen < RS_PANEL_MAX_PANELS &&
                (s_caps_req_ms[chosen] == 0u ||
                 (now_ms - s_caps_req_ms[chosen]) >= RS_PANEL_CAPS_RETRY_MS)) {
                s_caps_req_ms[chosen] = now_ms;
                s_last_caps_req_ms = now_ms;
                g_rs_master_dbg.caps_req_tx++;
                (void)RsBus_SendFrame(&master->bus,
                                      panel->cfg.addr,
                                      master->next_seq++,
                                      0u,
                                      RS_PANEL_CMD_CAPS_REQ,
                                      0,
                                      0u);
                rs_panel_master_arm_bus_quiet(now_ms);
            }
            return;
        }
    }

    if (panel->ack_wait_active != 0u) {
        /* Legacy UI_DATA ACK — сбросить, v3 POLL не блокировать. */
        panel->ack_wait_active = 0u;
        panel->pending_ack_seq = 0u;
        panel->ack_deadline_ms = 0u;
        panel->pending_ui_stream_len = 0u;
    }

    {

        uint8_t v3_payload[RS_BUS_MAX_WIRE_PAYLOAD];
        uint8_t panel_idx = 0u;
        uint8_t pi;
        for (pi = 0u; pi < master->panel_count; pi++) {
            if (&master->panels[pi] == panel) {
                panel_idx = pi;
                break;
            }
        }
        payload_len = RsPanelV3Master_EncodePollToBuf(v3_payload, sizeof(v3_payload),
                                                      panel_idx, now_ms);
        if (payload_len == 0u) {
            return;
        }
        panel->last_poll_ms = now_ms;
        panel->last_tx_seq = master->next_seq;
        g_rs_master_dbg.poll_req_tx++;
        if (panel_idx < 8u) {
            g_rs_master_dbg.poll_tx_by_panel[panel_idx]++;
            g_rs_master_dbg.poll_gap_ms_by_panel[panel_idx] = 0u;
        }
        (void)RsBus_SendFrame(&master->bus,
                              panel->cfg.addr,
                              master->next_seq++,
                              0u,
                              RS_PANEL_CMD_POLL,
                              v3_payload,
                              payload_len);
        rs_panel_master_arm_bus_quiet(now_ms);
        return;
    }
}

/* Сколько записей просить за раз. В ответ кладутся только те, что влезают целиком. */
#define RS_V3_JOURNAL_BATCH 3u
/* Мин. размер одной записи: rec+ts+code + 3×(len=0). */
#define RS_V3_JOURNAL_ITEM_MIN 13u

static uint8_t rs_journal_field_len(const char *src, uint8_t max_len)
{
    uint8_t n = 0u;

    if (src != 0) {
        while (n < max_len && src[n] != '\0') {
            n++;
        }
    }
    return n;
}

/* rec_idx+ts+code и три counted-строки, уже ограниченные RS_JOURNAL_*_MAX. */
static uint16_t rs_journal_item_wire_size(const EventLogUiLines_t *lines)
{
    uint16_t n = 10u;

    n = (uint16_t)(n + 1u + rs_journal_field_len(lines->header, RS_JOURNAL_HDR_MAX));
    n = (uint16_t)(n + 1u + rs_journal_field_len(lines->title, RS_JOURNAL_TITLE_MAX));
    n = (uint16_t)(n + 1u + rs_journal_field_len(lines->detail, RS_JOURNAL_DETAIL_MAX));
    return n;
}

static uint16_t rs_panel_v3_put_one_journal_item(uint8_t *dst,
                                                uint16_t pos,
                                                uint16_t item_cap,
                                                uint32_t rec_idx)
{
    EventLogRecord_t rec;
    EventLogRecStatus_t st = EVENT_LOG_REC_EMPTY;
    EventLogRecord_t *rec_ptr = 0;
    EventLogUiLines_t lines;
    uint16_t code;
    uint16_t need;
    uint32_t ts = 0u;

    if (dst == 0 || (uint16_t)(pos + RS_V3_JOURNAL_ITEM_MIN) > item_cap) {
        return pos;
    }
    if (EventLogReader_ReadLogical(0u, rec_idx, &st, &rec) && st == EVENT_LOG_REC_VALID) {
        rec_ptr = &rec;
    } else {
        rec.event_code = 0u;
    }
    rs_panel_master_format_journal_lines(rec_ptr, rec_idx, &lines);
    /* Не усекать detail: короткая нижняя строка на панели — это обрезанное имя зоны/МКУ.
     * Максимум одной записи 187 байт, вместе с заголовком списка (13) это ровно 200. */
    need = rs_journal_item_wire_size(&lines);
    if ((uint16_t)(pos + need) > item_cap) {
        return pos;
    }
    code = rec.event_code;

    dst[pos++] = (uint8_t)(rec_idx & 0xFFu);
    dst[pos++] = (uint8_t)((rec_idx >> 8) & 0xFFu);
    dst[pos++] = (uint8_t)((rec_idx >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((rec_idx >> 24) & 0xFFu);
    dst[pos++] = (uint8_t)(ts & 0xFFu);
    dst[pos++] = (uint8_t)((ts >> 8) & 0xFFu);
    dst[pos++] = (uint8_t)((ts >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((ts >> 24) & 0xFFu);
    dst[pos++] = (uint8_t)(code & 0xFFu);
    dst[pos++] = (uint8_t)((code >> 8) & 0xFFu);
    pos = rs_journal_put_counted_str(dst, pos, item_cap, lines.header, RS_JOURNAL_HDR_MAX);
    pos = rs_journal_put_counted_str(dst, pos, item_cap, lines.title, RS_JOURNAL_TITLE_MAX);
    pos = rs_journal_put_counted_str(dst, pos, item_cap, lines.detail, RS_JOURNAL_DETAIL_MAX);
    return pos;
}

/**
 * Упаковать окно журнала: window_first.. пока записи влезают целиком в cap.
 * Неполная запись не пишется: панель доберёт её следующим GET_N.
 * selected в заголовке — g_journal_selected.
 */
static uint16_t rs_panel_v3_put_journal_list_payload(uint8_t *dst, uint16_t cap,
                                                    uint32_t window_first,
                                                    uint8_t n_want)
{
    EventLogTierInfo_t info;
    uint16_t pos = 0u;
    uint32_t total;
    uint8_t n_items = 0u;
    uint8_t i;
    uint8_t n_pos;

    if (dst == 0 || cap < 14u) {
        return 0u;
    }
    if (EventLogReader_GetTierInfo(0u, &info) == false) {
        return 0u;
    }
    total = info.count;
    g_journal_total = total;
    if (total > 0u && g_journal_selected >= total) {
        g_journal_selected = total - 1u;
    }

    if (total == 0u) {
        window_first = 0u;
        n_items = 0u;
    } else {
        if (window_first >= total) {
            window_first = total - 1u;
        }
        if (n_want == 0u) {
            n_want = 1u;
        }
        if (n_want > RS_V3_JOURNAL_BATCH) {
            n_want = RS_V3_JOURNAL_BATCH;
        }
        {
            uint32_t avail = total - window_first;
            n_items = n_want;
            if ((uint32_t)n_items > avail) {
                n_items = (uint8_t)avail;
            }
        }
    }

    dst[pos++] = (uint8_t)(total & 0xFFu);
    dst[pos++] = (uint8_t)((total >> 8) & 0xFFu);
    dst[pos++] = (uint8_t)((total >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((total >> 24) & 0xFFu);
    dst[pos++] = (uint8_t)(g_journal_selected & 0xFFu);
    dst[pos++] = (uint8_t)((g_journal_selected >> 8) & 0xFFu);
    dst[pos++] = (uint8_t)((g_journal_selected >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((g_journal_selected >> 24) & 0xFFu);
    dst[pos++] = (uint8_t)(window_first & 0xFFu);
    dst[pos++] = (uint8_t)((window_first >> 8) & 0xFFu);
    dst[pos++] = (uint8_t)((window_first >> 16) & 0xFFu);
    dst[pos++] = (uint8_t)((window_first >> 24) & 0xFFu);
    n_pos = (uint8_t)pos;
    dst[pos++] = n_items;

    {
        uint8_t written = 0u;

        for (i = 0u; i < n_items; i++) {
            uint32_t rec_idx = window_first + (uint32_t)i;
            uint16_t next = rs_panel_v3_put_one_journal_item(dst, pos, cap, rec_idx);

            if (next == pos) {
                break;
            }
            pos = next;
            written++;
        }
        dst[n_pos] = written;
    }
    return pos;
}

uint8_t RsPanelV3Proto_DispatchDataEvent(uint8_t panel_idx,
                                         const RsPanelV3Event *ev,
                                         RsPanelV3EventReply *reply)
{
    /* Cursor журнала — per-panel (общий g_journal_selected ломал 2-ю панель). */
    uint32_t *jsel;
    if (ev == 0 || reply == 0) {
        return 0u;
    }
    if (panel_idx >= RS_PANEL_MAX_PANELS) {
        panel_idx = 0u;
    }
    jsel = &s_v3_journal_selected[panel_idx];
    memset(reply, 0, sizeof(*reply));

    /* ПО читает журнал по USART2 — не конкурировать со SPI Flash панели. */
    if ((ev->type == (uint8_t)RS_PANEL_V3_EVT_JOURNAL_COUNT ||
         ev->type == (uint8_t)RS_PANEL_V3_EVT_JOURNAL_GET ||
         ev->type == (uint8_t)RS_PANEL_V3_EVT_JOURNAL_GET_N) &&
        LogStream_IsPanelJournalBlocked()) {
        return 0u;
    }

    switch (ev->type) {
    case RS_PANEL_V3_EVT_JOURNAL_COUNT: {
        EventLogTierInfo_t info;
        if (EventLogReader_GetTierInfo(0u, &info) == false) {
            return 0u;
        }
        g_journal_total = info.count;
        if (reply->payload_len + 4u > RS_PANEL_V3_EVENT_REPLY_MAX) {
            return 0u;
        }
        reply->payload[0] = (uint8_t)(info.count & 0xFFu);
        reply->payload[1] = (uint8_t)((info.count >> 8) & 0xFFu);
        reply->payload[2] = (uint8_t)((info.count >> 16) & 0xFFu);
        reply->payload[3] = (uint8_t)((info.count >> 24) & 0xFFu);
        reply->payload_len = 4u;
        return 1u;
    }
    case RS_PANEL_V3_EVT_JOURNAL_GET: {
        /* Одна запись. u8_a: 0/1 nav, 2=новейшая, 3=refresh; иначе u16_a=index. */
        EventLogTierInfo_t jinfo;
        if (EventLogReader_GetTierInfo(0u, &jinfo) != false) {
            g_journal_total = jinfo.count;
        }
        if (g_journal_total > 0u) {
            if (ev->u8_a == 0u && (*jsel + 1u) < g_journal_total) {
                (*jsel)++;
            } else if (ev->u8_a == 1u && *jsel > 0u) {
                (*jsel)--;
            } else if (ev->u8_a == 2u) {
                *jsel = g_journal_total - 1u;
            } else if (ev->u8_a == 3u) {
                /* refresh текущей */
            } else if ((uint32_t)ev->u16_a < g_journal_total) {
                *jsel = (uint32_t)ev->u16_a;
            }
        } else {
            *jsel = 0u;
        }
        g_journal_selected = *jsel; /* legacy UI_DATA path */
        reply->payload_len = rs_panel_v3_put_journal_list_payload(
            reply->payload, RS_PANEL_V3_EVENT_REPLY_MAX, *jsel, 1u);
        return (reply->payload_len > 0u) ? 1u : 0u;
    }
    case RS_PANEL_V3_EVT_JOURNAL_GET_N: {
        /* Протокол: u16_a = index (начало окна), u16_b = n. */
        EventLogTierInfo_t jinfo;
        uint32_t idx;
        uint8_t n;

        if (EventLogReader_GetTierInfo(0u, &jinfo) != false) {
            g_journal_total = jinfo.count;
        }
        if (g_journal_total == 0u) {
            *jsel = 0u;
            reply->payload_len = rs_panel_v3_put_journal_list_payload(
                reply->payload, RS_PANEL_V3_EVENT_REPLY_MAX, 0u, 0u);
            return (reply->payload_len > 0u) ? 1u : 0u;
        }
        idx = (uint32_t)ev->u16_a;
        if (idx >= g_journal_total) {
            idx = g_journal_total - 1u;
        }
        n = (uint8_t)ev->u16_b;
        if (n == 0u) {
            n = 1u;
        }
        /* selected панели не трогаем — GET_N только догрузка окна. */
        reply->payload_len = rs_panel_v3_put_journal_list_payload(
            reply->payload, RS_PANEL_V3_EVENT_REPLY_MAX, idx, n);
        return (reply->payload_len > 0u) ? 1u : 0u;
    }
    case RS_PANEL_V3_EVT_DEVICES_COUNT: {
        uint8_t slots[MAX_MCU_IN_BUS];
        uint8_t count = rs_panel_master_collect_mcu_slots(slots, MAX_MCU_IN_BUS);
        reply->payload[0] = count;
        reply->payload_len = 1u;
        return 1u;
    }
    case RS_PANEL_V3_EVT_ZONE_BLOCK_LIST: {
        uint8_t zi;
        uint8_t n = 0u;
        for (zi = 0u; zi < ZONE_NUMBER && (uint16_t)(1u + n * 2u) < RS_PANEL_V3_EVENT_REPLY_MAX; zi++) {
            if (PPKYConfig.zone_name[zi][0] == 0) {
                continue;
            }
            reply->payload[1u + n * 2u] = (uint8_t)(zi + 1u);
            reply->payload[2u + n * 2u] = PPKY_ZoneFireModeGet(zi);
            n++;
        }
        reply->payload[0] = n;
        reply->payload_len = (uint16_t)(1u + (uint16_t)n * 2u);
        return 1u;
    }
    case RS_PANEL_V3_EVT_ZONE_BLOCK_SET: {
        uint8_t zone = ev->zone;
        uint8_t mode = ev->u8_a;
        if (Fire_IsActive() != 0u) {
            return 0u; /* во время пожара режим зон не меняется */
        }
        /* mode: 0 авто / 2 ручной / 3 пуск блок / 4 пожар выкл */
        if (zone >= 1u && zone <= ZONE_NUMBER &&
            (mode == 0u || mode == 2u || mode == 3u || mode == 4u)) {
            PPKY_ZoneFireModeSet((uint8_t)(zone - 1u), mode);
            reply->payload[0] = zone;
            reply->payload[1] = mode;
            reply->payload_len = 2u;
            return 1u;
        }
        return 0u;
    }
    case RS_PANEL_V3_EVT_EXT_CAN_SET: {
        RsPanelMaster_SetCanMirrorEnable(ev->u8_a != 0u);
        reply->payload[0] = (ev->u8_a != 0u) ? 1u : 0u;
        reply->payload_len = 1u;
        return 1u;
    }
    case RS_PANEL_V3_EVT_DEVICES_GET:
    case RS_PANEL_V3_EVT_DEVICES_GET_N: {
        uint8_t slots[MAX_MCU_IN_BUS];
        uint8_t count = rs_panel_master_collect_mcu_slots(slots, MAX_MCU_IN_BUS);
        uint8_t idx = (uint8_t)(ev->u16_a & 0xFFu);
        reply->payload[0] = count;
        if (idx < count) {
            reply->payload[1] = slots[idx];
            reply->payload_len = 2u;
        } else {
            reply->payload_len = 1u;
        }
        return 1u;
    }
    default:
        break;
    }
    return 0u;
}

uint8_t RsPanelMaster_InjectRawRsFrame(const uint8_t *frame, uint16_t frame_len)
{
    RsBusFrameView view;
    uint16_t consumed = 0u;
    uint32_t primask;

    if (g_active_master == 0 || frame == 0 || frame_len == 0u ||
        frame_len > ESP_UART_BODY_MAX) {
        return 0u;
    }
    if (RsBus_FrameDecode(frame, frame_len, &view, &consumed) == 0u) {
        return 0u;
    }
    /* Только master→slave (DIR=0): команды ПО на панель/бутлоадер. */
    if ((view.flags & RS_BUS_FLAG_DIR) != 0u) {
        return 0u;
    }
    /* WiFi enable — локально на ППКУ, на шину панелей не кладём. */
    if (view.cmd == RS_PANEL_CMD_PPKY_WIFI_ENABLE) {
        EspManager_RequestWifiEnable();
        return 1u;
    }
    /* CAN mirror set — локально, на шину не кладём. */
    if (view.cmd == RS_PANEL_CMD_PPKY_CAN_MIRROR_SET) {
        uint8_t en = 1u;
        if (view.payload_len > 0u && view.payload != 0) {
            en = (view.payload[0] != 0u) ? 1u : 0u;
        }
        RsPanelMaster_SetCanMirrorEnable(en);
        return 1u;
    }
    /* CAN→bus: локально на CAN, на RS повторно не кладём. */
    if (view.cmd == RS_PANEL_CMD_CAN_TO_BUS) {
        return CanHostTxFromBsu(view.payload, view.payload_len);
    }
    if (rs_panel_inject_cmd_allowed(view.cmd) == 0u) {
        return 0u;
    }

    /* Не TX из IRQ UART2 — только постановка в очередь Process10ms. */
    primask = __get_PRIMASK();
    __disable_irq();
    if (s_inject_q_count >= RS_PANEL_INJECT_Q_DEPTH) {
        __set_PRIMASK(primask);
        return 0u;
    }
    memcpy(s_inject_q[s_inject_q_tail].data, frame, frame_len);
    s_inject_q[s_inject_q_tail].len = frame_len;
    s_inject_q_tail = (uint8_t)((s_inject_q_tail + 1u) % RS_PANEL_INJECT_Q_DEPTH);
    s_inject_q_count++;
    /* Не ставить 45 с паузу здесь: реальная cmd → extend только в process_pending. */
    __set_PRIMASK(primask);
    return 1u;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    /* USART1 RS: Receive_DMA без IDLE — сюда не попадаем.
     * Оставляем пустым на случай чужого UART / старого вызова. */
    (void)huart;
    (void)size;
}

void RsPanelMaster_OnUartError(UART_HandleTypeDef *huart)
{
    if (g_active_master == 0 || huart == 0) {
        return;
    }
    if (huart != g_active_master->bus.uart) {
        return;
    }
    RsBus_RecoverFromError(&g_active_master->bus);
}

void RsPanelMaster_OnRxDmaComplete(void)
{
    if (g_active_master == 0) {
        return;
    }
    RsBus_OnRxDmaComplete(&g_active_master->bus);
}
