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

uint8_t RsPanelV3Master_IsSysReady(void);
uint16_t RsPanelV3Master_EncodePollToBuf(uint8_t *dst, uint16_t dst_size,
                                        uint8_t panel_idx, uint32_t now_ms);

void RsPanelV3Master_SetFaultSnapshot(const RsPanelV3FaultEvtItem *items, uint16_t count);
RsPanelV3FaultEvtItem *RsPanelV3Master_FaultItemsWritable(uint16_t *max_count);
void RsPanelV3Master_CommitFaultSnapshot(uint16_t count);
void RsPanelV3Master_RequestCatalogResync(void);

uint8_t RsPanelV3Master_IsV3PollActive(void);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_V3_MASTER_H */
