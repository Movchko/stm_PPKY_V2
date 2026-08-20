#ifndef PANEL_STATE_H
#define PANEL_STATE_H

#include <stdint.h>
#include "rs_panel_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_LINK_OFFLINE = 0,
    PANEL_LINK_CAPS_PENDING = 1,
    PANEL_LINK_READY = 2,
    PANEL_LINK_BLOCKED = 3
} PanelLinkState;

typedef struct {
    uint8_t enabled;
    uint8_t addr;
    uint8_t role;
    uint8_t poll_ms;
    uint16_t expected_hw_id;
} PanelConfig;

typedef struct {
    PanelConfig cfg;
    PanelLinkState link_state;
    RsPanelCaps caps;
    uint8_t caps_valid;
    uint8_t last_tx_seq;
    uint32_t last_rx_ms;
    uint32_t last_poll_ms;
    uint32_t watchdog_ms;
    /* Битмаска нажатых кнопок, агрегированная по событиям POLL от панели. */
    uint8_t remote_btn_mask;
    uint8_t pending_ack_seq;
    uint8_t ack_wait_active;
    uint8_t ack_retries_left;
    uint32_t ack_deadline_ms;
    uint16_t pending_ui_stream_len;
    uint8_t pending_ui_stream[RS_BUS_MAX_PAYLOAD];
    uint8_t journal_fault_latched;
} PanelState;

void PanelState_Reset(PanelState *state);
void PanelState_BindConfig(PanelState *state, const PanelConfig *cfg);
void PanelState_OnCaps(PanelState *state, const RsPanelCaps *caps, uint32_t now_ms);
void PanelState_OnPollRsp(PanelState *state, const RsPanelPollRsp *rsp, uint32_t now_ms);
uint8_t PanelState_IsReady(const PanelState *state);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_STATE_H */
