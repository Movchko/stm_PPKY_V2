#include "rs_panel_master_debug.h"

#include "main.h"

extern UART_HandleTypeDef huart1;

volatile RsPanelMasterDbg g_rs_master_dbg;

void RsPanelMasterDebug_OnRxDma(uint16_t nbytes)
{
    g_rs_master_dbg.rx_dma_events++;
    g_rs_master_dbg.rx_raw_bytes += nbytes;
}

void RsPanelMasterDebug_Timer10ms(void)
{
    g_rs_master_dbg.uart_rx_state = (uint8_t)huart1.RxState;
    g_rs_master_dbg.rx_arm_ok = (huart1.RxState == HAL_UART_STATE_BUSY_RX) ? 1u : 0u;
}
