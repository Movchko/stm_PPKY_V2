#include "rs_panel_proto.h"
#include "rs_panel_master_debug.h"

#include <stdio.h>
#include <string.h>

#include "device_config.h"
#include "config_zone_block.h"
#include "config_ign_block_sync.h"
#include "gost_mode.h"
#include "event_log.h"
#include "event_log_reader.h"
#include "event_logger.h"
#include "event_log_ui.h"
#include "menu_ui.h"
#include "esp_manager.h"
#include "esp_protocol.h"
#include "can_bus.h"
#include "led.h"
#include "beeper.h"
#include "warning.h"
#include "fire.h"

extern PPKYCfg PPKYConfig;
extern void SaveConfig(void);

static RsPanelMaster *g_active_master = 0;
static uint16_t g_esp_uart_fwd_seq = 1u;
static uint8_t g_rs_frag_id = 1u;
static uint32_t g_journal_total = 0u;
static uint32_t g_journal_selected = 0u;
static uint32_t g_journal_window_first = 0u;
static uint8_t g_journal_window_size = 0u;
static uint8_t g_journal_detail_open = 0u;

/* UI session state:
 * panel tells events via RSP_POLL.ui_events, master keeps which screen is active
 * (based on the last UI_NAV we sent). */
static uint16_t g_ui_current_screen_id = RS_PANEL_SCREEN_LOGO;
/* Как в stm_PPKY v1 / панели: 4000 мс (400×10 мс). */
#define RS_PANEL_LOGO_MAIN_DELAY_MS 4000u
static uint32_t s_logo_main_nav_deadline_ms = 0u;
static uint8_t s_panel_ui_resync_pending = 0u;

static uint16_t g_menu_selected = 0u;
#if GOST_MODE
static uint8_t g_menu_n_items = 6u;
#else
static uint8_t g_menu_n_items = 7u;
#endif
static uint8_t g_device_selected_slot = 0xFFu;

static uint8_t rs_panel_master_is_menu_ui_screen(uint16_t screen_id)
{
    return (screen_id == RS_PANEL_SCREEN_MENU_ROOT ||
            screen_id == RS_PANEL_SCREEN_MENU_SETTINGS ||
            screen_id == RS_PANEL_SCREEN_MENU_DEVICES ||
            screen_id == RS_PANEL_SCREEN_MENU_DEVICE_DETAIL ||
            screen_id == RS_PANEL_SCREEN_MENU_CONFIG ||
            screen_id == RS_PANEL_SCREEN_MENU_JOURNAL ||
            screen_id == RS_PANEL_SCREEN_MENU_JOURNAL_DETAIL ||
            screen_id == RS_PANEL_SCREEN_MENU_CONNECTION ||
            screen_id == RS_PANEL_SCREEN_MENU_SOUND ||
            screen_id == RS_PANEL_SCREEN_MENU_BLOCK_ZONE) ? 1u : 0u;
}

/* Панель уже в корневом меню, а мастер ещё держит LOGO/MAIN (дедлайн логотипа / reconnect). */
static uint8_t rs_panel_master_is_menu_root_session(void)
{
    return (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_ROOT) ? 1u : 0u;
}

/* BACK/MENU_SELECT: принять и если мастер отстал на MAIN/LOGO после открытия меню панелью. */
static uint8_t rs_panel_master_accept_menu_root_evt(void)
{
    return (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_ROOT ||
            g_ui_current_screen_id == RS_PANEL_SCREEN_MAIN ||
            g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO) ? 1u : 0u;
}

#define RS_PANEL_CAPS_RETRY_MS 50u
static uint32_t s_last_caps_req_ms = 0u;
static uint8_t g_block_zone_selected = 0u;
static uint8_t g_connection_selected = 0u;

/* UI TX нельзя делать из HAL_UARTEx_RxEventCallback: AbortReceive+Transmit
 * на half-duplex ломает DMA/RX. Очередь → Process10ms (main). */
#define RS_PANEL_UI_EVT_Q_DEPTH 4u
typedef struct {
    uint8_t panel_idx;
    RsPanelPollRsp rsp;
} RsPanelUiEvtQItem;
static RsPanelUiEvtQItem s_ui_evt_q[RS_PANEL_UI_EVT_Q_DEPTH];
static volatile uint8_t s_ui_evt_q_head = 0u;
static volatile uint8_t s_ui_evt_q_tail = 0u;
static volatile uint8_t s_ui_evt_q_count = 0u;

static void rs_panel_master_handle_ui_events(RsPanelMaster *master,
                                             PanelState *panel,
                                             const RsPanelPollRsp *rsp);

static uint8_t rs_panel_master_ui_evt_enqueue(uint8_t panel_idx, const RsPanelPollRsp *rsp)
{
    uint32_t primask;

    if (rsp == 0) {
        return 0u;
    }
    /* Пустой POLL и RELEASE без UI — не занимаем очередь (CONFIRM не должен вытесняться). */
    if (rsp->ui_evt_count == 0u) {
        uint8_t press = 0u;
        uint8_t i;
        for (i = 0u; i < rsp->evt_count && i < RS_PANEL_MAX_POLL_BTN_EVENTS; i++) {
            if (rsp->btn_events[i].state == (uint8_t)RS_PANEL_BUTTON_PRESS) {
                press = 1u;
                break;
            }
        }
        if (press == 0u) {
            return 0u;
        }
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_ui_evt_q_count >= RS_PANEL_UI_EVT_Q_DEPTH) {
        __set_PRIMASK(primask);
        g_rs_master_dbg.ui_evt_q_overflow++;
        return 0u;
    }
    s_ui_evt_q[s_ui_evt_q_tail].panel_idx = panel_idx;
    s_ui_evt_q[s_ui_evt_q_tail].rsp = *rsp;
    s_ui_evt_q_tail = (uint8_t)((s_ui_evt_q_tail + 1u) % RS_PANEL_UI_EVT_Q_DEPTH);
    s_ui_evt_q_count++;
    __set_PRIMASK(primask);

    g_rs_master_dbg.ui_evt_enqueued++;
    if (rsp->ui_evt_count != 0u) {
        g_rs_master_dbg.last_ui_evt_type = rsp->ui_events[0].evt_type;
        g_rs_master_dbg.last_ui_evt_p1 = rsp->ui_events[0].p1;
    }
    return 1u;
}

static void rs_panel_master_ui_evt_process_pending(RsPanelMaster *master)
{
    while (1) {
        RsPanelUiEvtQItem item;
        uint32_t primask;
        PanelState *panel;

        primask = __get_PRIMASK();
        __disable_irq();
        if (s_ui_evt_q_count == 0u) {
            __set_PRIMASK(primask);
            break;
        }
        item = s_ui_evt_q[s_ui_evt_q_head];
        s_ui_evt_q_head = (uint8_t)((s_ui_evt_q_head + 1u) % RS_PANEL_UI_EVT_Q_DEPTH);
        s_ui_evt_q_count--;
        __set_PRIMASK(primask);

        if (master == 0 || item.panel_idx >= master->panel_count) {
            continue;
        }
        panel = &master->panels[item.panel_idx];
        rs_panel_master_handle_ui_events(master, panel, &item.rsp);
        g_rs_master_dbg.ui_evt_handled++;
        g_rs_master_dbg.menu_selected = g_menu_selected;
        g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
    }
}

static void rs_panel_master_update_journal_fault_mask(void)
{
    uint8_t mask = 0u;

    if (g_active_master != 0) {
        for (uint8_t i = 0u; i < g_active_master->panel_count && i < 8u; i++) {
            const PanelState *panel = &g_active_master->panels[i];
            if (panel->cfg.enabled == 0u) {
                continue;
            }
            if (panel->journal_fault_latched != 0u && panel->cfg.addr >= 1u && panel->cfg.addr <= 8u) {
                mask = (uint8_t)(mask | (uint8_t)(1u << (panel->cfg.addr - 1u)));
            }
        }
    }

    Warning_SetPanelJournalFaultMask(mask);
}

static void rs_panel_master_log_journal_link(uint8_t panel_addr, uint8_t recovered)
{
    EventLogPayload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.additional[0] = panel_addr;
    payload.additional[1] = (recovered != 0u) ? 1u : 0u;
    (void)EventLog_Post(EVENT_LOG_HOST_LINK, &payload);
}

/*
 * PPCU-side: передаём “нажатые” кнопки с панели в общий Button FSM.
 * В `button.c` эта функция объявлена как weak, поэтому сильная реализация здесь
 * автоматически перехватывает remote-кнопки.
 */
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

static void rs_panel_master_send_stream_to_panel(RsPanelMaster *master,
                                                 PanelState *panel,
                                                 const uint8_t *stream,
                                                 uint16_t stream_len,
                                                 uint8_t force_frag,
                                                 uint8_t request_ack)
{
    if (master == 0 || panel == 0 || stream == 0 || stream_len == 0u) {
        return;
    }

    if (force_frag == 0u && stream_len <= RS_BUS_MAX_WIRE_PAYLOAD) {
        uint8_t seq = master->next_seq++;
        uint8_t flags = request_ack ? RS_BUS_FLAG_ACK_REQ : 0u;
        (void)RsBus_SendFrame(&master->bus,
                              panel->cfg.addr,
                              seq,
                              flags,
                              RS_PANEL_CMD_UI_DATA,
                              stream,
                              stream_len);
        if (request_ack != 0u) {
            panel->pending_ack_seq = seq;
            panel->ack_wait_active = 1u;
            panel->ack_retries_left = RS_PANEL_ACK_RETRIES;
            panel->ack_deadline_ms = HAL_GetTick() + RS_PANEL_ACK_TIMEOUT_MS;
            panel->pending_ui_stream_len = stream_len;
            memcpy(panel->pending_ui_stream, stream, stream_len);
        }
        return;
    }

    {
        uint8_t frag_id = g_rs_frag_id++;
        uint16_t frag_data_capacity = (uint16_t)(RS_BUS_MAX_WIRE_PAYLOAD - 3u);
        uint8_t frag_total = (uint8_t)((stream_len + frag_data_capacity - 1u) / frag_data_capacity);
        uint16_t stream_off = 0u;
        uint8_t last_seq = 0u;

        for (uint8_t frag_idx = 0u; frag_idx < frag_total; frag_idx++) {
            uint8_t payload[RS_BUS_MAX_PAYLOAD];
            uint16_t pos = 0u;
            uint16_t chunk_len = stream_len - stream_off;
            if (chunk_len > frag_data_capacity) {
                chunk_len = frag_data_capacity;
            }

            payload[pos++] = frag_id;
            payload[pos++] = frag_idx;
            payload[pos++] = frag_total;
            memcpy(&payload[pos], &stream[stream_off], chunk_len);
            pos = (uint16_t)(pos + chunk_len);

            uint8_t flags = RS_BUS_FLAG_FRAG;
            if ((uint8_t)(frag_idx + 1u) < frag_total) {
                flags |= RS_BUS_FLAG_MORE;
            } else if (request_ack != 0u) {
                flags |= RS_BUS_FLAG_ACK_REQ;
            }

            last_seq = master->next_seq++;
            (void)RsBus_SendFrame(&master->bus,
                                  panel->cfg.addr,
                                  last_seq,
                                  flags,
                                  RS_PANEL_CMD_UI_DATA,
                                  payload,
                                  pos);

            stream_off = (uint16_t)(stream_off + chunk_len);
        }

        if (request_ack != 0u) {
            panel->pending_ack_seq = last_seq;
            panel->ack_wait_active = 1u;
            panel->ack_retries_left = RS_PANEL_ACK_RETRIES;
            panel->ack_deadline_ms = HAL_GetTick() + RS_PANEL_ACK_TIMEOUT_MS;
            panel->pending_ui_stream_len = stream_len;
            memcpy(panel->pending_ui_stream, stream, stream_len);
        }
    }
}

static void rs_panel_master_send_ui_nav_to_ready_panels(RsPanelMaster *master,
                                                         uint16_t screen_id,
                                                         uint8_t action)
{
    if (master == 0u) {
        return;
    }

    g_ui_current_screen_id = screen_id;

    uint8_t payload[5u];
    RsPanelUiNavCmd nav_cmd = {
        .screen_id = screen_id,
        .action = action,
        .param = 0u,
    };
    uint16_t payload_len = RsPanel_EncodeUiNavCmd(payload, sizeof(payload), &nav_cmd);
    if (payload_len == 0u) {
        return;
    }

    for (uint8_t i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }
        (void)RsBus_SendFrame(&master->bus,
                               panel->cfg.addr,
                               master->next_seq++,
                               0u, /* flags: master->panel (no DIR) */
                               RS_PANEL_CMD_UI_NAV,
                               payload,
                               payload_len);
    }
}

/* WARN/FIRE не должны срывать меню. После логотипа панель сама уходит на MAIN;
 * повторный UI_NAV MAIN сдвигает виджеты, поэтому здесь только синхронизируем
 * локальный id. */
static void rs_panel_master_ensure_main_screen(RsPanelMaster *master)
{
    if (master == 0u || s_logo_main_nav_deadline_ms != 0u) {
        return;
    }
    if (g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO) {
        g_ui_current_screen_id = RS_PANEL_SCREEN_MAIN;
    }
}

/* forward decl: используется внутри MENU_LIST */
static uint8_t rs_panel_master_send_ui_data_to_ready_panels(RsPanelMaster *master,
                                                           uint8_t sub_id,
                                                           const uint8_t *data,
                                                           uint16_t data_len);

static void rs_panel_master_send_menu_list_to_ready_panels(RsPanelMaster *master,
                                                             uint16_t selected,
                                                             uint8_t n_items)
{
    if (master == 0u || n_items == 0u) {
        return;
    }

    /* Format per RS_PANEL_PROTOCOL.md (MENU_LIST):
     *   selected u16 + n_items u8 + items[n_items] where each item:
     *     len u8 + utf8[len]
     * For now we send empty labels (panel currently only needs selection+count). */
    uint8_t payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;

    payload[pos++] = (uint8_t)(selected & 0xFFu);
    payload[pos++] = (uint8_t)((selected >> 8) & 0xFFu);
    payload[pos++] = n_items;

    for (uint8_t i = 0u; i < n_items; i++) {
        payload[pos++] = 0u; /* label length = 0 */
        if (pos >= RS_BUS_MAX_PAYLOAD) {
            break;
        }
    }

    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_MENU_LIST,
                                                 payload,
                                                 pos);
    g_rs_master_dbg.menu_list_tx++;
}

static void rs_panel_master_send_menu_state_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    {
        uint8_t payload[2u];
        payload[0] = 0u; /* item_id: fire_mode */
        payload[1] = PPKYConfig.fire_mode;
        rs_panel_master_send_ui_data_to_ready_panels(master,
                                                     RS_PANEL_UI_DATA_MENU_VALUE,
                                                     payload,
                                                     sizeof(payload));
    }

    {
        uint8_t payload[3u];
        payload[0] = 1u; /* item_id: sound */
        payload[1] = (PPKYConfig.beep != 0u) ? 1u : 0u;
        payload[2] = (PPKYConfig.beep_block != 0u) ? 1u : 0u;
        rs_panel_master_send_ui_data_to_ready_panels(master,
                                                     RS_PANEL_UI_DATA_MENU_TOGGLE,
                                                     payload,
                                                     sizeof(payload));
    }
}

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

static void rs_panel_master_send_device_list_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    uint8_t slots[MAX_MCU_IN_BUS];
    uint8_t count = rs_panel_master_collect_mcu_slots(slots, MAX_MCU_IN_BUS);
    uint8_t selected_slot = 0xFFu;

    if (count != 0u) {
        selected_slot = slots[0];
        for (uint8_t i = 0u; i < count; i++) {
            if (slots[i] == g_device_selected_slot) {
                selected_slot = g_device_selected_slot;
                break;
            }
        }
    }
    g_device_selected_slot = selected_slot;

    uint8_t payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;
    payload[pos++] = selected_slot;
    payload[pos++] = count;

    for (uint8_t i = 0u; i < count; i++) {
        const Device *dev = &PPKYConfig.CfgDevices[slots[i]].UId.devId;
        if ((uint16_t)(pos + 4u + 1u) > RS_BUS_MAX_PAYLOAD) {
            break;
        }
        payload[pos++] = slots[i];
        payload[pos++] = dev->zone;
        payload[pos++] = dev->d_type;
        payload[pos++] = dev->h_adr;
    }

    uint8_t zone_name_len = 0u;
    if (selected_slot < MAX_MCU_IN_BUS) {
        uint8_t zone = PPKYConfig.CfgDevices[selected_slot].UId.devId.zone;
        if (zone >= 1u && zone <= ZONE_NUMBER) {
            zone_name_len = (uint8_t)strnlen((const char *)PPKYConfig.zone_name[zone - 1u], ZONE_NAME_SIZE);
        }
    }
    payload[pos++] = zone_name_len;
    if (zone_name_len != 0u) {
        memcpy(&payload[pos], PPKYConfig.zone_name[PPKYConfig.CfgDevices[selected_slot].UId.devId.zone - 1u], zone_name_len);
        pos = (uint16_t)(pos + zone_name_len);
    }

    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_DEVICE_LIST,
                                                 payload,
                                                 pos);
}

static void rs_panel_master_send_device_detail_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u || g_device_selected_slot >= MAX_MCU_IN_BUS) {
        return;
    }

    const MKUCfg *mku = &PPKYConfig.CfgDevices[g_device_selected_slot];
    const Device *dev = &mku->UId.devId;
    uint8_t payload[1u + 1u + 1u + 1u + 4u + 4u + 4u + 1u + ZONE_NAME_SIZE];
    uint16_t pos = 0u;
    payload[pos++] = g_device_selected_slot;
    payload[pos++] = dev->zone;
    payload[pos++] = dev->d_type;
    payload[pos++] = dev->h_adr;
    pos += rs_put_u32le(&payload[pos], mku->UId.UId0);
    pos += rs_put_u32le(&payload[pos], mku->UId.UId1);
    pos += rs_put_u32le(&payload[pos], mku->UId.UId2);

    uint8_t zone_name_len = 0u;
    if (dev->zone >= 1u && dev->zone <= ZONE_NUMBER) {
        zone_name_len = (uint8_t)strnlen((const char *)PPKYConfig.zone_name[dev->zone - 1u], ZONE_NAME_SIZE);
    }
    payload[pos++] = zone_name_len;
    if (zone_name_len != 0u) {
        memcpy(&payload[pos], PPKYConfig.zone_name[dev->zone - 1u], zone_name_len);
        pos = (uint16_t)(pos + zone_name_len);
    }

    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_DEVICE_DETAIL,
                                                 payload,
                                                 pos);
}

static void rs_panel_master_send_zone_mode_list_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    uint8_t payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;
    uint8_t count = 0u;
    uint8_t selected_zone = 0xFFu;

    for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
        if (PPKYConfig.zone_name[zi][0] == 0) {
            continue;
        }
        if (selected_zone == 0xFFu) {
            selected_zone = zi;
        }
        if (zi == g_block_zone_selected) {
            selected_zone = zi;
        }
    }

    if (selected_zone == 0xFFu) {
        selected_zone = 0u;
    }
    g_block_zone_selected = selected_zone;

    payload[pos++] = selected_zone;
    payload[pos++] = 0u; /* count, fill later */

    for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
        if (PPKYConfig.zone_name[zi][0] == 0) {
            continue;
        }

        uint8_t name_len = (uint8_t)strnlen((const char *)PPKYConfig.zone_name[zi], ZONE_NAME_SIZE);
        if ((uint16_t)(pos + 3u + name_len) > RS_BUS_MAX_PAYLOAD) {
            break;
        }

        payload[pos++] = zi;
        payload[pos++] = PPKY_ZoneFireModeGet(zi);
        payload[pos++] = name_len;
        if (name_len != 0u) {
            memcpy(&payload[pos], PPKYConfig.zone_name[zi], name_len);
            pos = (uint16_t)(pos + name_len);
        }
        count++;
    }

    payload[1] = count;
    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_ZONE_MODE_LIST,
                                                 payload,
                                                 pos);
}

static void rs_panel_master_send_connection_status_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    /* [selected][wifi_block][esp_en][online][host][session][wifi_on][rs485_on] */
    uint8_t payload[8u];
    payload[0] = g_connection_selected;
    payload[1] = (PPKYConfig.wifi_block != 0u) ? 1u : 0u;
    payload[2] = (Esp32_IsEnabled() != 0u) ? 1u : 0u;
    payload[3] = (EspManager_IsOnline() != 0u) ? 1u : 0u;
    payload[4] = (EspManager_IsHostConnected() != 0u) ? 1u : 0u;
    payload[5] = (EspManager_IsWifiSessionActive() != 0u) ? 1u : 0u;
    payload[6] = (EspManager_IsUserWifiOn() != 0u) ? 1u : 0u;
    payload[7] = (PPKYConfig.rs485_on != 0u) ? 1u : 0u;
    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_CONNECTION_STATUS,
                                                 payload,
                                                 sizeof(payload));
}

static void rs_panel_master_send_config_status_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    uint8_t payload[2u];
    payload[0] = (uint8_t)MenuConfig_GetState();
    payload[1] = MenuConfig_GetPercent();
    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_CONFIG_STATUS,
                                                 payload,
                                                 sizeof(payload));
}

static uint8_t rs_panel_master_send_ui_data_to_ready_panels(RsPanelMaster *master,
                                                           uint8_t sub_id,
                                                           const uint8_t *data,
                                                           uint16_t data_len)
{
    uint8_t delivered = 0u;

    if (master == 0u || data == 0u || data_len == 0u) {
        return 0u;
    }

    for (uint8_t i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }
        /* Не слать WARN/FIRE поверх кадра, который ещё ждёт ACK — иначе CRC на панели. */
        if (panel->ack_wait_active != 0u) {
            continue;
        }

        uint8_t stream[RS_BUS_MAX_PAYLOAD];
        uint16_t stream_len = 0u;
        uint8_t force_frag = (sub_id == RS_PANEL_UI_DATA_JOURNAL_DETAIL && data_len > 160u) ? 1u : 0u;
        uint8_t request_ack = (sub_id == RS_PANEL_UI_DATA_JOURNAL_DETAIL && force_frag != 0u) ? 1u : 0u;

        if ((uint16_t)(1u + data_len) > RS_BUS_MAX_PAYLOAD) {
            continue;
        }

        stream[stream_len++] = sub_id;
        memcpy(&stream[stream_len], data, data_len);
        stream_len = (uint16_t)(stream_len + data_len);

        rs_panel_master_send_stream_to_panel(master,
                                             panel,
                                             stream,
                                             stream_len,
                                             force_frag,
                                             request_ack);
        delivered = 1u;
    }

    return delivered;
}

static uint8_t rs_panel_master_pick_common_journal_lines(const RsPanelMaster *master)
{
    if (master == 0u) {
        return 0u;
    }

    uint8_t min_lines = 0xFFu;
    uint8_t any = 0u;

    for (uint8_t i = 0u; i < master->panel_count; i++) {
        const PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }

        uint8_t jl = panel->caps.journal_lines;
        if (jl == 0u) {
            jl = 1u;
        }
        if (jl < min_lines) {
            min_lines = jl;
        }
        any = 1u;
    }

    if (any == 0u) {
        return 0u;
    }

    /* Защита по размеру payload: на 512 байт влезает порядка 8 строк. */
    if (min_lines > 8u) {
        min_lines = 8u;
    }
    return min_lines;
}

static void rs_panel_master_format_journal_short(const EventLogRecord_t *rec,
                                                 uint32_t rec_idx,
                                                 char *dst,
                                                 size_t dst_size)
{
    EventLogUiLines_t lines;
    if (dst == 0 || dst_size == 0u) {
        return;
    }
    if (rec == 0) {
        dst[0] = '\0';
        return;
    }
    EventLogUi_FormatRecord(rec, rec_idx + 1u, g_journal_total, &lines);
    (void)snprintf(dst, dst_size, "%s", lines.title);
}

static void rs_panel_master_format_journal_detail(const EventLogRecord_t *rec,
                                                  uint32_t rec_idx,
                                                  char *dst,
                                                  size_t dst_size)
{
    EventLogUiLines_t lines;
    if (dst == 0 || dst_size == 0u) {
        return;
    }
    if (rec == 0) {
        dst[0] = '\0';
        return;
    }
    EventLogUi_FormatRecord(rec, rec_idx + 1u, g_journal_total, &lines);
    (void)snprintf(dst, dst_size, "%s | %s", lines.title, lines.detail);
}

static void rs_panel_master_normalize_journal_window(uint32_t total, uint8_t window_size)
{
    if (window_size == 0u) {
        g_journal_window_first = 0u;
        g_journal_selected = 0u;
        return;
    }

    if (total == 0u) {
        g_journal_selected = 0u;
        g_journal_window_first = 0u;
        return;
    }

    if (g_journal_selected >= total) {
        g_journal_selected = total - 1u;
    }
    if (g_journal_window_first > g_journal_selected) {
        g_journal_window_first = g_journal_selected;
    }
    if ((uint32_t)(g_journal_window_first + window_size) <= g_journal_selected) {
        g_journal_window_first = g_journal_selected - (uint32_t)window_size + 1u;
    }
    if (g_journal_window_first >= total) {
        g_journal_window_first = total - 1u;
    }
}

static void rs_panel_master_send_journal_detail_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u || g_journal_total == 0u) {
        return;
    }

    EventLogRecord_t rec;
    EventLogRecStatus_t st = EVENT_LOG_REC_EMPTY;
    EventLogRecord_t *rec_ptr = 0;
    char full_text[96];
    uint16_t code = 0u;

    if (EventLogReader_ReadLogical(0u, g_journal_selected, &st, &rec) && st == EVENT_LOG_REC_VALID) {
        rec_ptr = &rec;
        code = rec.event_code;
    }

    rs_panel_master_format_journal_detail(rec_ptr, g_journal_selected, full_text, sizeof(full_text));

    uint8_t ui_payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;
    uint16_t text_len = (uint16_t)strnlen(full_text, sizeof(full_text) - 1u);

    ui_payload[pos++] = (uint8_t)(g_journal_selected & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 8) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 16) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 24) & 0xFFu);

    ui_payload[pos++] = 0u; /* ts */
    ui_payload[pos++] = 0u;
    ui_payload[pos++] = 0u;
    ui_payload[pos++] = 0u;

    ui_payload[pos++] = (uint8_t)(code & 0xFFu);
    ui_payload[pos++] = (uint8_t)((code >> 8) & 0xFFu);

    ui_payload[pos++] = (uint8_t)(text_len & 0xFFu);
    ui_payload[pos++] = (uint8_t)((text_len >> 8) & 0xFFu);
    if (text_len != 0u) {
        memcpy(&ui_payload[pos], full_text, text_len);
        pos = (uint16_t)(pos + text_len);
    }
    ui_payload[pos++] = 0u; /* raw_len */

    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                RS_PANEL_SCREEN_MENU_JOURNAL_DETAIL,
                                                RS_PANEL_UI_ACTION_REPLACE);
    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                 RS_PANEL_UI_DATA_JOURNAL_DETAIL,
                                                 ui_payload,
                                                 pos);
}

static void rs_panel_master_send_journal_list_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    uint8_t n_items = rs_panel_master_pick_common_journal_lines(master);
    if (n_items == 0u) {
        return;
    }

    EventLogTierInfo_t info;
    if (EventLogReader_GetTierInfo(0u, &info) == false) {
        return;
    }

    uint32_t total = info.count;
    g_journal_total = total;
    if (total == 0u) {
        /* header только, items==0 → панель сбросит кэш. */
        uint8_t ui_payload[1u + 13u];
        uint16_t pos = 0u;

        /* total */
        ui_payload[pos++] = (uint8_t)(total & 0xFFu);
        ui_payload[pos++] = (uint8_t)((total >> 8) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((total >> 16) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((total >> 24) & 0xFFu);

        /* selected_idx */
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;

        /* window_first */
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;
        ui_payload[pos++] = 0u;

        ui_payload[pos++] = 0u; /* n_items */

        rs_panel_master_send_ui_data_to_ready_panels(master,
                                                      RS_PANEL_UI_DATA_JOURNAL_LIST,
                                                      ui_payload,
                                                      pos);
        return;
    }

    if (total < (uint32_t)n_items) {
        n_items = (uint8_t)total;
    }

    g_journal_window_size = n_items;
    if (g_journal_selected >= total) {
        g_journal_selected = total - 1u;
    }
    if (g_journal_window_first == 0u && g_journal_selected == 0u) {
        g_journal_selected = total - 1u;
        if (total > (uint32_t)n_items) {
            g_journal_window_first = total - (uint32_t)n_items;
        }
    }
    rs_panel_master_normalize_journal_window(total, n_items);

    uint8_t ui_payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;

    /* total u32 */
    ui_payload[pos++] = (uint8_t)(total & 0xFFu);
    ui_payload[pos++] = (uint8_t)((total >> 8) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((total >> 16) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((total >> 24) & 0xFFu);

    /* selected_idx u32 */
    ui_payload[pos++] = (uint8_t)(g_journal_selected & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 8) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 16) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_selected >> 24) & 0xFFu);

    /* window_first u32 */
    ui_payload[pos++] = (uint8_t)(g_journal_window_first & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_window_first >> 8) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_window_first >> 16) & 0xFFu);
    ui_payload[pos++] = (uint8_t)((g_journal_window_first >> 24) & 0xFFu);

    ui_payload[pos++] = n_items;

    for (uint8_t i = 0u; i < n_items; i++) {
        uint32_t rec_idx = g_journal_window_first + (uint32_t)i;
        EventLogRecord_t rec;
        EventLogRecStatus_t st = EVENT_LOG_REC_EMPTY;
        EventLogRecord_t *rec_ptr = 0;

        if (!EventLogReader_ReadLogical(0u, rec_idx, &st, &rec) || st != EVENT_LOG_REC_VALID) {
            rec.event_code = 0u;
        } else {
            rec_ptr = &rec;
        }

        uint16_t code = rec.event_code;

        /* ts сейчас отправляем 0: в первом шаге нужен UI-контур. */
        uint32_t ts = 0u;

        char short_text[49];
        rs_panel_master_format_journal_short(rec_ptr, rec_idx, short_text, sizeof(short_text));
        uint8_t text_len = (uint8_t)strnlen(short_text, 48u);

        uint16_t need = (uint16_t)(4u /*rec_idx*/ + 4u /*ts*/ + 2u /*code*/ + 1u /*text_len*/ + (uint16_t)text_len);
        if ((uint16_t)(pos + need) > RS_BUS_MAX_PAYLOAD) {
            break;
        }

        /* rec_idx u32 */
        ui_payload[pos++] = (uint8_t)(rec_idx & 0xFFu);
        ui_payload[pos++] = (uint8_t)((rec_idx >> 8) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((rec_idx >> 16) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((rec_idx >> 24) & 0xFFu);

        /* ts u32 */
        ui_payload[pos++] = (uint8_t)(ts & 0xFFu);
        ui_payload[pos++] = (uint8_t)((ts >> 8) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((ts >> 16) & 0xFFu);
        ui_payload[pos++] = (uint8_t)((ts >> 24) & 0xFFu);

        /* code u16 */
        ui_payload[pos++] = (uint8_t)(code & 0xFFu);
        ui_payload[pos++] = (uint8_t)((code >> 8) & 0xFFu);

        ui_payload[pos++] = text_len;
        if (text_len != 0u) {
            memcpy(&ui_payload[pos], short_text, text_len);
            pos = (uint16_t)(pos + text_len);
        }
    }

    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                  RS_PANEL_UI_DATA_JOURNAL_LIST,
                                                  ui_payload,
                                                  pos);
}

static uint8_t rs_led_type_from_local(uint8_t local_led)
{
    switch (local_led) {
    case LED_POWER:
        return RS_PANEL_LED_POWER;
    case LED_NORM:
        return RS_PANEL_LED_NORM;
    case LED_START:
        return RS_PANEL_LED_START;
    case LED_STOP:
        return RS_PANEL_LED_STOP;
    case LED_ERR:
        return RS_PANEL_LED_ERR;
    case LED_FIRE:
        return RS_PANEL_LED_FIRE;
    case LED_AUTO_OFF:
        return RS_PANEL_LED_AUTO_OFF;
    case LED_BUT_START_ALL:
        return RS_PANEL_LED_BUT_START_ALL;
    case LED_BUT_STOP:
        return RS_PANEL_LED_BUT_STOP;
    case LED_BUT_START_SP:
        return RS_PANEL_LED_BUT_START_SP;
    case LED_STR_START_ALL:
        return RS_PANEL_LED_LBL_START_ALL;
    case LED_STR_STOP:
        return RS_PANEL_LED_LBL_STOP;
    case LED_STR_START_SP:
        return RS_PANEL_LED_LBL_START_SP;
    case LED_BUT_ENTER_UP:
        return RS_PANEL_LED_BUT_ENTER;
    case LED_BUT_ESC_DW:
        return RS_PANEL_LED_BUT_ESC;
    default:
        return 0xFFu;
    }
}

static void rs_panel_master_send_leds_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    /* local: 15 led -> protocol: <= 16 led items */
    uint8_t payload[1u + 15u * 3u];
    uint16_t pos = 0u;
    payload[pos++] = 15u; /* count */

    for (uint8_t local_led = 0u; local_led < 15u; local_led++) {
        uint8_t type = rs_led_type_from_local(local_led);
        if (type == 0xFFu) {
            continue;
        }

        uint8_t st = Led_GetState(local_led);
        st &= 0x03u;

        uint8_t mode;
        uint8_t value = 0u;
        if (st == 0u) {
            mode = RS_PANEL_LED_MODE_OFF;
        } else if (st == 2u) {
            mode = RS_PANEL_LED_MODE_BLINK;
        } else {
            mode = RS_PANEL_LED_MODE_BRIGHT;
            value = Led_GetBrightness(local_led);
        }

        payload[pos++] = type;
        payload[pos++] = mode;
        payload[pos++] = value;
    }

    for (uint8_t i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }

        (void)RsBus_SendFrame(&master->bus,
                               panel->cfg.addr,
                               master->next_seq++,
                               0u, /* flags: master->panel (no DIR) */
                               RS_PANEL_CMD_LED,
                               payload,
                               pos);
    }
}

static void rs_panel_master_send_sound_to_ready_panels(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    uint8_t sound_enabled = Beeper_IsSoundEnabled();
    uint8_t state_code = Beeper_GetStateCode(); /* BEEPER_STATE_IDLE == 0 */

    uint8_t mute = (sound_enabled != 0u) ? 0u : 1u;
    uint8_t profile = RS_PANEL_SOUND_OFF;
    uint16_t on_ms = 0u;
    uint16_t off_ms = 0u;
    uint8_t pulses = 0u;
    uint16_t repeat_ms = 0u;

    if (sound_enabled != 0u && state_code != 0u) {
        profile = RS_PANEL_SOUND_CUSTOM;
        mute = 0u;
        on_ms = Beeper_GetPatternOnMs();
        off_ms = Beeper_GetPatternOffMs();
        pulses = Beeper_GetPatternPulses();
        repeat_ms = Beeper_GetPatternRepeatMs();
    }

    uint8_t payload[9u];
    payload[0] = profile;
    payload[1] = mute;

    uint16_t payload_len = (profile == RS_PANEL_SOUND_CUSTOM) ? 9u : 2u;
    if (payload_len == 9u) {
        payload[2] = (uint8_t)(on_ms & 0xFFu);
        payload[3] = (uint8_t)(on_ms >> 8);
        payload[4] = (uint8_t)(off_ms & 0xFFu);
        payload[5] = (uint8_t)(off_ms >> 8);
        payload[6] = pulses;
        payload[7] = (uint8_t)(repeat_ms & 0xFFu);
        payload[8] = (uint8_t)(repeat_ms >> 8);
    }

    for (uint8_t i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.enabled == 0u || PanelState_IsReady(panel) == 0u) {
            continue;
        }

        (void)RsBus_SendFrame(&master->bus,
                               panel->cfg.addr,
                               master->next_seq++,
                               0u, /* flags: master->panel (no DIR) */
                               RS_PANEL_CMD_SOUND,
                               payload,
                               payload_len);
    }
}

static void rs_panel_master_on_panel_became_ready(RsPanelMaster *master)
{
    if (master == 0u) {
        return;
    }

    /* Не сбрасывать MENU_* в LOGO: NAV на логотип не шлём, панель остаётся в меню,
     * а мастер начинает игнорировать UP/DOWN/ESC. */
    if (rs_panel_master_is_menu_ui_screen(g_ui_current_screen_id) == 0u) {
        g_ui_current_screen_id = RS_PANEL_SCREEN_LOGO;
        s_logo_main_nav_deadline_ms = HAL_GetTick() + RS_PANEL_LOGO_MAIN_DELAY_MS;
    } else {
        s_logo_main_nav_deadline_ms = 0u;
    }
    s_panel_ui_resync_pending = 1u;
    /* UI_NAV/WARN/FIRE не из обработчика RX CAPS: UART half-duplex, плюс логотип
     * на панели сам переходит на MAIN через 400 тиков. Снимок отправим после лого. */
}

void App_OnFireUiUpdate(uint8_t active,
                          uint8_t mode,
                          uint8_t remaining_s,
                          uint8_t n_zones,
                          char (*zone_names)[ZONE_NAME_SIZE + 1])
{
    RsPanelMaster *master = g_active_master;
    if (master == 0u || zone_names == 0u) {
        return;
    }

    if (n_zones > 16u) {
        n_zones = 16u;
    }

    /* rs_apply_main_fire ждёт PAYLOAD без sub_id:
     * active(1) mode(1) remaining_s(1) sel_index(1) n_zones(1) + [str_len + str]* */
    uint8_t ui_payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;
    uint16_t max_pos = RS_BUS_MAX_PAYLOAD;

    ui_payload[pos++] = (active != 0u) ? (uint8_t)1u : (uint8_t)0u;
    ui_payload[pos++] = mode;
    ui_payload[pos++] = remaining_s;
    ui_payload[pos++] = 0u; /* sel_index (не используется в панели) */
    uint16_t n_zones_pos = pos;
    ui_payload[pos++] = 0u; /* n_zones: заполним по факту (может урезаться по размеру кадра) */

    uint8_t out_n_zones = 0u;
    for (uint8_t i = 0u; i < n_zones; i++) {
        uint8_t str_len = (uint8_t)strnlen(zone_names[i], ZONE_NAME_SIZE);
        /* payload: [str_len][str bytes] */
        if ((uint16_t)(pos + 1u + str_len) > max_pos) {
            break;
        }
        ui_payload[pos++] = str_len;
        if (str_len != 0u) {
            memcpy(&ui_payload[pos], zone_names[i], str_len);
            pos = (uint16_t)(pos + str_len);
        }
        out_n_zones++;
        ui_payload[n_zones_pos] = out_n_zones;
    }

    if (pos < 5u) {
        return;
    }

    /* Как в stm_PPKY v1: если пришёл пожар — принудительно переводим UI панелей на MAIN. */
    rs_panel_master_ensure_main_screen(master);
    rs_panel_master_send_ui_data_to_ready_panels(master, RS_PANEL_UI_DATA_MAIN_FIRE, ui_payload, pos);

    rs_panel_master_send_leds_to_ready_panels(master);
    rs_panel_master_send_sound_to_ready_panels(master);

    /* Журнал: обновляем кэш на панели по мере прихода UI-обновлений. */
    rs_panel_master_send_journal_list_to_ready_panels(master);
}

uint8_t App_OnWarningUiUpdate(uint8_t active,
                            uint8_t n_items,
                            char (*big_titles)[WARNING_TITLE_LEN],
                            char (*details)[ZONE_NAME_SIZE + 1])
{
    RsPanelMaster *master = g_active_master;
    uint8_t delivered;

    if (master == 0u || big_titles == 0u || details == 0u) {
        return 0u;
    }
    /* Не слать WARN в том же тике, что UI_NAV меню. */
    if (s_ui_evt_q_count != 0u) {
        return 0u;
    }

    if (n_items > 4u) {
        n_items = 4u;
    }

    /* rs_apply_main_warn ждёт PAYLOAD без sub_id:
     * count(1) ver(1) crc16(2) n_items(1)
     * затем для каждого item: flags(1) title_len(1) title(title_len) detail_len(1) detail(detail_len) */
    uint8_t ui_payload[RS_BUS_MAX_PAYLOAD];
    uint16_t pos = 0u;

    ui_payload[pos++] = (active != 0u) ? 1u : 0u; /* count (bool) */
    ui_payload[pos++] = 0u; /* ver */
    ui_payload[pos++] = 0u; /* crc16 lo */
    ui_payload[pos++] = 0u; /* crc16 hi */
    pos++; /* n_items: заполним после упаковки */
    uint8_t packed_items = 0u;

    for (uint8_t i = 0u; i < n_items; i++) {
        uint8_t title_len = (uint8_t)strnlen(big_titles[i], 23u);
        /* Detail режем, чтобы весь WARN (sub_id + payload) влез в 1 кадр ≤251 байт. */
        uint8_t detail_len = (uint8_t)strnlen(details[i], 32u);

        uint16_t need = (uint16_t)(1u /*flags*/ + 1u + title_len + 1u + detail_len);
        if ((uint16_t)(pos + need) > (uint16_t)(RS_BUS_MAX_WIRE_PAYLOAD - 1u)) {
            break;
        }

        ui_payload[pos++] = 0u; /* flags */
        ui_payload[pos++] = title_len;
        if (title_len != 0u) {
            memcpy(&ui_payload[pos], big_titles[i], title_len);
            pos = (uint16_t)(pos + title_len);
        }

        ui_payload[pos++] = detail_len;
        if (detail_len != 0u) {
            memcpy(&ui_payload[pos], details[i], detail_len);
            pos = (uint16_t)(pos + detail_len);
        }
        packed_items++;
    }
    ui_payload[4] = packed_items;

    /* Как в stm_PPKY v1: если пришло warning — переводим на MAIN только если не там. */
    rs_panel_master_ensure_main_screen(master);
    delivered = rs_panel_master_send_ui_data_to_ready_panels(master,
                                                             RS_PANEL_UI_DATA_MAIN_WARN,
                                                             ui_payload,
                                                             pos);
    if (delivered == 0u) {
        return 0u;
    }

    g_rs_master_dbg.warn_ui_tx++;
    g_rs_master_dbg.last_warn_active = (active != 0u) ? 1u : 0u;
    g_rs_master_dbg.last_warn_n_items = packed_items;

    rs_panel_master_send_leds_to_ready_panels(master);
    rs_panel_master_send_sound_to_ready_panels(master);
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

static uint8_t rs_decode_ack(const uint8_t *src, uint16_t src_len, uint8_t *ack_seq)
{
    if (src == 0 || ack_seq == 0 || src_len < 1u) {
        return 0u;
    }
    *ack_seq = src[0];
    return 1u;
}

/* Если панель по ошибке прислала btn_event вместо ui_event — всё равно
 * обработать навигацию меню (только когда мастер точно в MENU_*). */
static void rs_panel_master_synth_menu_ui_from_btns(RsPanelPollRsp *rsp)
{
    uint8_t i;
    uint8_t n = 0u;

    if (rsp == 0 || rsp->ui_evt_count != 0u || rsp->evt_count == 0u) {
        return;
    }
    if (rs_panel_master_is_menu_ui_screen(g_ui_current_screen_id) == 0u) {
        return;
    }

    for (i = 0u; i < rsp->evt_count && i < RS_PANEL_MAX_POLL_BTN_EVENTS; i++) {
        const RsPanelButtonEvent *be = &rsp->btn_events[i];
        RsPanelUiEvent *ue;

        if (be->state != (uint8_t)RS_PANEL_BUTTON_PRESS) {
            continue;
        }
        if (n >= RS_PANEL_MAX_POLL_UI_EVENTS) {
            break;
        }
        ue = &rsp->ui_events[n];
        memset(ue, 0, sizeof(*ue));

        if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_ROOT) {
            if (be->type == RS_PANEL_BTN_ESC) {
                ue->evt_type = RS_PANEL_UI_EVT_BACK;
            } else if (be->type == RS_PANEL_BTN_UP) {
                ue->evt_type = RS_PANEL_UI_EVT_NAV;
                ue->p1 = 0u;
                ue->p2 = g_menu_selected;
            } else if (be->type == RS_PANEL_BTN_DOWN) {
                ue->evt_type = RS_PANEL_UI_EVT_NAV;
                ue->p1 = 1u;
                ue->p2 = g_menu_selected;
            } else if (be->type == RS_PANEL_BTN_ENTER) {
                ue->evt_type = RS_PANEL_UI_EVT_MENU_SELECT;
                ue->p1 = g_menu_selected;
            } else {
                continue;
            }
        } else {
            if (be->type == RS_PANEL_BTN_ESC) {
                ue->evt_type = RS_PANEL_UI_EVT_BACK;
            } else if (be->type == RS_PANEL_BTN_UP) {
                ue->evt_type = RS_PANEL_UI_EVT_NAV;
                ue->p1 = 0u;
            } else if (be->type == RS_PANEL_BTN_DOWN) {
                ue->evt_type = RS_PANEL_UI_EVT_NAV;
                ue->p1 = 1u;
            } else if (be->type == RS_PANEL_BTN_ENTER) {
                ue->evt_type = RS_PANEL_UI_EVT_CONFIRM;
            } else {
                continue;
            }
        }
        n++;
    }
    rsp->ui_evt_count = n;
}

static void rs_panel_master_handle_ui_events(RsPanelMaster *master,
                                             PanelState *panel,
                                             const RsPanelPollRsp *rsp)
{
    RsPanelPollRsp local;
    const RsPanelPollRsp *use = rsp;

    if (master == 0 || rsp == 0) {
        return;
    }

    if (rsp->ui_evt_count == 0u && rsp->evt_count != 0u &&
        rs_panel_master_is_menu_ui_screen(g_ui_current_screen_id) != 0u) {
        local = *rsp;
        rs_panel_master_synth_menu_ui_from_btns(&local);
        use = &local;
    }

    for (uint8_t i = 0u; i < use->ui_evt_count && i < RS_PANEL_MAX_POLL_UI_EVENTS; i++) {
        const RsPanelUiEvent *evt = &use->ui_events[i];

        switch (evt->evt_type) {
        case RS_PANEL_UI_EVT_NAV:
            if (rs_panel_master_is_menu_root_session() != 0u) {
                g_ui_current_screen_id = RS_PANEL_SCREEN_MENU_ROOT;
                s_logo_main_nav_deadline_ms = 0u;
                /* MENU_ROOT навигация */
                if (g_menu_n_items == 0u) {
                    break;
                }

                if (evt->p1 == 0u) {
                    /* UP */
                    if (g_menu_selected == 0u) {
                        g_menu_selected = (uint16_t)(g_menu_n_items - 1u);
                    } else {
                        g_menu_selected--;
                    }
                } else {
                    /* DOWN */
                    g_menu_selected = (uint16_t)((g_menu_selected + 1u) % g_menu_n_items);
                }

                MenuUi_SetMenuSelected(g_menu_selected);
                rs_panel_master_send_menu_list_to_ready_panels(master,
                                                                 g_menu_selected,
                                                                 g_menu_n_items);
                rs_panel_master_send_menu_state_to_ready_panels(master);
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_DEVICES ||
                g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_DEVICE_DETAIL) {
                uint8_t slots[MAX_MCU_IN_BUS];
                uint8_t count = rs_panel_master_collect_mcu_slots(slots, MAX_MCU_IN_BUS);
                if (count == 0u) {
                    break;
                }

                uint8_t idx = 0u;
                for (uint8_t s = 0u; s < count; s++) {
                    if (slots[s] == g_device_selected_slot) {
                        idx = s;
                        break;
                    }
                }

                if (evt->p1 == 0u) {
                    idx = (idx == 0u) ? (uint8_t)(count - 1u) : (uint8_t)(idx - 1u);
                } else {
                    idx = (uint8_t)((idx + 1u) % count);
                }
                g_device_selected_slot = slots[idx];

                if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_DEVICE_DETAIL) {
                    rs_panel_master_send_device_detail_to_ready_panels(master);
                } else {
                    rs_panel_master_send_device_list_to_ready_panels(master);
                }
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_BLOCK_ZONE) {
                uint8_t zones[ZONE_NUMBER];
                uint8_t count = 0u;
                uint8_t idx = 0u;

                for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
                    if (PPKYConfig.zone_name[zi][0] == 0) {
                        continue;
                    }
                    zones[count++] = zi;
                }
                if (count == 0u) {
                    break;
                }

                for (uint8_t i = 0u; i < count; i++) {
                    if (zones[i] == g_block_zone_selected) {
                        idx = i;
                        break;
                    }
                }

                if (evt->p1 == 0u) {
                    idx = (idx == 0u) ? (uint8_t)(count - 1u) : (uint8_t)(idx - 1u);
                } else {
                    idx = (uint8_t)((idx + 1u) % count);
                }
                g_block_zone_selected = zones[idx];
                rs_panel_master_send_zone_mode_list_to_ready_panels(master);
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_CONNECTION) {
                if (PPKYConfig.wifi_block != 0u) {
                    g_connection_selected = 1u; /* только RS-485 */
                } else {
                    g_connection_selected = (evt->p1 == 0u) ? 0u : 1u;
                }
                rs_panel_master_send_connection_status_to_ready_panels(master);
                break;
            }

            /* ЖУРНАЛ навигация */
            if (g_ui_current_screen_id != RS_PANEL_SCREEN_MENU_JOURNAL &&
                g_ui_current_screen_id != RS_PANEL_SCREEN_MENU_JOURNAL_DETAIL) {
                break;
            }

            if (g_journal_total == 0u) {
                break;
            }

            if (evt->p1 == 0u) {
                if (g_journal_selected > 0u) {
                    g_journal_selected--;
                }
            } else if (evt->p1 == 1u) {
                if ((g_journal_selected + 1u) < g_journal_total) {
                    g_journal_selected++;
                }
            }

            rs_panel_master_normalize_journal_window(g_journal_total, g_journal_window_size);
            g_journal_detail_open = 0u;
            rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                        RS_PANEL_SCREEN_MENU_JOURNAL,
                                                        RS_PANEL_UI_ACTION_REPLACE);
            rs_panel_master_send_journal_list_to_ready_panels(master);
            break;

        case RS_PANEL_UI_EVT_CONFIRM:
            /* Панель после logo сама на MAIN; мастер может ещё держать LOGO.
             * Если UI_NAV меню не дошёл, мастер уже MENU_ROOT, а панель шлёт CONFIRM
             * с главного — повторяем открытие, иначе ENTER «теряется». */
            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MAIN ||
                g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO ||
                g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_ROOT) {
                /* ENTER на главном экране → открыть MENU_ROOT и отправить MENU_LIST */
                g_menu_selected = 0u;
                MenuUi_SetMenuSelected(0u);
                MenuUi_ResetMenuIndex();
#if GOST_MODE
                g_menu_n_items = 6u;
#else
                g_menu_n_items = 7u;
#endif
                rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                            RS_PANEL_SCREEN_MENU_ROOT,
                                                            RS_PANEL_UI_ACTION_REPLACE);
                rs_panel_master_send_menu_list_to_ready_panels(master,
                                                                 g_menu_selected,
                                                                 g_menu_n_items);
                rs_panel_master_send_menu_state_to_ready_panels(master);
                s_logo_main_nav_deadline_ms = 0u;
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_DEVICES) {
                rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                            RS_PANEL_SCREEN_MENU_DEVICE_DETAIL,
                                                            RS_PANEL_UI_ACTION_REPLACE);
                rs_panel_master_send_device_detail_to_ready_panels(master);
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_BLOCK_ZONE) {
                uint8_t mode = PPKY_ZoneFireModeGet(g_block_zone_selected);
                if (mode == 0u) {
                    mode = 2u;
                } else if (mode == 2u) {
                    mode = 3u;
                } else {
                    mode = 0u;
                }
                PPKY_ZoneFireModeSet(g_block_zone_selected, mode);
                PPKY_ZoneModeUiNotify(g_block_zone_selected);
                SaveConfig();
                ConfigIgnBlockSync_Request();
                rs_panel_master_send_zone_mode_list_to_ready_panels(master);
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_CONNECTION) {
                if (g_connection_selected == 0u) {
                    if (PPKYConfig.wifi_block != 0u) {
                        rs_panel_master_send_connection_status_to_ready_panels(master);
                        break;
                    }
                    if (EspManager_IsUserWifiOn() != 0u) {
                        EspManager_RequestWifiDisable();
                    } else {
                        EspManager_RequestWifiEnable();
                    }
                } else {
                    PPKYConfig.rs485_on = (PPKYConfig.rs485_on == 0u) ? 1u : 0u;
                    SaveConfig();
                }
                rs_panel_master_send_connection_status_to_ready_panels(master);
                break;
            }

            /* ЖУРНАЛ: ENTER → открыть JOURNAL_DETAIL */
            if (g_ui_current_screen_id != RS_PANEL_SCREEN_MENU_JOURNAL) {
                break;
            }
            if (g_journal_total == 0u) {
                break;
            }
            g_journal_detail_open = 1u;
            rs_panel_master_send_journal_detail_to_ready_panels(master);
            break;

        case RS_PANEL_UI_EVT_BACK:
            if (rs_panel_master_accept_menu_root_evt() != 0u) {
                /* ESC в меню → назад на MAIN (и если мастер ещё думал, что MAIN/LOGO). */
                s_logo_main_nav_deadline_ms = 0u;
                rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                            RS_PANEL_SCREEN_MAIN,
                                                            RS_PANEL_UI_ACTION_REPLACE);
                break;
            }

            /* ЖУРНАЛ */
            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_JOURNAL ||
                g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_JOURNAL_DETAIL) {
                if (g_journal_detail_open != 0u) {
                    g_journal_detail_open = 0u;
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_JOURNAL,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_journal_list_to_ready_panels(master);
                } else {
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_ROOT,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_menu_list_to_ready_panels(master,
                                                                     g_menu_selected,
                                                                     g_menu_n_items);
                    rs_panel_master_send_menu_state_to_ready_panels(master);
                }
                break;
            }

            /* Подэкраны */
            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_DEVICE_DETAIL) {
                rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                            RS_PANEL_SCREEN_MENU_DEVICES,
                                                            RS_PANEL_UI_ACTION_REPLACE);
                rs_panel_master_send_device_list_to_ready_panels(master);
                break;
            }

            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_CONFIG) {
                MenuUi_SetConfigSession(0u);
                rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                            RS_PANEL_SCREEN_MENU_CONNECTION,
                                                            RS_PANEL_UI_ACTION_REPLACE);
                rs_panel_master_send_connection_status_to_ready_panels(master);
                break;
            }

            /* По умолчанию: назад в MENU_ROOT */
            if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_CONNECTION) {
                MenuUi_SetConfigSession(0u);
            }
            rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                        RS_PANEL_SCREEN_MENU_ROOT,
                                                        RS_PANEL_UI_ACTION_REPLACE);
            rs_panel_master_send_menu_list_to_ready_panels(master,
                                                             g_menu_selected,
                                                             g_menu_n_items);
            rs_panel_master_send_menu_state_to_ready_panels(master);
            break;

        case RS_PANEL_UI_EVT_MENU_SELECT:
            if (rs_panel_master_accept_menu_root_evt() == 0u) {
                break;
            }
            g_ui_current_screen_id = RS_PANEL_SCREEN_MENU_ROOT;
            s_logo_main_nav_deadline_ms = 0u;
            if (panel == 0) {
                break;
            }

            if (g_menu_n_items == 0u) {
                break;
            }

            {
                uint16_t idx = evt->p1;
                if (idx >= (uint16_t)g_menu_n_items) {
                    idx = (uint16_t)(g_menu_n_items - 1u);
                }

                const uint8_t panel_addr = panel->cfg.addr;

                /* Синхронизируем “индекс пункта меню” с логикой на панели:
                 * panel: ScreenMenuPresenter::menuActionIndex(logical_idx) */
                int action = (int)idx;
#if GOST_MODE
                action = action + 1;
#endif

                switch ((uint32_t)action) {
                case 0u: {
                    /* РЕЖИМ: циклически 0..2 */
                    uint8_t new_mode = (uint8_t)((PPKYConfig.fire_mode + 1u) % 3u);
                    if (new_mode != PPKYConfig.fire_mode) {
                        PPKYConfig.fire_mode = new_mode;
                        EventLog_LogFireModeChange(new_mode, panel_addr);
                    }
                    rs_panel_master_send_menu_state_to_ready_panels(master);
                    rs_panel_master_send_leds_to_ready_panels(master);
                } break;

                case 1u: {
                    /* ЗВУК */
                    if (PPKYConfig.beep_block != 0u) {
                        break;
                    }
                    uint8_t new_beep = (PPKYConfig.beep != 0u) ? 0u : 1u;
                    if (new_beep != PPKYConfig.beep) {
                        PPKYConfig.beep = new_beep;
                        Beeper_SoundOnOff(new_beep != 0u);
                        EventLog_LogSoundToggle(new_beep, panel_addr);
                        rs_panel_master_send_menu_state_to_ready_panels(master);
                        rs_panel_master_send_sound_to_ready_panels(master);
                        rs_panel_master_send_leds_to_ready_panels(master);
                    }
                } break;

                case 3u: {
                    /* ЖУРНАЛ */
                    g_journal_detail_open = 0u;
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_JOURNAL,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_journal_list_to_ready_panels(master);
                } break;

                case 2u: {
                    /* СВЯЗЬ */
                    g_journal_detail_open = 0u;
                    g_connection_selected = (PPKYConfig.wifi_block != 0u) ? 1u : 0u;
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_CONNECTION,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_connection_status_to_ready_panels(master);
                } break;

                case 4u: {
                    /* УСТРОЙСТВА */
                    g_journal_detail_open = 0u;
                    g_device_selected_slot = 0xFFu;
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_DEVICES,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_device_list_to_ready_panels(master);
                } break;

                case 5u: {
                    /* БЛОК ЗОНЫ: отдельный экран, как в stm_PPKY v1 */
                    g_journal_detail_open = 0u;
                    g_block_zone_selected = 0u;
                    rs_panel_master_send_ui_nav_to_ready_panels(master,
                                                                RS_PANEL_SCREEN_MENU_BLOCK_ZONE,
                                                                RS_PANEL_UI_ACTION_REPLACE);
                    rs_panel_master_send_zone_mode_list_to_ready_panels(master);
                } break;

                case 6u: {
                    /* Общая команда запуска теста индикации/экрана.
                     * Что именно делает тест, определяет сама панель. */
                    static const uint8_t k_no_payload = 0u;
                    rs_panel_master_send_ui_data_to_ready_panels(master,
                                                                 RS_PANEL_UI_DATA_MENU_SELF_TEST,
                                                                 &k_no_payload,
                                                                 1u);
                } break;

                default:
                    /* Остальные пункты меню пока не реализованы в master. */
                    break;
                }
            }
            break;

        default:
            break;
        }
    }
}

uint16_t RsPanel_EncodePollReq(uint8_t *dst, uint16_t dst_size, const RsPanelPollReq *req)
{
    if (dst == 0 || req == 0 || dst_size < 2u) {
        return 0u;
    }
    dst[0] = req->flags;
    dst[1] = req->ack_seq;
    return 2u;
}

uint8_t RsPanel_DecodePollReq(const uint8_t *src, uint16_t src_len, RsPanelPollReq *out_req)
{
    if (src == 0 || out_req == 0 || src_len < 2u) {
        return 0u;
    }
    out_req->flags = src[0];
    out_req->ack_seq = src[1];
    return 1u;
}

uint16_t RsPanel_EncodePollRsp(uint8_t *dst, uint16_t dst_size, const RsPanelPollRsp *rsp)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (dst == 0 || rsp == 0 || dst_size < 3u) {
        return 0u;
    }
    dst[pos++] = rsp->status;
    dst[pos++] = rsp->evt_count;
    for (i = 0u; i < rsp->evt_count && i < RS_PANEL_MAX_POLL_BTN_EVENTS; i++) {
        if ((uint16_t)(pos + 3u) > dst_size) {
            return 0u;
        }
        dst[pos++] = rsp->btn_events[i].type;
        dst[pos++] = rsp->btn_events[i].state;
        dst[pos++] = rsp->btn_events[i].level;
    }
    if ((uint16_t)(pos + 1u) > dst_size) {
        return 0u;
    }
    dst[pos++] = rsp->ui_evt_count;
    for (i = 0u; i < rsp->ui_evt_count && i < RS_PANEL_MAX_POLL_UI_EVENTS; i++) {
        if ((uint16_t)(pos + 5u) > dst_size) {
            return 0u;
        }
        dst[pos++] = rsp->ui_events[i].evt_type;
        pos += rs_put_u16le(&dst[pos], rsp->ui_events[i].p1);
        pos += rs_put_u16le(&dst[pos], rsp->ui_events[i].p2);
    }
    return pos;
}

uint8_t RsPanel_DecodePollRsp(const uint8_t *src, uint16_t src_len, RsPanelPollRsp *out_rsp)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (src == 0 || out_rsp == 0 || src_len < 3u) {
        return 0u;
    }
    memset(out_rsp, 0, sizeof(*out_rsp));
    out_rsp->status = src[pos++];
    out_rsp->evt_count = src[pos++];
    if (out_rsp->evt_count > RS_PANEL_MAX_POLL_BTN_EVENTS) {
        return 0u;
    }
    for (i = 0u; i < out_rsp->evt_count; i++) {
        if ((uint16_t)(pos + 3u) > src_len) {
            return 0u;
        }
        out_rsp->btn_events[i].type = src[pos++];
        out_rsp->btn_events[i].state = src[pos++];
        out_rsp->btn_events[i].level = src[pos++];
    }
    if ((uint16_t)(pos + 1u) > src_len) {
        return 0u;
    }
    out_rsp->ui_evt_count = src[pos++];
    if (out_rsp->ui_evt_count > RS_PANEL_MAX_POLL_UI_EVENTS) {
        return 0u;
    }
    for (i = 0u; i < out_rsp->ui_evt_count; i++) {
        if ((uint16_t)(pos + 5u) > src_len) {
            return 0u;
        }
        out_rsp->ui_events[i].evt_type = src[pos++];
        out_rsp->ui_events[i].p1 = rs_get_u16le(&src[pos]);
        pos += 2u;
        out_rsp->ui_events[i].p2 = rs_get_u16le(&src[pos]);
        pos += 2u;
    }
    return 1u;
}

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
    return pos;
}

uint8_t RsPanel_DecodeActivity(const uint8_t *src, uint16_t src_len, RsPanelActivity *out_act)
{
    if (src == 0 || out_act == 0 || src_len < RS_PANEL_ACTIVITY_PAYLOAD_SIZE) {
        return 0u;
    }
    out_act->dev_type = src[0];
    out_act->fw_ver = rs_get_u16le(&src[1]);
    out_act->hw_id = rs_get_u16le(&src[3]);
    out_act->status = src[5];
    out_act->uptime_sec = (uint32_t)src[6] |
                          ((uint32_t)src[7] << 8) |
                          ((uint32_t)src[8] << 16) |
                          ((uint32_t)src[9] << 24);
    return 1u;
}

uint16_t RsPanel_EncodeLedCmd(uint8_t *dst, uint16_t dst_size, const RsPanelLedCmd *cmd)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (dst == 0 || cmd == 0 || dst_size < 1u) {
        return 0u;
    }
    dst[pos++] = cmd->count;
    for (i = 0u; i < cmd->count && i < RS_PANEL_MAX_LED_ITEMS; i++) {
        if ((uint16_t)(pos + 3u) > dst_size) {
            return 0u;
        }
        dst[pos++] = cmd->items[i].type;
        dst[pos++] = cmd->items[i].mode;
        dst[pos++] = cmd->items[i].value;
    }
    return pos;
}

uint8_t RsPanel_DecodeLedCmd(const uint8_t *src, uint16_t src_len, RsPanelLedCmd *out_cmd)
{
    uint16_t pos = 0u;
    uint8_t i;

    if (src == 0 || out_cmd == 0 || src_len < 1u) {
        return 0u;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->count = src[pos++];
    if (out_cmd->count > RS_PANEL_MAX_LED_ITEMS) {
        return 0u;
    }
    for (i = 0u; i < out_cmd->count; i++) {
        if ((uint16_t)(pos + 3u) > src_len) {
            return 0u;
        }
        out_cmd->items[i].type = src[pos++];
        out_cmd->items[i].mode = src[pos++];
        out_cmd->items[i].value = src[pos++];
    }
    return 1u;
}

uint16_t RsPanel_EncodeSoundCmd(uint8_t *dst, uint16_t dst_size, const RsPanelSoundCmd *cmd)
{
    if (dst == 0 || cmd == 0 || dst_size < 2u) {
        return 0u;
    }
    dst[0] = cmd->profile;
    dst[1] = cmd->mute;
    if (cmd->profile != RS_PANEL_SOUND_CUSTOM) {
        return 2u;
    }
    if (dst_size < 9u) {
        return 0u;
    }
    rs_put_u16le(&dst[2], cmd->on_ms);
    rs_put_u16le(&dst[4], cmd->off_ms);
    dst[6] = cmd->pulses;
    rs_put_u16le(&dst[7], cmd->repeat_ms);
    return 9u;
}

uint8_t RsPanel_DecodeSoundCmd(const uint8_t *src, uint16_t src_len, RsPanelSoundCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < 2u) {
        return 0u;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->profile = src[0];
    out_cmd->mute = src[1];
    if (out_cmd->profile != RS_PANEL_SOUND_CUSTOM) {
        return 1u;
    }
    if (src_len < 9u) {
        return 0u;
    }
    out_cmd->on_ms = rs_get_u16le(&src[2]);
    out_cmd->off_ms = rs_get_u16le(&src[4]);
    out_cmd->pulses = src[6];
    out_cmd->repeat_ms = rs_get_u16le(&src[7]);
    return 1u;
}

uint16_t RsPanel_EncodeTimeCmd(uint8_t *dst, uint16_t dst_size, const RsPanelTimeCmd *cmd)
{
    if (dst == 0 || cmd == 0 || dst_size < 6u) {
        return 0u;
    }
    dst[0] = cmd->hour;
    dst[1] = cmd->min;
    dst[2] = cmd->sec;
    dst[3] = cmd->day;
    dst[4] = cmd->month;
    dst[5] = cmd->year;
    return 6u;
}

uint8_t RsPanel_DecodeTimeCmd(const uint8_t *src, uint16_t src_len, RsPanelTimeCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < 6u) {
        return 0u;
    }
    out_cmd->hour = src[0];
    out_cmd->min = src[1];
    out_cmd->sec = src[2];
    out_cmd->day = src[3];
    out_cmd->month = src[4];
    out_cmd->year = src[5];
    return 1u;
}

uint16_t RsPanel_EncodeUiNavCmd(uint8_t *dst, uint16_t dst_size, const RsPanelUiNavCmd *cmd)
{
    if (dst == 0 || cmd == 0 || dst_size < 5u) {
        return 0u;
    }
    rs_put_u16le(&dst[0], cmd->screen_id);
    dst[2] = cmd->action;
    rs_put_u16le(&dst[3], cmd->param);
    return 5u;
}

uint8_t RsPanel_DecodeUiNavCmd(const uint8_t *src, uint16_t src_len, RsPanelUiNavCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < 5u) {
        return 0u;
    }
    out_cmd->screen_id = rs_get_u16le(&src[0]);
    out_cmd->action = src[2];
    out_cmd->param = rs_get_u16le(&src[3]);
    return 1u;
}

uint8_t RsPanel_DecodeUiDataCmd(const uint8_t *src, uint16_t src_len, RsPanelUiDataCmd *out_cmd)
{
    if (src == 0 || out_cmd == 0 || src_len < 1u) {
        return 0u;
    }
    out_cmd->sub_id = src[0];
    out_cmd->payload = &src[1];
    out_cmd->payload_len = (uint16_t)(src_len - 1u);
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
    if (frame->cmd == RS_PANEL_RSP_ACTIVITY) {
        return 1u;
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

static void rs_panel_master_on_frame(const RsBusFrameView *frame, void *ctx)
{
    RsPanelMaster *master = (RsPanelMaster *)ctx;
    uint8_t i;

    if (master == 0 || frame == 0) {
        return;
    }
    if ((frame->flags & RS_BUS_FLAG_DIR) == 0u) {
        g_rs_master_dbg.rx_frames_wrong_dir++;
        return;
    }
    g_rs_master_dbg.rx_frames_ok++;

    /* Прокидка в WiFi/ПО: activity + ответы boot-команд (тот же addr панели). */
    if (rs_panel_should_forward_to_host(frame) != 0u) {
        rs_panel_forward_frame_to_esp(frame);
    }

    for (i = 0u; i < master->panel_count; i++) {
        PanelState *panel = &master->panels[i];
        if (panel->cfg.addr != frame->addr) {
            g_rs_master_dbg.rx_frames_wrong_addr++;
            continue;
        }

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
            uint8_t ack_seq = 0u;
            if (rs_decode_ack(frame->payload, frame->payload_len, &ack_seq) != 0u &&
                panel->ack_wait_active != 0u &&
                panel->pending_ack_seq == ack_seq) {
                if (panel->journal_fault_latched != 0u) {
                    panel->journal_fault_latched = 0u;
                    rs_panel_master_update_journal_fault_mask();
                    rs_panel_master_log_journal_link(panel->cfg.addr, 1u);
                }
                panel->ack_wait_active = 0u;
                panel->pending_ack_seq = 0u;
                panel->ack_retries_left = 0u;
                panel->ack_deadline_ms = 0u;
                panel->pending_ui_stream_len = 0u;
            }
        } else if (frame->cmd == RS_PANEL_RSP_POLL) {
            RsPanelPollRsp rsp;
            g_rs_master_dbg.rsp_poll_rx++;
            if (RsPanel_DecodePollRsp(frame->payload, frame->payload_len, &rsp)) {
                PanelState_OnPollRsp(panel, &rsp, HAL_GetTick());
                /* Не SendFrame из RX IRQ — только очередь. */
                (void)rs_panel_master_ui_evt_enqueue(i, &rsp);
            }
        } else if (frame->cmd == RS_PANEL_RSP_ACTIVITY) {
            /* Presence для ПО уже прокинута выше; локально только фиксируем RX. */
            panel->last_rx_ms = HAL_GetTick();
        }
        break;
    }
}

void RsPanelMaster_LoadDefaultConfig(RsPanelMaster *master)
{
    PanelConfig cfg;

    if (master == 0) {
        return;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1u;
    cfg.addr = 0x01u;
    cfg.role = RS_PANEL_ROLE_PRIMARY;
    cfg.poll_ms = 10u;
    cfg.expected_hw_id = 0u;

    master->panel_count = 1u;
    PanelState_Reset(&master->panels[0]);
    PanelState_BindConfig(&master->panels[0], &cfg);
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
    RsBus_Init(&master->bus, uart, de_port, de_pin, rs_panel_master_on_frame, master);
    RsPanelMaster_LoadDefaultConfig(master);
    g_active_master = master;
}

void RsPanelMaster_PushSound(void)
{
    if (g_active_master != 0) {
        rs_panel_master_send_sound_to_ready_panels(g_active_master);
    }
}

void RsPanelMaster_OnRxBytes(RsPanelMaster *master, const uint8_t *data, uint16_t len)
{
    if (master == 0) {
        return;
    }
    RsBus_ProcessRxBytes(&master->bus, data, len);
}

void RsPanelMaster_Process10ms(RsPanelMaster *master, uint32_t now_ms)
{
    PanelState *panel;
    RsPanelPollReq req;
    uint8_t payload[8];
    uint16_t payload_len;

    if (master == 0 || master->panel_count == 0u) {
        return;
    }

    /* Сначала UI-ответы на кнопки (TX вне RX IRQ).
     * В этом же тике не шлём POLL/WARN: иначе UI_NAV+MENU_LIST+POLL одним пакетом
     * и панель часто теряет первый вход в меню. */
    if (s_ui_evt_q_count != 0u) {
        rs_panel_master_ui_evt_process_pending(master);
        RsPanelMasterDebug_Timer10ms();
        g_rs_master_dbg.menu_selected = g_menu_selected;
        g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;
        return;
    }

    RsPanelMasterDebug_Timer10ms();
    g_rs_master_dbg.panel_caps_valid = master->panels[0].caps_valid;
    g_rs_master_dbg.panel_link_state = (uint8_t)master->panels[0].link_state;
    g_rs_master_dbg.menu_selected = g_menu_selected;
    g_rs_master_dbg.ui_screen_id = g_ui_current_screen_id;

    if (s_logo_main_nav_deadline_ms != 0u &&
        PanelState_IsReady(&master->panels[0]) != 0u &&
        (int32_t)(now_ms - s_logo_main_nav_deadline_ms) >= 0) {
        s_logo_main_nav_deadline_ms = 0u;
        /* Панель сама уходит с logo (400 тиков). Повторный UI_NAV сдвигает виджеты.
         * Не затирать MENU_*: пользователь мог уже открыть меню. */
        if (g_ui_current_screen_id == RS_PANEL_SCREEN_LOGO) {
            g_ui_current_screen_id = RS_PANEL_SCREEN_MAIN;
        }
    }

    if (s_panel_ui_resync_pending != 0u &&
        PanelState_IsReady(&master->panels[0]) != 0u &&
        s_logo_main_nav_deadline_ms == 0u &&
        rs_panel_master_is_menu_ui_screen(g_ui_current_screen_id) == 0u &&
        Warning_IsProcessDelayActive() == 0u &&
        Warning_GetLastUiBuildCount() != 0u) {
        s_panel_ui_resync_pending = 0u;
        Warning_ResetPanelUiCache();
        Warning_RepublishUiNow();
        Fire_ForceUiResync();
    }

    /* Если панель READY, но кэш «отправлено» без реальной доставки — сброс раз в 1 с,
     * следующий WarningProcess1ms переотправит через PushUiIfChanged. */
    {
        static uint32_t s_warn_cache_reset_ms = 0u;
        if (PanelState_IsReady(&master->panels[0]) != 0u &&
            rs_panel_master_is_menu_ui_screen(g_ui_current_screen_id) == 0u &&
            Warning_IsProcessDelayActive() == 0u &&
            Warning_GetLastUiBuildCount() != 0u) {
            if (s_warn_cache_reset_ms == 0u || (now_ms - s_warn_cache_reset_ms) >= 1000u) {
                s_warn_cache_reset_ms = now_ms;
                Warning_ResetPanelUiCache();
            }
        } else {
            s_warn_cache_reset_ms = 0u;
        }
    }

    panel = &master->panels[master->round_robin_idx % master->panel_count];
    master->round_robin_idx = (uint8_t)((master->round_robin_idx + 1u) % master->panel_count);

    if (!panel->cfg.enabled) {
        return;
    }

    if ((now_ms - panel->last_rx_ms) > panel->watchdog_ms && panel->last_rx_ms != 0u) {
        panel->link_state = PANEL_LINK_CAPS_PENDING;
        panel->caps_valid = 0u;
    }

    if (panel->link_state == PANEL_LINK_CAPS_PENDING || panel->caps_valid == 0u) {
        if (s_last_caps_req_ms != 0u && (now_ms - s_last_caps_req_ms) < RS_PANEL_CAPS_RETRY_MS) {
            return;
        }
        s_last_caps_req_ms = now_ms;
        g_rs_master_dbg.caps_req_tx++;
        (void)RsBus_SendFrame(&master->bus,
                              panel->cfg.addr,
                              master->next_seq++,
                              0u,
                              RS_PANEL_CMD_CAPS_REQ,
                              0,
                              0u);
        return;
    }

    if ((now_ms - panel->last_poll_ms) < panel->cfg.poll_ms) {
        return;
    }

    if (panel->ack_wait_active != 0u) {
        if ((int32_t)(now_ms - panel->ack_deadline_ms) >= 0) {
            if (panel->ack_retries_left == 0u || panel->pending_ui_stream_len == 0u) {
                if (panel->journal_fault_latched == 0u) {
                    panel->journal_fault_latched = 1u;
                    rs_panel_master_update_journal_fault_mask();
                    rs_panel_master_log_journal_link(panel->cfg.addr, 0u);
                }
                panel->ack_wait_active = 0u;
                panel->pending_ack_seq = 0u;
                panel->ack_deadline_ms = 0u;
                panel->pending_ui_stream_len = 0u;
            } else {
                panel->ack_retries_left--;
                rs_panel_master_send_stream_to_panel(master,
                                                     panel,
                                                     panel->pending_ui_stream,
                                                     panel->pending_ui_stream_len,
                                                     1u,
                                                     1u);
            }
        }
        return;
    }

    if (MenuUi_IsConfigSessionActive()) {
        rs_panel_master_send_config_status_to_ready_panels(master);
    }
    {
        /* После окончания сессии шлём ещё раз, чтобы панель скрыла значок. */
        static uint8_t s_last_wifi_session_sent = 0u;
        uint8_t session = EspManager_IsWifiSessionActive();
        if (g_ui_current_screen_id == RS_PANEL_SCREEN_MENU_CONNECTION ||
            session != 0u ||
            s_last_wifi_session_sent != 0u) {
            rs_panel_master_send_connection_status_to_ready_panels(master);
            s_last_wifi_session_sent = session;
        }
    }

    req.flags = 0u;
    req.ack_seq = panel->last_tx_seq;
    payload_len = RsPanel_EncodePollReq(payload, sizeof(payload), &req);
    if (payload_len == 0u) {
        return;
    }

    panel->last_poll_ms = now_ms;
    panel->last_tx_seq = master->next_seq;
    g_rs_master_dbg.poll_req_tx++;
    (void)RsBus_SendFrame(&master->bus,
                          panel->cfg.addr,
                          master->next_seq++,
                          0u,
                          RS_PANEL_CMD_POLL,
                          payload,
                          payload_len);
}

uint8_t RsPanelMaster_InjectRawRsFrame(const uint8_t *frame, uint16_t frame_len)
{
    RsBusFrameView view;
    uint16_t consumed = 0u;

    if (g_active_master == 0 || frame == 0 || frame_len == 0u) {
        return 0u;
    }
    if (RsBus_FrameDecode(frame, frame_len, &view, &consumed) == 0u) {
        return 0u;
    }
    /* Только master→slave (DIR=0): команды ПО на панель/бутлоадер. */
    if ((view.flags & RS_BUS_FLAG_DIR) != 0u) {
        return 0u;
    }
    if (RsBus_SendRaw(&g_active_master->bus, frame, frame_len) != HAL_OK) {
        return 0u;
    }
    return 1u;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (g_active_master == 0 || huart == 0 || size == 0u) {
        return;
    }
    if (huart != g_active_master->bus.uart) {
        return;
    }

    RsPanelMasterDebug_OnRxDma(size);
    RsPanelMaster_OnRxBytes(g_active_master, g_active_master->bus.rx_dma_buf, size);
    (void)HAL_UARTEx_ReceiveToIdle_DMA(huart,
                                       g_active_master->bus.rx_dma_buf,
                                       sizeof(g_active_master->bus.rx_dma_buf));
    if (huart->hdmarx != 0) {
        __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);
    }
}
