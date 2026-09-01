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
    volatile uint32_t caps_req_tx;
    volatile uint32_t rsp_caps_rx;
    volatile uint32_t rsp_caps_decode_fail;
    volatile uint32_t rsp_poll_rx;
    volatile uint32_t poll_req_tx;
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
} RsPanelMasterDbg;

extern volatile RsPanelMasterDbg g_rs_master_dbg;

void RsPanelMasterDebug_OnRxDma(uint16_t nbytes);
void RsPanelMasterDebug_Timer10ms(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_MASTER_DEBUG_H */
