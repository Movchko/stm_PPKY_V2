#include "panel_state.h"

#include <string.h>
#include "device_config.h"

static uint8_t rs_btn_type_to_but(uint8_t type)
{
    /* Local mapping (см. stm_PPKY_V2/Core/Inc/button.h):
     * BUT_ESC=0, BUT_UP=1, BUT_DOWN=2, BUT_ENTER=3,
     * BUT_FORCE=4 (START_ALL), BUT_STOP=5, BUT_FIRE=6 (START_SP)
     */
    switch (type) {
    case RS_PANEL_BTN_ESC:        return 0u;
    case RS_PANEL_BTN_UP:         return 1u;
    case RS_PANEL_BTN_DOWN:       return 2u;
    case RS_PANEL_BTN_ENTER:      return 3u;
    case RS_PANEL_BTN_START_ALL:  return 4u;
    case RS_PANEL_BTN_STOP:       return 5u;
    case RS_PANEL_BTN_START_SP:   return 6u;
    default:                      return 0xFFu;
    }
}

void PanelState_Reset(PanelState *state)
{
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
    /* 1 мс: быстрее прежних 10 мс, но с запасом на half-duplex RSP.
     * 0 / каждый тик TIM3@2кГц — ломает связь (POLL раньше ответа панели). */
    state->cfg.poll_ms = 1u;
    /* ACTIVITY панели — 1 Гц. 500 мс давали CAPS-флап при задержке POLL
     * (WARN/LED/SOUND) → PushSound рестартил дежурный звук ~раз в секунду. */
    state->watchdog_ms = 3000u;
    state->remote_btn_mask = 0u;
}

void PanelState_BindConfig(PanelState *state, const PanelConfig *cfg)
{
    if (state == 0 || cfg == 0) {
        return;
    }
    state->cfg = *cfg;
    state->link_state = cfg->enabled ? PANEL_LINK_CAPS_PENDING : PANEL_LINK_OFFLINE;
    if (state->cfg.poll_ms == 0u) {
        state->cfg.poll_ms = 1u;
    }
    if (state->watchdog_ms == 0u) {
        state->watchdog_ms = 3000u;
    }
}

void PanelState_OnCaps(PanelState *state, const RsPanelCaps *caps, uint32_t now_ms)
{
    if (state == 0 || caps == 0) {
        return;
    }

    state->caps = *caps;
    state->caps_valid = 1u;
    state->last_rx_ms = now_ms;
    /* ui_profile = PANEL_TYPE_1/2/3; малые = 2 и 3 (и устаревший 0). */
    state->is_small_panel = PANEL_TYPE_IS_SMALL(caps->ui_profile) ? 1u : 0u;
    /* Запасной признак: в CAPS нет START_ALL ⇒ малая. */
    if (state->is_small_panel == 0u && caps->ui_profile != PANEL_TYPE_1) {
        uint8_t has_start_all = 0u;
        uint8_t i;
        for (i = 0u; i < caps->btn_count && i < RS_PANEL_MAX_CAPS_BUTTONS; i++) {
            if (caps->btn_list[i] == (uint8_t)RS_PANEL_BTN_START_ALL) {
                has_start_all = 1u;
                break;
            }
        }
        if (has_start_all == 0u && caps->btn_count > 0u) {
            state->is_small_panel = 1u;
        }
    }

    if (state->cfg.expected_hw_id != 0u && state->cfg.expected_hw_id != caps->hw_id) {
        state->link_state = PANEL_LINK_BLOCKED;
        return;
    }

    state->link_state = PANEL_LINK_READY;
}

void PanelState_OnPollRsp(PanelState *state, const RsPanelPollRsp *rsp, uint32_t now_ms)
{
    if (state == 0) {
        return;
    }
    if (rsp == 0) {
        return;
    }
    state->last_rx_ms = now_ms;
    if (state->link_state == PANEL_LINK_CAPS_PENDING && state->caps_valid != 0u) {
        state->link_state = PANEL_LINK_READY;
    }

    for (uint8_t i = 0u; i < rsp->evt_count && i < RS_PANEL_MAX_POLL_BTN_EVENTS; i++) {
        uint8_t but = rs_btn_type_to_but(rsp->btn_events[i].type);
        if (but == 0xFFu) {
            continue;
        }

        /* Хост: любое событие ПУСК ОБЩИЙ ⇒ панель большая (согласовано с автоопределением). */
        if (rsp->btn_events[i].type == (uint8_t)RS_PANEL_BTN_START_ALL) {
            state->is_small_panel = 0u;
        }

        uint8_t pressed = (rsp->btn_events[i].level != 0u) ? 1u : 0u;
        if (pressed != 0u) {
            state->remote_btn_mask = (uint8_t)(state->remote_btn_mask | (uint8_t)(1u << but));
        } else {
            state->remote_btn_mask = (uint8_t)(state->remote_btn_mask & (uint8_t)~(uint8_t)(1u << but));
        }
    }
}

uint8_t PanelState_IsReady(const PanelState *state)
{
    return (state != 0 && state->link_state == PANEL_LINK_READY) ? 1u : 0u;
}
