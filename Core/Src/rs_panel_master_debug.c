#include "rs_panel_master_debug.h"

#include "main.h"
#include "rs_panel_v3_master.h"

extern UART_HandleTypeDef huart1;

volatile RsPanelMasterDbg g_rs_master_dbg;

void RsPanelMasterDebug_OnRxDma(uint16_t nbytes)
{
    g_rs_master_dbg.rx_dma_events++;
    g_rs_master_dbg.rx_raw_bytes += nbytes;
}

void RsPanelMasterDebug_OnRxHead(const uint8_t *data, uint16_t nbytes)
{
    uint16_t i;

    if (data == 0) {
        return;
    }
    g_rs_master_dbg.rx_last_len = nbytes;
    for (i = 0u; i < (uint16_t)sizeof(g_rs_master_dbg.rx_last_head); i++) {
        g_rs_master_dbg.rx_last_head[i] = (i < nbytes) ? data[i] : 0u;
    }
    for (i = 0u; i < nbytes; i++) {
        if (data[i] == 0xA5u) {
            g_rs_master_dbg.rx_a5_seen++;
            break;
        }
    }
}

void RsPanelMasterDebug_Timer10ms(void)
{
    g_rs_master_dbg.uart_rx_state = (uint8_t)huart1.RxState;
    g_rs_master_dbg.rx_arm_ok = (huart1.RxState == HAL_UART_STATE_BUSY_RX) ? 1u : 0u;
    g_rs_master_dbg.sys_ready = RsPanelV3Master_IsSysReady();
    g_rs_master_dbg.boot_age_ms = RsPanelV3Master_GetBootAgeMs();
    /* inject_pause_* / rx_decode_* обновляются в rs_panel_proto.c. */
}
