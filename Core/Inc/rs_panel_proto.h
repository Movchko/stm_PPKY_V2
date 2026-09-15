#ifndef RS_PANEL_PROTO_H
#define RS_PANEL_PROTO_H

#include "panel_state.h"
#include "rs_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RS_PANEL_ADDR_FSM_IDLE = 0,
    RS_PANEL_ADDR_FSM_WAIT_BOOT,
    RS_PANEL_ADDR_FSM_DISCOVER,
    RS_PANEL_ADDR_FSM_ASSIGN,
    RS_PANEL_ADDR_FSM_SETTLE
} RsPanelAddrFsm;

typedef struct {
    uint32_t uid0;
    uint32_t uid1;
    uint32_t uid2;
    uint8_t current_addr;
    uint8_t flags;
    uint8_t assigned_addr;
    uint8_t seen;
} RsPanelDiscoverEntry;

typedef struct {
    RsBusContext bus;
    PanelState panels[RS_PANEL_MAX_PANELS];
    uint8_t panel_count;
    uint8_t next_seq;
    uint8_t round_robin_idx;
    /* Автораздача адресов / коллизии. */
    RsPanelAddrFsm addr_fsm;
    uint32_t addr_fsm_deadline_ms;
    uint8_t addr_assign_idx;
    uint8_t discover_count;
    RsPanelDiscoverEntry discover[RS_PANEL_MAX_PANELS];
    uint8_t collision_pending;
    uint8_t first_boot_discover_done;
} RsPanelMaster;

void RsPanelMaster_Init(RsPanelMaster *master,
                        UART_HandleTypeDef *uart,
                        GPIO_TypeDef *de_port,
                        uint16_t de_pin);
void RsPanelMaster_Process10ms(RsPanelMaster *master, uint32_t now_ms);
void RsPanelMaster_OnRxBytes(RsPanelMaster *master, const uint8_t *data, uint16_t len);
void RsPanelMaster_LoadDefaultConfig(RsPanelMaster *master);
/* Синхронизировать текущее состояние Beeper на все готовые панели. */
void RsPanelMaster_PushSound(void);
/* Прокидка сырого RS-кадра с WiFi/ПО (BSU_PKT_TYPE_ESP_UART) на шину панелей. */
uint8_t RsPanelMaster_InjectRawRsFrame(const uint8_t *frame, uint16_t frame_len);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_PROTO_H */
