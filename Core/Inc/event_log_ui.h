/*
 * event_log_ui.h — форматирование записи журнала для UI.
 */

#ifndef INC_EVENT_LOG_UI_H_
#define INC_EVENT_LOG_UI_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "event_logger.h"

#define EVENT_LOG_UI_HEADER_LEN  40u
#define EVENT_LOG_UI_TITLE_LEN   24u
#define EVENT_LOG_UI_DETAIL_LEN  384u

typedef struct {
	char header[EVENT_LOG_UI_HEADER_LEN];
	char title[EVENT_LOG_UI_TITLE_LEN];
	char detail[EVENT_LOG_UI_DETAIL_LEN];
} EventLogUiLines_t;

#define EVENT_LOG_UI_TIER  0u

void EventLogUi_FormatRecord(const EventLogRecord_t *rec,
			     uint32_t display_index_1based,
			     uint32_t count,
			     EventLogUiLines_t *out);
void EventLogUi_FormatEmpty(EventLogUiLines_t *out);

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOG_UI_H_ */
