#ifndef RS_PANEL_MASTER_DEBUG_H
#define RS_PANEL_MASTER_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Live Watch на ППКУ v2 (USART1 / RS master). */
typedef struct {
    volatile uint32_t rx_dma_events;
    volatile uint32_t rx_raw_bytes;
    volatile uint32_t rx_frames_ok;
    volatile uint32_t rx_frames_wrong_dir;
    volatile uint32_t rx_frames_wrong_addr;
    /* Почему кадр не собрался (Live: если raw_bytes>0 а frames_ok=0). */
    volatile uint32_t rx_decode_crc_fail;
    volatile uint32_t rx_decode_bad_hdr;
    volatile uint32_t rx_decode_short_hold;
    volatile uint32_t rx_resync_bytes;
    volatile uint32_t caps_req_tx;
    volatile uint32_t rsp_caps_rx;
    volatile uint32_t rsp_caps_decode_fail;
    volatile uint32_t rsp_poll_rx;
    volatile uint32_t poll_req_tx;
    /* Per-slot poll/time (Live Expressions: обе панели получают кадры). */
    volatile uint8_t  panel_count;
    volatile uint8_t  panel_addr[8];
    volatile uint32_t poll_tx_by_panel[8];
    volatile uint32_t time_tlv_tx_by_panel[8];
    /* Bring-up: SYS_READY и возраст boot (мс) — снятие «ПРОВЕРКА». */
    volatile uint8_t  sys_ready;
    volatile uint32_t boot_age_ms;
    /* Inject/bootloader pause: POLL silent while now < inject_pause_until_ms. */
    volatile uint32_t inject_pause_until_ms;
    volatile uint32_t inject_pause_armed_count;
    volatile uint8_t  inject_pause_active;
    volatile uint8_t  last_inject_pause_cmd;
    /* Скан RS-адресов 1..8 @ 2 addr/s. */
    volatile uint32_t addr_scan_tx;
    volatile uint8_t  addr_scan_next;
    volatile uint8_t  addr_scan_probe_addr;
    volatile uint32_t addr_scan_found;
    volatile uint32_t activity_req_tx;
    volatile uint8_t  activity_req_addr;
    /* Multi-panel half-duplex: тишина после TX→RSP; gap POLL на слот. */
    volatile uint32_t bus_quiet_skips;
    volatile uint32_t bus_quiet_cleared_rx;
    volatile uint32_t bus_quiet_timeout_flush;
    volatile uint32_t poll_dominate_defer;
    volatile uint16_t poll_gap_ms_by_panel[8];
    volatile uint16_t poll_gap_max_ms_by_panel[8];
    volatile uint32_t warn_ui_tx;
    volatile uint32_t warn_ui_deliver_fail;
    volatile uint32_t warn_push_skip_cache;
    volatile uint8_t  last_warn_active;
    volatile uint8_t  last_warn_n_items;
    volatile uint8_t  last_warn_build_count;
    volatile uint8_t  panel_caps_valid;
    volatile uint8_t  panel_link_state;
    volatile uint8_t  uart_rx_state;
    volatile uint8_t  rx_arm_ok;
    /* UI events: enqueue in RX IRQ, handle+TX in Process10ms */
    volatile uint32_t ui_evt_enqueued;
    volatile uint32_t ui_evt_handled;
    volatile uint32_t ui_evt_q_overflow;
    volatile uint8_t  last_ui_evt_type;
    volatile uint16_t last_ui_evt_p1;
    volatile uint16_t menu_selected;
    volatile uint16_t ui_screen_id;
    volatile uint32_t menu_list_tx;
    /* Каталог v3: раздельный CRC skip/rewrite (Live Watch). */
    volatile uint32_t catalog_skip_zones;
    volatile uint32_t catalog_skip_devices;
    volatile uint32_t catalog_rewrite_zones;
    volatile uint32_t catalog_rewrite_devices;
    /* Сырые байты последнего RX-события (что реально на линии: 00/FF — шум/обрыв,
     * 5A xx — потерян первый байт 0xA5 на фронте DE, A5 5A — кадр). */
    volatile uint8_t  rx_last_head[16];
    volatile uint16_t rx_last_len;
    /* Первый байт 0xA5 вообще встречался в RX (0 = преамбулы на линии нет). */
    volatile uint32_t rx_a5_seen;
    /* HAL_UART_ErrorCallback → Recover; ErrorCode последней ошибки (FE=2/NE=4/ORE=8). */
    volatile uint32_t rx_err_count;
    volatile uint32_t rx_last_err_code;
    /* RX был не BUSY_RX и перезапущен сторожем из Process10ms. */
    volatile uint32_t rx_rearm_wd;
} RsPanelMasterDbg;

extern volatile RsPanelMasterDbg g_rs_master_dbg;

void RsPanelMasterDebug_OnRxDma(uint16_t nbytes);
void RsPanelMasterDebug_OnRxHead(const uint8_t *data, uint16_t nbytes);
void RsPanelMasterDebug_Timer10ms(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_MASTER_DEBUG_H */
