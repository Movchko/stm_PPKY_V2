/*
 * event_log_catalog.h
 *
 * Каталог кодов событий: уровень логирования и флаг debug-only.
 */

#ifndef INC_EVENT_LOG_CATALOG_H_
#define INC_EVENT_LOG_CATALOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
	EVENT_LOG_LEVEL_CRITICAL = 0,
	EVENT_LOG_LEVEL_GENERAL  = 1
} EventLogLevel_t;

typedef enum {
	EVENT_LOG_MASTER_START           = 1,
	EVENT_LOG_MASTER_STOP            = 2,
	EVENT_LOG_SYSTEM_START_OK        = 3,
	EVENT_LOG_DEVICE_MISSING         = 4,
	EVENT_LOG_DEVICE_FOUND           = 5,
	EVENT_LOG_CONFIG_MISMATCH        = 6,
	EVENT_LOG_TELEMETRY              = 7,
	EVENT_LOG_DEVICE_FAULT           = 8,
	EVENT_LOG_FIRE_DETECTED          = 9,
	EVENT_LOG_EXTINGUISH_START       = 10,
	EVENT_LOG_EXTINGUISH_FORCE_STOP  = 11,
	EVENT_LOG_EXTINGUISH_COMPLETE    = 12,
	EVENT_LOG_EXTINGUISH_INCOMPLETE  = 13,
	EVENT_LOG_PANEL_BUTTON           = 14,
	EVENT_LOG_HOST_LINK              = 15,
	EVENT_LOG_CONFIG_APPLY_OK        = 16,
	EVENT_LOG_CONFIG_APPLY_FAIL      = 17,
	EVENT_LOG_SOUND_TOGGLE           = 18,
	EVENT_LOG_FIRE_MODE_CHANGE       = 19,
	EVENT_LOG_TELEMETRY_SAMPLE       = 20,
	EVENT_LOG_FIRE_RESET             = 21,
	EVENT_LOG_MCU_SAVED              = 22,
	EVENT_LOG_PANEL_BTN_PRESS        = 23,
	EVENT_LOG_COUNTDOWN_PAUSE        = 24,
	EVENT_LOG_COUNTDOWN_RESUME       = 25,
	EVENT_LOG_CONFIG_SAVED           = 26,
	EVENT_LOG_ZONE_NAME              = 27,
} EventLogCode_t;

typedef struct {
	uint16_t         code;
	EventLogLevel_t  level;
	uint8_t          debug_only;
} EventLogDescriptor_t;

extern const EventLogDescriptor_t g_event_log_catalog[];
extern const uint32_t g_event_log_catalog_count;

const EventLogDescriptor_t *EventLogCatalog_Find(uint16_t code);

#ifdef __cplusplus
}
#endif

#endif /* INC_EVENT_LOG_CATALOG_H_ */
