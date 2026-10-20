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
    /* Конец последнего цикла DISCOVER/ASSIGN — cooldown повторных коллизий. */
    uint32_t last_addr_cycle_ms;
} RsPanelMaster;

void RsPanelMaster_Init(RsPanelMaster *master,
                        UART_HandleTypeDef *uart,
                        GPIO_TypeDef *de_port,
                        uint16_t de_pin);
void RsPanelMaster_Process10ms(RsPanelMaster *master, uint32_t now_ms);
void RsPanelMaster_OnRxBytes(RsPanelMaster *master, const uint8_t *data, uint16_t len);
/** Из HAL_UART_ErrorCallback (USART1): сброс ORE и перезапуск RX DMA. */
void RsPanelMaster_OnUartError(UART_HandleTypeDef *huart);
/** Из HAL_UART_RxCpltCallback (USART1): DMA RX буфер полный. */
void RsPanelMaster_OnRxDmaComplete(void);
void RsPanelMaster_LoadDefaultConfig(RsPanelMaster *master);
/* UI_DATA CONNECTION_STATUS (wifi_on / rs485_on) — отложить TX в Process10ms. */
void RsPanelMaster_PushConnectionStatus(void);
/* 1 = подключена малая панель (нет ПУСК ОБЩИЙ). */
uint8_t RsPanelMaster_IsSmallPanel(void);
/* Прокидка сырого RS-кадра с WiFi/ПО (BSU_PKT_TYPE_ESP_UART) на шину панелей. */
uint8_t RsPanelMaster_InjectRawRsFrame(const uint8_t *frame, uint16_t frame_len);
/* Зеркало CAN→RS485 (cmd 0xF7/0xF8): default OFF. */
void RsPanelMaster_SetCanMirrorEnable(uint8_t enable);
uint8_t RsPanelMaster_IsCanMirrorEnabled(void);
/* Поставить полный BSU CAN-кадр (22 байта) в очередь на RS (если mirror ON). */
void RsPanelMaster_PushCanMirrorBsu(const uint8_t *bsu_pkt, uint16_t len);
/* RS-адрес панели по slot (panel_idx), 0xFF если нет. */
uint8_t RsPanelMaster_GetPanelAddr(uint8_t panel_idx);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_PROTO_H */
