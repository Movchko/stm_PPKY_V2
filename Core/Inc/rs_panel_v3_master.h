#ifndef RS_PANEL_V3_MASTER_H
#define RS_PANEL_V3_MASTER_H

#include <stdint.h>
#include "rs_panel_protocol_v3.h"
#include "rs_panel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

void RsPanelV3Master_Init(void);
void RsPanelV3Master_OnBoot(uint32_t boot_ms);

void RsPanelV3Master_BuildPoll(RsPanelV3Poll *out, uint8_t panel_idx, uint32_t now_ms);

void RsPanelV3Master_OnRsp(uint8_t panel_idx, const RsPanelV3Rsp *rsp, uint32_t now_ms);
/* Fire/journal из RSP — только main (не UART IRQ). Звать в начале Process10ms. */
void RsPanelV3Master_ProcessDeferred(uint32_t now_ms);

uint8_t RsPanelV3Master_IsSysReady(void);
uint32_t RsPanelV3Master_GetBootAgeMs(void);
uint16_t RsPanelV3Master_EncodePollToBuf(uint8_t *dst, uint16_t dst_size,
                                        uint8_t panel_idx, uint32_t now_ms);
/* Короткий POLL только SYS. panel_idx=0xFF — probe без слота (SOUND_ON если маска≠0). */
uint16_t RsPanelV3Master_EncodeSysOnlyPoll(uint8_t *dst, uint16_t dst_size,
                                           uint8_t panel_idx);

void RsPanelV3Master_SetFaultSnapshot(const RsPanelV3FaultEvtItem *items, uint16_t count);
RsPanelV3FaultEvtItem *RsPanelV3Master_FaultItemsWritable(uint16_t *max_count);
void RsPanelV3Master_CommitFaultSnapshot(uint16_t count);
void RsPanelV3Master_RequestCatalogResync(void);
/* Каталог только для одной панели (hot-plug) — не сбрасывать соседние слоты. */
void RsPanelV3Master_RequestCatalogResyncPanel(uint8_t panel_idx);
/* Сброс last_time_ms → TIME в ближайшем POLL (после CAPS/reboot панели). */
void RsPanelV3Master_ForceTimeSyncPanel(uint8_t panel_idx);
/* Сброс last_tx ZONES → следующий POLL обязан отдать снимок (пуски с CAN/панели). */
void RsPanelV3Master_InvalidateFireZonesTx(void);
/* Сдвиг s_ctx[] при prune слота (иначе индекс панелей и ctx разъезжаются). */
void RsPanelV3Master_RemovePanelSlot(uint8_t panel_idx);

uint8_t RsPanelV3Master_IsV3PollActive(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_V3_MASTER_H */
