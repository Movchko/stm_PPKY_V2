#ifndef RS_PANEL_PROTO_H
#define RS_PANEL_PROTO_H

#include "panel_state.h"
#include "rs_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    RsBusContext bus;
    PanelState panels[RS_PANEL_MAX_PANELS];
    uint8_t panel_count;
    uint8_t next_seq;
    uint8_t round_robin_idx;
} RsPanelMaster;

void RsPanelMaster_Init(RsPanelMaster *master,
                        UART_HandleTypeDef *uart,
                        GPIO_TypeDef *de_port,
                        uint16_t de_pin);
void RsPanelMaster_Process10ms(RsPanelMaster *master, uint32_t now_ms);
void RsPanelMaster_OnRxBytes(RsPanelMaster *master, const uint8_t *data, uint16_t len);
void RsPanelMaster_LoadDefaultConfig(RsPanelMaster *master);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_PROTO_H */
