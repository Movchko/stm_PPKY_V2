#ifndef INC_MENU_UI_H_
#define INC_MENU_UI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	MENU_CFG_STATE_IDLE = 0,
	MENU_CFG_STATE_RECEIVING,
	MENU_CFG_STATE_APPLYING,
	MENU_CFG_STATE_SUCCESS
} MenuCfgState;

void MenuUi_SetConfigSession(uint8_t active);
uint8_t MenuUi_IsConfigSessionActive(void);

void MenuUi_SetMainScreenActive(uint8_t active);
uint8_t MenuUi_IsMainScreenActive(void);

void Esp32_SetEnabled(uint8_t enabled);
uint8_t Esp32_IsEnabled(void);

void MenuConfig_Reset(void);
void MenuConfig_OnWordReceived(uint16_t word_num);
void MenuConfig_OnSaveCompleted(void);
void MenuConfig_OnApplySuccess(void);

MenuCfgState MenuConfig_GetState(void);
uint8_t MenuConfig_GetPercent(void);

void MenuUi_SetMcuDetailSlot(uint8_t cfg_slot);
uint8_t MenuUi_GetMcuDetailSlot(void);

/* Remote-controlled menu selection (panel-driven UI state).
 * Master uses it as part of UI session state machine; panel uses it for highlight. */
void MenuUi_SetMenuSelected(uint16_t selected);
uint16_t MenuUi_GetMenuSelected(void);
void MenuUi_SetMenuNItems(uint8_t n_items);
uint8_t MenuUi_GetMenuNItems(void);

#ifdef __cplusplus
}
#endif

#endif /* INC_MENU_UI_H_ */
