#include "power_control.h"
#include "app.hpp"
#include "button.h"
#include "beeper.h"
#include "device_config.h"
#include "led.h"
#include "backend.h"
#include "service.h"
#include "config_sync.hpp"
#include "config_monitor.h"
#include "config_ign_block_sync.h"
#include "can_bus.h"
#include "tick_time.h"
#include "device_dpt.hpp"
#include "device_igniter.hpp"
#include "menu_ui.h"
#include "rtc_cache.h"
#include "event_log.h"
#include "log_transport.h"
#include "esp_manager.h"
#include "rs_panel_proto.h"
#include "fire.h"
#include "warning.h"

#define WARNING_TITLE_LEN 24

struct PPKYCfg PPKYConfig;       // локальная (рабочая) конфигурация
struct PPKYCfg SavedPPKYConfig; // копия сохранённой конфигурации из Flash

extern SPIF_HandleTypeDef hFlash;

PControl *Power[POWER_NUM_CHANNELS];



ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
uint8_t g_active_devices_count = 0;
uint8_t g_mku_mismatch_flag = 0;
static uint8_t g_cfg_crc_mismatch_flag = 0u;
static uint32_t g_position_fault_mask = 0u;
static constexpr uint8_t RELAY_AUTO_MAX_TRACK = 64u;
static uint8_t g_relay_fire_zone_active[ZONE_NUMBER];
static uint8_t g_relay_start_zone_active[ZONE_NUMBER];

typedef struct {
	uint8_t valid;
	uint8_t zone;
	uint8_t h_adr;
	uint8_t l_adr;
	uint8_t target_state;
} RelayAutoTrack;

static RelayAutoTrack g_relay_auto_track[RELAY_AUTO_MAX_TRACK];

/* --- Механизм автоматической установки адресов по команде 10 --- */
typedef enum {
	ADDR_AUTO_IDLE = 0,
	ADDR_AUTO_WAIT_AFTER_STOP,
	ADDR_AUTO_WAIT_AFTER_SET
} AddrAutoState;

static AddrAutoState g_addr_auto_state = ADDR_AUTO_IDLE;
static uint32_t g_addr_auto_phase_start_ms = 0;



GPIO_TypeDef   *POWER_ST_PORT[2] = {ST1_MK_GPIO_Port, ST2_MK_GPIO_Port};
uint16_t  		POWER_ST_PIN[2] = {ST1_MK_Pin, ST2_MK_Pin};
GPIO_TypeDef   *POWER_OUT_PORT[2] = {KEY_1_GPIO_Port, KEY_2_GPIO_Port};
uint16_t  		POWER_OUT_PIN[2] = {KEY_1_Pin, KEY_2_Pin};

bool isAppInit = 0;

extern Device BoardDevicesList[];
extern uint8_t nDevs;


extern int32_t CHANNEL_VAL[NUM_ADC_CHANNEL];

uint8_t status_sec_cnt = 0;

uint8_t uart_send_buf[32] = {0};

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
static RsPanelMaster g_rs_panel_master;

extern "C" __attribute__((weak)) void App_OnFireUiUpdate(uint8_t active, uint8_t mode,
							      uint8_t remaining_s, uint8_t n_zones,
							      char (*zone_names)[ZONE_NAME_SIZE + 1])
{
	(void)active;
	(void)mode;
	(void)remaining_s;
	(void)n_zones;
	(void)zone_names;
}

extern "C" __attribute__((weak)) void App_OnWarningUiUpdate(uint8_t active, uint8_t n_items,
								 char (*big_titles)[WARNING_TITLE_LEN],
								 char (*details)[ZONE_NAME_SIZE + 1])
{
	(void)active;
	(void)n_items;
	(void)big_titles;
	(void)details;
}

RTC_TimeTypeDef cur_time = {0};
RTC_DateTypeDef cur_date;

static void AddrAuto_ClearActiveDevices(void) {
	memset(g_active_devices, 0, sizeof(g_active_devices));
	memset(g_relay_auto_track, 0, sizeof(g_relay_auto_track));
	memset(g_relay_fire_zone_active, 0, sizeof(g_relay_fire_zone_active));
	memset(g_relay_start_zone_active, 0, sizeof(g_relay_start_zone_active));
	g_active_devices_count = 0;
	g_mku_mismatch_flag = 0;
}

static void AddrAuto_Start(void) {
	// Широковещательно: остановить ретрансляцию на CAN
	uint8_t data[7] = {0};
	data[0] = 1u; // 1 = стоп
	SendAllMessage(ServiceCmd_StopStartReTranslate, data, SEND_NOW, BUS_CAN12);

	g_addr_auto_state = ADDR_AUTO_WAIT_AFTER_STOP;
	g_addr_auto_phase_start_ms = HAL_GetTick();
}

static void AddrAuto_Process(uint32_t now_ms) {
	switch (g_addr_auto_state) {
	case ADDR_AUTO_IDLE:
		break;
	case ADDR_AUTO_WAIT_AFTER_STOP:
		// ждём  после остановки ретрансляции, затем шлём CircSetAdr
		if ((now_ms - g_addr_auto_phase_start_ms) >= 2000u) {
			uint8_t data[7] = {0};
			data[0] = 1u; // новый адрес = 1
			SendAllMessage(ServiceCmd_CircSetAdr, data, SEND_NOW, BUS_CAN0);

			g_addr_auto_state = ADDR_AUTO_WAIT_AFTER_SET;
			g_addr_auto_phase_start_ms = now_ms;
		}
		break;
	case ADDR_AUTO_WAIT_AFTER_SET:
		// ещё 100 мс, потом включаем ретрансляцию, очищаем список устройств
		// и перезапускаем питание на обоих каналах
		if ((now_ms - g_addr_auto_phase_start_ms) >= 5000u) {
			//uint8_t data[7] = {0};
			//data[0] = 0u; // 0 = старт ретрансляции
			//SendAllMessage(ServiceCmd_StopStartReTranslate, data, SEND_NOW, BUS_CAN12);

			// адреса изменились — очищаем список активных устройств, он будет заполнен заново
			AddrAuto_ClearActiveDevices();

			// Перезапустить питание МКУ на обоих каналах (короткое выключение/включение)
			for (uint8_t i = 0; i < POWER_NUM_MKU_CH; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, false);
				}
			}
			HAL_Delay(500);
			for (uint8_t i = 0; i < POWER_NUM_MKU_CH; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, true);
				}
			}

			g_addr_auto_state = ADDR_AUTO_IDLE;
		}
		break;
	}
}


typedef enum {
	MKU_RESET_IDLE = 0,
	MKU_RESET_WAIT_DELAY,
	MKU_RESET_WAIT_POWERON
} MkuResetState;

static MkuResetState g_mku_reset_state = MKU_RESET_IDLE;
static uint32_t g_mku_reset_deadline_ms = 0u;
static constexpr uint32_t MKU_RESET_AFTER_APPLY_DELAY_MS = 5000u;
static constexpr uint32_t MKU_RESET_POWER_OFF_HOLD_MS = 1000u;
static uint8_t g_mku_reset_prev_enable[POWER_NUM_MKU_CH] = {0u};
static uint8_t g_mku_reset_prev_valid = 0u;

static void MkuHardReset_StartNow(uint32_t now_ms)
{
	/* Замораживаем желаемое состояние выходов, чтобы PControl::Process()
	 * не включил питание обратно раньше окончания окна OFF. */
	for (uint8_t i = 0u; i < POWER_NUM_MKU_CH; i++) {
		if (Power[i] != nullptr) {
			g_mku_reset_prev_enable[i] = Power[i]->GetEnable() ? 1u : 0u;
			Power[i]->SetEnable(false);
		} else {
			g_mku_reset_prev_enable[i] = 0u;
		}
	}
	g_mku_reset_prev_valid = 1u;

	for (uint8_t i = 0u; i < POWER_NUM_MKU_CH; i++) {
		if (Power[i] != nullptr) {
			Power[i]->PControlSetOut(i, false);
		}
	}
	g_mku_reset_state = MKU_RESET_WAIT_POWERON;
	g_mku_reset_deadline_ms = now_ms + MKU_RESET_POWER_OFF_HOLD_MS;
}

static void MkuHardReset_ScheduleAfterApply(void)
{
	g_mku_reset_state = MKU_RESET_WAIT_DELAY;
	g_mku_reset_deadline_ms = HAL_GetTick() + MKU_RESET_AFTER_APPLY_DELAY_MS;
}

static void MkuHardReset_Process(uint32_t now_ms)
{
	switch (g_mku_reset_state) {
	case MKU_RESET_IDLE:
		break;
	case MKU_RESET_WAIT_DELAY:
		if ((int32_t)(now_ms - g_mku_reset_deadline_ms) >= 0) {
			MkuHardReset_StartNow(now_ms);
		}
		break;
	case MKU_RESET_WAIT_POWERON:
		if ((int32_t)(now_ms - g_mku_reset_deadline_ms) >= 0) {
			if (g_mku_reset_prev_valid != 0u) {
				for (uint8_t i = 0u; i < POWER_NUM_MKU_CH; i++) {
					if (Power[i] != nullptr) {
						Power[i]->SetEnable(g_mku_reset_prev_enable[i] != 0u);
					}
				}
			}
			for (uint8_t i = 0u; i < POWER_NUM_MKU_CH; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, true);
				}
			}
			g_mku_reset_prev_valid = 0u;
			g_mku_reset_state = MKU_RESET_IDLE;
		}
		break;
	default:
		g_mku_reset_state = MKU_RESET_IDLE;
		break;
	}
}

extern "C" void App_OnConfigApplySuccess(void)
{
	/* Headless-friendly: без вызова TouchGFX UI. */
	MenuConfig_OnApplySuccess();
	MkuHardReset_ScheduleAfterApply();
}

void USBSendData(uint8_t *Buf) {};

void PPKY_GetLastPowerOnDate(RTC_DateTypeDef *out_date, RTC_TimeTypeDef *out_time)
{
	uint32_t v = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);

	/* Формат BKP-регистра: 0xMMDDHHmm (BCD) */
	if (out_date != nullptr) {
		RTC_DateTypeDef d = {};
		d.Month = (uint8_t)((v >> 24) & 0xFFu);  // BCD-месяц
		d.Date  = (uint8_t)((v >> 16) & 0xFFu);  // BCD-день
		// Год и день недели не сохраняем
		*out_date = d;
	}

	if (out_time != nullptr) {
		RTC_TimeTypeDef t = {};
		t.Hours   = (uint8_t)((v >> 8)  & 0xFFu);  // BCD-часы
		t.Minutes = (uint8_t)((v >> 0)  & 0xFFu);  // BCD-минуты
		// Секунды не сохраняем
		*out_time = t;
	}
}

void CommandCB(uint8_t Dev, uint8_t Command, uint8_t *Parameters) {
	(void)Dev;
	switch(Command) {
	case 10: {
		// Запуск механизма установки адресов (работаем только по CAN0).
		// Механизм неблокирующий: шаги выполняются в AddrAuto_Process() по таймеру.
		if (g_addr_auto_state == ADDR_AUTO_IDLE) {
			AddrAuto_Start();
		}
	}break;
	case 11: {
		/* Сохранить состояние системы + прочитать полные конфиги всех активных МКУ. */
		ConfigSync_StartReadAllAndSave();
	}break;
	case 12: {
		/* Перезапуск устройств на шине.
		 * Parameters[0]: 0 = мягкий (софт‑ресет), 1 = жёсткий (хард‑ресет). */
		uint8_t mode = Parameters ? Parameters[0] : 0u;
		if (mode == 0u) {
			/* Софт‑ресет: широковещательный ServiceCmd_ResetMCU по обеим шинам. */
			uint8_t data[7] = {0};
			SendAllMessage(ServiceCmd_ResetMCU, data, SEND_NOW, BUS_CAN12);
		} else {
			/* Хард‑ресет: отключить питание на 1 с и снова включить. */
			for (uint8_t i = 0; i < POWER_NUM_CHANNELS; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, false);
				}
			}
			HAL_Delay(1000);
			for (uint8_t i = 0; i < POWER_NUM_CHANNELS; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, true);
				}
			}
		}
	}break;
	case 13: {
		/* Установка режима пуска:
		 * Parameters[0] = 0 (auto), 1 (автономный), 2 (manual). */
		if (Parameters != nullptr) {
			if (Parameters[0] <= 2u) {
				PPKYConfig.fire_mode = Parameters[0];
			}
		}
	}break;
	case 14: {
		/* Запустить сверку конфигов ППКУ <-> МКУ по CRC. */
		ConfigSync_StartVerify();
	}break;
	case 15: {
		/* Применить конфиг-образ из ППКУ ко всем МКУ и проверить по CRC. */
		ConfigSync_StartApply();
	}break;

	default: break;
	}


}



static void UartSendPpkyTime(void) {
	// Формат: "PPKY " + 6 цифр BCD (HHMMSS) + "\r\n"
	// Системное время берём из RTC
	// BCD поля RTC: 0x23 → "23"
	uint8_t h = cur_time.Hours;
	uint8_t m = cur_time.Minutes;
	uint8_t s = cur_time.Seconds;

	uint8_t buf[16];
	buf[0] = 'P';
	buf[1] = 'P';
	buf[2] = 'K';
	buf[3] = 'Y';
	buf[4] = ' ';
	buf[5] = ((h >> 4) & 0x0F) + '0';
	buf[6] = (h & 0x0F) + '0';
	buf[7] = ((m >> 4) & 0x0F) + '0';
	buf[8] = (m & 0x0F) + '0';
	buf[9] = ((s >> 4) & 0x0F) + '0';
	buf[10] = (s & 0x0F) + '0';
	buf[11] = '\r';
	buf[12] = '\n';

	// Копируем в глобальный uart_send_buf и шлём по USART2 раз в секунду
	memset(uart_send_buf, 0, sizeof(uart_send_buf));
	memcpy(uart_send_buf, buf, 13);
	HAL_UART_Transmit(&huart2, uart_send_buf, 13, 10);
}


void AppSetStatus() {

	int32_t power_v = CHANNEL_VAL[4] / 1000;   // шаг 1В
	int32_t rpower_v = CHANNEL_VAL[0] / 1000;  // шаг 1В
	if (power_v < 0) power_v = 0;
	if (power_v > 255) power_v = 255;
	if (rpower_v < 0) rpower_v = 0;
	if (rpower_v > 255) rpower_v = 255;
	uint8_t power = (uint8_t)power_v;
	uint8_t Rpower = (uint8_t)rpower_v;
	uint8_t current1 = (CHANNEL_VAL[1] / 50) & 0xFF; // шаг 50мА
	uint8_t current2 = (CHANNEL_VAL[2] / 50) & 0xFF;
	uint8_t status_data[7] = {
			status_sec_cnt,
			power,
			Rpower,
			current1,
			current2,
			0,
			0
	};
	/* Dev=0 — сама плата ППКУ, отправляем через backend */
	SendMessage(0, 0, status_data, SEND_NOW, BUS_CAN12);

	if (HAL_RTC_GetTime(&hrtc, &cur_time, RTC_FORMAT_BCD) != HAL_OK) {
			return;
	}
	if (HAL_RTC_GetDate(&hrtc, &cur_date, RTC_FORMAT_BCD) != HAL_OK) {
			return;
	}
	uint8_t date[7] = {
			cur_time.Hours,
			cur_time.Minutes,
			cur_time.Seconds,
			cur_date.Year,
			cur_date.Month,
			cur_date.Date,
			0
	};

	SendMessage(0, ServiceCmd_SetSystemTime, date, SEND_NOW, BUS_CAN12);

	// Параллельно раз в секунду шлём время ППКУ по UART
	UartSendPpkyTime();
}

static void UpdateActiveDeviceList(uint32_t msg_id, uint32_t now_ms) {
	can_ext_id_t id;
	id.ID = msg_id;
	// интересуют только устройства МКУ (13, 14) и посылки dir=1
	if (id.field.dir == 0)
		return;
	if (id.field.d_type != DEVICE_MCU_IGN_TYPE &&
	    id.field.d_type != DEVICE_MCU_TC_TYPE &&
	    id.field.d_type != DEVICE_MCU_K1 &&
	    id.field.d_type != DEVICE_MCU_K2 &&
	    id.field.d_type != DEVICE_MCU_K3 &&
	    id.field.d_type != DEVICE_MCU_KR)
		return;

	Device dev;
	dev.zone  = (uint8_t)(id.field.zone & 0x7Fu);
	dev.h_adr = (uint8_t)id.field.h_adr;
	dev.l_adr = (uint8_t)(id.field.l_adr & 0x3Fu);
	dev.d_type = (uint8_t)id.field.d_type;

	// поиск уже известного
	for (uint8_t i = 0; i < g_active_devices_count; i++) {
		if (g_active_devices[i].dev.zone  == dev.zone &&
		    g_active_devices[i].dev.h_adr == dev.h_adr &&
		    g_active_devices[i].dev.l_adr == dev.l_adr &&
		    g_active_devices[i].dev.d_type == dev.d_type) {
			g_active_devices[i].last_seen_ms = now_ms;
			g_active_devices[i].online = 1;
			return;
		}
	}

	if (g_active_devices_count < 32) {
		g_active_devices[g_active_devices_count].dev = dev;
		g_active_devices[g_active_devices_count].last_seen_ms = now_ms;
		g_active_devices[g_active_devices_count].online = 1;
		g_active_devices[g_active_devices_count].can_status_mask = 0u;
		g_active_devices[g_active_devices_count].can_state_mask = 0u;
		g_active_devices[g_active_devices_count].can_status_valid = 0u;
		g_active_devices[g_active_devices_count].u24_01v = 0u;
		memset(g_active_devices[g_active_devices_count].mcu_status_data, 0, sizeof(g_active_devices[g_active_devices_count].mcu_status_data));
		g_active_devices[g_active_devices_count].vdev_count = 0u;
		/* vdevs[] уже обнулены при memset в AddrAuto_ClearActiveDevices() */
		g_active_devices_count++;
	}
}

static void RefreshActiveDevices(uint32_t now_ms) {
	for (uint8_t i = 0; i < g_active_devices_count; i++) {
		if (g_active_devices[i].online &&
		    (now_ms - g_active_devices[i].last_seen_ms) > 5000u) {
			g_active_devices[i].online = 0;
			g_active_devices[i].can_status_mask = 0u;
			g_active_devices[i].can_state_mask = 0u;
			g_active_devices[i].can_status_valid = 0u;
			g_active_devices[i].u24_01v = 0u;
			memset(g_active_devices[i].mcu_status_data, 0, sizeof(g_active_devices[i].mcu_status_data));
			g_active_devices[i].vdev_count = 0u;
			memset(g_active_devices[i].vdevs, 0, sizeof(g_active_devices[i].vdevs));
		}
	}
}

static int FindActiveMcuExactIndex(uint8_t zone, uint8_t h_adr, uint8_t l_adr, uint8_t d_type) {
	for (uint8_t i = 0; i < g_active_devices_count; i++) {
		if (!g_active_devices[i].online)
			continue;
		if (g_active_devices[i].dev.zone == zone &&
		    g_active_devices[i].dev.h_adr == h_adr &&
		    g_active_devices[i].dev.l_adr == l_adr &&
		    g_active_devices[i].dev.d_type == d_type) {
			return (int)i;
		}
	}
	return -1;
}

static int FindActiveMcuByZoneHAdrIndex(uint8_t zone, uint8_t h_adr) {
	for (uint8_t i = 0; i < g_active_devices_count; i++) {
		if (!g_active_devices[i].online)
			continue;
		if (g_active_devices[i].dev.zone == zone &&
		    g_active_devices[i].dev.h_adr == h_adr) {
			return (int)i;
		}
	}
	return -1;
}

static uint8_t RelayAuto_IsFireTriggerInZone(uint8_t zone)
{
	uint8_t z_can = (uint8_t)(zone & 0x7Fu);
	if (z_can >= 1u && z_can <= ZONE_NUMBER) {
		uint8_t zi = (uint8_t)(z_can - 1u);
		if (g_relay_fire_zone_active[zi] != 0u) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t RelayAuto_IsFireTriggerAnyZone(void)
{
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (g_relay_fire_zone_active[zi] != 0u) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t RelayAuto_IsStartTriggerInZone(uint8_t zone)
{
	uint8_t z_can = (uint8_t)(zone & 0x7Fu);
	if (z_can >= 1u && z_can <= ZONE_NUMBER) {
		uint8_t zi = (uint8_t)(z_can - 1u);
		if (g_relay_start_zone_active[zi] != 0u) {
			return 1u;
		}
	}
	return 0u;
}

static uint8_t RelayAuto_IsStartTriggerAnyZone(void)
{
	for (uint8_t zi = 0u; zi < ZONE_NUMBER; zi++) {
		if (g_relay_start_zone_active[zi] != 0u) {
			return 1u;
		}
	}
	return 0u;
}

extern "C" void RelayAuto_NotifyStartExtinguish(uint8_t zone_can)
{
	uint8_t z = (uint8_t)(zone_can & 0x7Fu);
	if (z == 0u) {
		memset(g_relay_start_zone_active, 1, sizeof(g_relay_start_zone_active));
		return;
	}
	if (z >= 1u && z <= ZONE_NUMBER) {
		g_relay_start_zone_active[z - 1u] = 1u;
	}
}

static void RelayAuto_OnFireServiceCmd(uint32_t MsgID, uint8_t command)
{
	can_ext_id_t id;
	id.ID = MsgID;
	uint8_t z_can = (uint8_t)(id.field.zone & 0x7Fu);
	if (command == ServiceCmd_Fire_SetStatusFire) {
		if (z_can >= 1u && z_can <= ZONE_NUMBER) {
			uint8_t zi = (uint8_t)(z_can - 1u);
			g_relay_fire_zone_active[zi] = 1u;
		}
	} else if (command == ServiceCmd_Fire_StopExtinguishment) {
		if (z_can == 0u) {
			memset(g_relay_fire_zone_active, 0, sizeof(g_relay_fire_zone_active));
		} else if (z_can >= 1u && z_can <= ZONE_NUMBER) {
			uint8_t zi = (uint8_t)(z_can - 1u);
			g_relay_fire_zone_active[zi] = 0u;
		}
	}
}

static uint8_t RelayAuto_IsFaultTriggerInZone(uint8_t zone)
{
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || ((m->dev.zone & 0x7Fu) != (zone & 0x7Fu))) {
			continue;
		}
		if (m->can_status_valid != 0u && (m->can_state_mask & 0x0Fu) != 0u) {
			return 1u;
		}
		for (uint8_t j = 0u; j < m->vdev_count; j++) {
			const ActiveDeviceInfo::s_active_vdev *v = &m->vdevs[j];
			if (!v->online) {
				continue;
			}
			if (v->v_d_type == DEVICE_IGNITER_TYPE) {
				if (v->status_cmd == DeviceIgniterStatus_Error ||
				    v->line_state == DeviceIgniterLineState_Break ||
				    v->line_state == DeviceIgniterLineState_Short) {
					return 1u;
				}
			} else if (v->v_d_type == DEVICE_DPT_TYPE ||
				   v->v_d_type == DEVICE_BUTTON_TYPE ||
				   v->v_d_type == DEVICE_LSWITCH_TYPE) {
				if (v->status_cmd == DeviceDPTStatus_Error ||
				    v->line_state == DeviceDPTLineState_Break ||
				    v->line_state == DeviceDPTLineState_Short ||
				    v->line_state == DeviceDPTLineState_Fault) {
					return 1u;
				}
			}
		}
	}
	return 0u;
}

static uint8_t RelayAuto_IsLswitchOpenTriggerInZone(uint8_t zone)
{
	for (uint8_t i = 0u; i < g_active_devices_count; i++) {
		const ActiveDeviceInfo *m = &g_active_devices[i];
		if (!m->online || ((m->dev.zone & 0x7Fu) != (zone & 0x7Fu))) {
			continue;
		}
		for (uint8_t j = 0u; j < m->vdev_count; j++) {
			const ActiveDeviceInfo::s_active_vdev *v = &m->vdevs[j];
			if (!v->online || v->v_d_type != DEVICE_LSWITCH_TYPE) {
				continue;
			}
			if (v->line_state == DeviceDPTLineState_Press ||
			    v->status_cmd == DeviceDPTStatus_Warning) {
				return 1u;
			}
		}
	}
	return 0u;
}

static int8_t RelayAuto_FindTrack(uint8_t zone, uint8_t h_adr, uint8_t l_adr)
{
	for (uint8_t i = 0u; i < RELAY_AUTO_MAX_TRACK; i++) {
		if (g_relay_auto_track[i].valid == 0u) {
			continue;
		}
		if (g_relay_auto_track[i].zone == zone &&
		    g_relay_auto_track[i].h_adr == h_adr &&
		    g_relay_auto_track[i].l_adr == l_adr) {
			return (int8_t)i;
		}
	}
	return -1;
}

static int8_t RelayAuto_AllocTrack(void)
{
	for (uint8_t i = 0u; i < RELAY_AUTO_MAX_TRACK; i++) {
		if (g_relay_auto_track[i].valid == 0u) {
			return (int8_t)i;
		}
	}
	return -1;
}

static void RelayAuto_SendTarget(uint8_t zone, uint8_t h_adr, uint8_t l_adr, uint8_t target_state)
{
	can_ext_id_t can_id;
	uint8_t data[8] = {0u};
	can_id.ID = 0u;
	can_id.field.dir = 0u;
	can_id.field.d_type = DEVICE_RELAY_TYPE;
	can_id.field.h_adr = h_adr;
	can_id.field.l_adr = (uint8_t)(l_adr & 0x3Fu);
	can_id.field.zone = (uint8_t)(zone & 0x7Fu);
	data[0] = 10u;
	data[1] = (target_state != 0u) ? 1u : 0u;
	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
}

static void RelayAuto_Process(void)
{
	for (uint8_t mi = 0u; mi < 32u; mi++) {
		const MKUCfg *m = &PPKYConfig.CfgDevices[mi];
		const Device *mdev = &m->UId.devId;
		if (mdev->d_type == 0u) {
			continue;
		}
		const uint8_t zone = (uint8_t)(mdev->zone & 0x7Fu);
		const uint8_t h_adr = (uint8_t)mdev->h_adr;
		for (uint8_t slot = 0u; slot < NUM_DEV_IN_MCU; slot++) {
			if ((uint8_t)m->VDtype[slot] != DEVICE_RELAY_TYPE) {
				continue;
			}
			const DeviceRelayConfig *cfg = (const DeviceRelayConfig*)m->Devices[slot].reserv;
			const uint8_t mode = cfg->mode;
			if (mode == 0u || mode > 6u) {
				continue;
			}

			uint8_t trigger = 0u;
			if (mode == 1u) {
				trigger = RelayAuto_IsFireTriggerInZone(zone);
			} else if (mode == 2u) {
				trigger = RelayAuto_IsFaultTriggerInZone(zone);
			} else if (mode == 3u) {
				trigger = RelayAuto_IsLswitchOpenTriggerInZone(zone);
			} else if (mode == 4u) {
				trigger = RelayAuto_IsFireTriggerAnyZone();
			} else if (mode == 5u) {
				trigger = RelayAuto_IsStartTriggerInZone(zone);
			} else {
				trigger = RelayAuto_IsStartTriggerAnyZone();
			}

			uint8_t target_state = (cfg->initial_state != 0u) ? 1u : 0u;
			if (trigger != 0u) {
				target_state = (target_state == 0u) ? 1u : 0u;
			}

			const uint8_t l_adr = (uint8_t)((slot + 1u) & 0x3Fu);
			int8_t track_idx = RelayAuto_FindTrack(zone, h_adr, l_adr);
			if (track_idx < 0) {
				track_idx = RelayAuto_AllocTrack();
				if (track_idx >= 0) {
					g_relay_auto_track[(uint8_t)track_idx].valid = 1u;
					g_relay_auto_track[(uint8_t)track_idx].zone = zone;
					g_relay_auto_track[(uint8_t)track_idx].h_adr = h_adr;
					g_relay_auto_track[(uint8_t)track_idx].l_adr = l_adr;
					g_relay_auto_track[(uint8_t)track_idx].target_state = (uint8_t)(target_state ^ 1u);
				}
			}

			if (track_idx >= 0) {
				RelayAutoTrack *tr = &g_relay_auto_track[(uint8_t)track_idx];
				if (tr->target_state != target_state) {
					RelayAuto_SendTarget(zone, h_adr, l_adr, target_state);
					tr->target_state = target_state;
				}
			}
		}
	}
}

static void UpdateMcuCanStatus(uint32_t MsgID, uint8_t *MsgData) {
	can_ext_id_t id;
	id.ID = MsgID;

	/* Только МКУ и только их статус (cmd=0) */
	if (id.field.d_type != DEVICE_MCU_IGN_TYPE &&
	    id.field.d_type != DEVICE_MCU_TC_TYPE &&
	    id.field.d_type != DEVICE_MCU_K1 &&
	    id.field.d_type != DEVICE_MCU_K2 &&
	    id.field.d_type != DEVICE_MCU_K3 &&
	    id.field.d_type != DEVICE_MCU_KR)
		return;
	if (MsgData[0] != 0u)
		return;

	uint8_t zone  = (uint8_t)(id.field.zone & 0x7Fu);
	uint8_t h_adr = (uint8_t)id.field.h_adr;
	uint8_t l_adr = (uint8_t)(id.field.l_adr & 0x3Fu);
	uint8_t d_type = (uint8_t)id.field.d_type;

	int idx = FindActiveMcuExactIndex(zone, h_adr, l_adr, d_type);
	if (idx < 0)
		return;

	/* В MsgData:
	 * MsgData[0] = cmd (0)
	 * MsgData[1..7] = Data[0..6] из SendMessage()
	 * В Data[4] находится CAN mask (CAN1_Active | CAN2_Active<<1)
	 * => MsgData[5]
	 * В Data[5] находится U24 (1V)
	 * => MsgData[6]
	 * В Data[6] находится CAN state mask (2 бита на шину)
	 * => MsgData[7] */
	g_active_devices[idx].can_status_mask = MsgData[5];
	g_active_devices[idx].can_state_mask = MsgData[7];
	g_active_devices[idx].can_status_valid = 1u;
	g_active_devices[idx].u24_01v = MsgData[6];
	memcpy(g_active_devices[idx].mcu_status_data, MsgData, sizeof(g_active_devices[idx].mcu_status_data));
}

static void UpdateActiveVirtualDevices(uint32_t MsgID, uint8_t *MsgData, uint32_t now_ms) {
	can_ext_id_t id;
	id.ID = MsgID;

	if (id.field.dir == 0)
		return;

	/* Физические устройства МКУ игнорируем — виртуальные приходят как d_type=DEVICE_*TYPE */
	uint8_t v_d_type = (uint8_t)id.field.d_type;
	if (v_d_type == DEVICE_MCU_IGN_TYPE ||
	    v_d_type == DEVICE_MCU_TC_TYPE ||
	    v_d_type == DEVICE_MCU_K1 ||
	    v_d_type == DEVICE_MCU_K2 ||
	    v_d_type == DEVICE_MCU_K3 ||
	    v_d_type == DEVICE_MCU_KR)
		return;

	/* Принимаем только известные виртуальные типы из device_lib.
	 * Для «любых других типов» нужен реальный декодер под них —
	 * пока храним raw status_params. */
	if (v_d_type != DEVICE_IGNITER_TYPE &&
	    v_d_type != DEVICE_DPT_TYPE &&
	    v_d_type != DEVICE_BUTTON_TYPE &&
	    v_d_type != DEVICE_LSWITCH_TYPE) {
		return;
	}

	uint8_t zone  = (uint8_t)(id.field.zone & 0x7Fu);
	uint8_t h_adr = (uint8_t)id.field.h_adr;
	uint8_t v_l_adr = (uint8_t)(id.field.l_adr & 0x3Fu);

	int mcu_idx = FindActiveMcuByZoneHAdrIndex(zone, h_adr);
	if (mcu_idx < 0)
		return;

	ActiveDeviceInfo *m = &g_active_devices[mcu_idx];
	if (m->vdev_count >= PPKY_MAX_ACTIVE_VDEVS_PER_MCU)
		return;

	/* поиск существующего виртуального устройства */
	uint8_t v_idx = 0xFFu;
	for (uint8_t i = 0; i < m->vdev_count; i++) {
		if (m->vdevs[i].v_d_type == v_d_type && m->vdevs[i].v_l_adr == v_l_adr) {
			v_idx = i;
			break;
		}
	}

	if (v_idx == 0xFFu) {
		v_idx = m->vdev_count;
		m->vdev_count++;
		memset(&m->vdevs[v_idx], 0, sizeof(m->vdevs[v_idx]));
		m->vdevs[v_idx].v_d_type = v_d_type;
		m->vdevs[v_idx].v_l_adr = v_l_adr;
	}

	/* Обновляем raw-статус */
	uint8_t new_status_cmd = MsgData[0];

	uint8_t was_online = m->vdevs[v_idx].online;
	uint8_t old_status_cmd = m->vdevs[v_idx].status_cmd;

	m->vdevs[v_idx].online = 1u;
	m->vdevs[v_idx].last_seen_ms = now_ms;
	m->vdevs[v_idx].prev_status_cmd = old_status_cmd;
	/* Липкий флаг: если уже был 1 — не сбрасываем. Становится 1 только при реальной смене статуса. */
	if (m->vdevs[v_idx].status_changed == 0u) {
		m->vdevs[v_idx].status_changed = (was_online ? (old_status_cmd != new_status_cmd) : 0u);
	}
	m->vdevs[v_idx].status_cmd = new_status_cmd;
	memcpy(m->vdevs[v_idx].status_params, &MsgData[1], 7u);

	/* Декодинг удобных полей для популярных типов */
	if (v_d_type == DEVICE_IGNITER_TYPE) {
		/* status_params[0] = LineState
		 * status_params[1] = ack_flags
		 * status_params[2..3] = измерение линии (LE, 2 байта) */
		m->vdevs[v_idx].line_state = m->vdevs[v_idx].status_params[0];
		m->vdevs[v_idx].ack_flags  = m->vdevs[v_idx].status_params[1];
		m->vdevs[v_idx].igniter_resistance_ohm =
			(uint16_t)m->vdevs[v_idx].status_params[2] |
			((uint16_t)m->vdevs[v_idx].status_params[3] << 8);
	} else if (v_d_type == DEVICE_DPT_TYPE ||
	           v_d_type == DEVICE_BUTTON_TYPE ||
	           v_d_type == DEVICE_LSWITCH_TYPE) {
		/* status_params[0] = LineState
		 * status_params[1] = max_fault_mask (bitmask)
		 * status_params[2..3] = max_temp_tc (int16 LE)
		 * status_params[4..5] = max_internal_temp (int16 LE)
		 * status_params[6] = resistance_x100 (uint8), R=byte*100 Ом */
		m->vdevs[v_idx].line_state = m->vdevs[v_idx].status_params[0];
		m->vdevs[v_idx].resistance_ohm = (uint16_t)m->vdevs[v_idx].status_params[6] * 100u;
		m->vdevs[v_idx].max_fault_mask = m->vdevs[v_idx].status_params[1];
		m->vdevs[v_idx].max_temp_c =
			(int16_t)((uint16_t)m->vdevs[v_idx].status_params[2] |
				 ((uint16_t)m->vdevs[v_idx].status_params[3] << 8));
		m->vdevs[v_idx].max_internal_temp_c =
			(int16_t)((uint16_t)m->vdevs[v_idx].status_params[4] |
				 ((uint16_t)m->vdevs[v_idx].status_params[5] << 8));
	}
}

static void CheckMkuConfigMismatch(void) {
	// Сравнить активные онлайн-устройства с конфигом PPKYConfig.CfgDevices
	uint8_t presence_mismatch = 0u;

	for (uint8_t i = 0; i < g_active_devices_count; i++) {
		if (!g_active_devices[i].online)
			continue;

		uint8_t found = 0;
		for (uint8_t j = 0; j < 32u; j++) {
			const MKUCfg *m = &PPKYConfig.CfgDevices[j];
			const Device *dv = &m->UId.devId;
			if (dv->d_type == 0)
				continue;
			if (dv->zone  == g_active_devices[i].dev.zone &&
			    dv->h_adr == g_active_devices[i].dev.h_adr &&
			    dv->l_adr == g_active_devices[i].dev.l_adr &&
			    dv->d_type == g_active_devices[i].dev.d_type) {
				found = 1;
				break;
			}
		}
		if (!found) {
			presence_mismatch = 1u;
			break;
		}
	}
	g_mku_mismatch_flag = (presence_mismatch || g_cfg_crc_mismatch_flag) ? 1u : 0u;
}


void SetHAdr(uint8_t h_adr) {
	extern Device BoardDevicesList[];
	PPKYConfig.UId.devId.h_adr = h_adr;
	BoardDevicesList[0].h_adr = h_adr;
	SaveConfig();
}


uint8_t PControlGetSTCB(uint8_t ch) {
	if (ch == POWER_CH_PANEL) {
		/* PANEL_DG: низкий уровень — ошибка (КЗ/обрыв), в логике PControl это status!=0. */
		GPIO_PinState dg = HAL_GPIO_ReadPin(PANEL_DG_GPIO_Port, PANEL_DG_Pin);
		return (dg == GPIO_PIN_RESET) ? 1u : 0u;
	}
	if (ch < POWER_NUM_MKU_CH) {
		return (uint8_t)HAL_GPIO_ReadPin(POWER_ST_PORT[ch], POWER_ST_PIN[ch]);
	}
	return 0u;
}

uint32_t PControlGetADCCB(uint8_t ch) {
	// ch = 0 → ток канала 1, ch = 1 → ток канала 2
	// CHANNEL_VAL[1], CHANNEL_VAL[2] — токи в мА (или код АЦП/пересчитанное значение)
	switch (ch) {
	case POWER_CH_MKU_1:
		return CHANNEL_VAL[1];
	case POWER_CH_MKU_2:
		return CHANNEL_VAL[2];
	case POWER_CH_PANEL:
	default:
		return 0u;
	}
}

void PControlSetOutCB(uint8_t ch, uint8_t out) {
	if (ch == POWER_CH_PANEL) {
		HAL_GPIO_WritePin(PANEL_EN_GPIO_Port, PANEL_EN_Pin, (GPIO_PinState)out);
		return;
	}
	if (ch < POWER_NUM_MKU_CH) {
		HAL_GPIO_WritePin(POWER_OUT_PORT[ch], POWER_OUT_PIN[ch], (GPIO_PinState)out);
	}
}

static volatile uint32_t sizesctruct;
void AppInit() {

	// Чтение сохранённой конфигурации из Flash (область конфигурации)
	uint32_t cfg_addr = SPIF_SectorToAddress(FLASH_CFG_START_SECTOR);
	PPKYConfigHeader hdr;

	//SPIF_EraseChip(&hFlash);
/*
	for (uint32_t s = 0; s < FLASH_CFG_SECTORS_USED; s++) {
		SPIF_EraseSector(&hFlash, FLASH_CFG_START_SECTOR + s);
	}
*/
	SPIF_ReadAddress(&hFlash, cfg_addr, (uint8_t *)&hdr, sizeof(hdr));

	bool header_ok = (hdr.magic == PPKY_CFG_HEADER_MAGIC) &&
			         (hdr.size  == sizeof(PPKYConfig));

	//ReadSavedConfig();

	if (header_ok) {


		ReadSavedConfig();
/*
		// Заголовок валиден — читаем полезную часть
		SPIF_ReadAddress(&hFlash,
				         cfg_addr + sizeof(PPKYConfigHeader),
						 (uint8_t *)&SavedPPKYConfig,
						 sizeof(SavedPPKYConfig));
						 */
		PPKYConfig = SavedPPKYConfig;
	} else {
		// Заголовок мусор: считаем, что конфигурации нет
		// Сбрасываем на значения по умолчанию и сохраняем в область конфигурации
		//DefaultConfig();

		FillConfigTemplate();

		SaveConfig();
	}

	//FillConfigTemplate();

	//SaveConfig();

	// Передаём указатели в backend (для сервисных команд работы с конфигурацией)
	SetConfigPtr((uint8_t *)&SavedPPKYConfig, (uint8_t *)&PPKYConfig);
	ConfigSync_Init(&PPKYConfig, g_active_devices, &g_active_devices_count, SaveConfig, App_OnConfigApplySuccess, &g_cfg_crc_mismatch_flag);
	ConfigMonitor_Init(HAL_GetTick());
	ConfigIgnBlockSync_Init();
	RtcCache_Refresh();
	(void)EventLog_Init(&hFlash);
	EventLog_LogMasterBoot();
	LogTransport_Init();
	EspManager_Init();
	/* ESP32 включён при старте; WiFi — только из меню «Связь → WIFI». */
	Esp32_SetEnabled(1u);

	// Список устройств по аналогии с МКУ: 0-й элемент — сама плата ППКУ
	extern Device BoardDevicesList[];
	extern uint8_t nDevs;

	if(PPKYConfig.UId.devId.h_adr == 0) PPKYConfig.UId.devId.h_adr = 1;

	nDevs = 1; /* Dev 0 — ППКУ */
	BoardDevicesList[0].zone  = PPKYConfig.UId.devId.zone & 0x7Fu;
	BoardDevicesList[0].h_adr = PPKYConfig.UId.devId.h_adr;
	BoardDevicesList[0].l_adr = PPKYConfig.UId.devId.l_adr & 0x3Fu;
	BoardDevicesList[0].d_type = DEVICE_PPKY_TYPE;

	Button_Init();
	Beeper_Init();

	for (uint8_t i = 0; i < POWER_NUM_CHANNELS; i++) {
		Power[i] = new PControl(i);
		Power[i]->PControlInit(PControlGetSTCB, PControlGetADCCB, PControlSetOutCB);
		Power[i]->SetEnable(1);
	}
/*
	HAL_Delay(1000);
	Power[0]->PControlSetOut(0, true);
	HAL_Delay(1000);
	Power[1]->PControlSetOut(1, true);
	*/
	isAppInit = true;
	extern bool isListener;
	isListener = true;
	extern uint8_t isMaster;
	isMaster = 1;

	/* Инициализация FSM пожара */
	Fire_Init();
	RsPanelMaster_Init(&g_rs_panel_master, &huart1, BRP_485_EN_GPIO_Port, BRP_485_EN_Pin);



	PPKYConfig.fire_and[0] = 1;
	PPKYConfig.fire_and[1] = 1;
	sizesctruct = sizeof(PPKYConfig);

}

extern "C" void PControl_OnStatusFault(uint8_t ch, uint32_t now_ms) {
	if (ch < POWER_NUM_CHANNELS && Power[ch] != nullptr) {
		Power[ch]->OnStatusFault(now_ms);
	}
}

void AppProcess(uint32_t now_ms) {
	if (isAppInit == false)
		return;
	for (uint8_t i = 0; i < POWER_NUM_CHANNELS; i++) {
		if (Power[i] == nullptr)
			continue;

		Power[i]->Process(now_ms);

	}
	// Неблокирующая машина состояний автозадания адресов по команде 10
	AddrAuto_Process(now_ms);

	/* Отложенный hard reset после APPLY/Sync-операций config_sync. */
	MkuHardReset_Process(now_ms);
}

uint32_t counter1s = 0;

uint32_t warning_process_delay = 10000;

uint32_t led_power_toogle_cnt = 0;
uint8_t led_power_is_toogle = 0;

/* Гистерезис порогов входа питания: ширина зоны возврата (~2% номинала, не меньше 300 мВ).
 * Без него на границе enter-порога ADC даёт частые set/clear → спам EventLog. */
static constexpr uint32_t PPKU_POWER_HYST_PCT = 2u;
static constexpr uint32_t PPKU_POWER_HYST_MIN_MV = 300u;
static uint8_t s_ppku_input_fault_latched = 0u;

static uint8_t App_PpkuInputFaultHyst(uint32_t mv_mV, uint8_t prev_fault,
				      uint32_t low_enter_mv, uint32_t high_enter_mv,
				      uint32_t low_exit_mv, uint32_t high_exit_mv)
{
	if (prev_fault != 0u) {
		/* Сброс только когда напряжение уверенно внутри рабочей полосы. */
		return (mv_mV >= low_exit_mv && mv_mV <= high_exit_mv) ? 0u : 1u;
	}
	/* Установка при выходе за enter-пороги. */
	return (mv_mV < low_enter_mv || mv_mV > high_enter_mv) ? 1u : 0u;
}

static void App_UpdatePowerFaultIndication(uint32_t now_ms)
{
	uint8_t power_fault_mask = 0u;       /* Ошибки выходов power-модуля (внешнее питание МКУ). */
	uint8_t ppku_input_fault_mask = 0u;  /* Ошибки входов питания ППКУ. */
	(void)now_ms;

	/* Для "пропадания питания" используем порог присутствия 15%/10% от номинала + гистерезис. */
	uint32_t nominal_mv = ((PPKYConfig.power_value != 0u) ? (uint32_t)PPKYConfig.power_value : 24u) * 1000u;
	uint32_t lov_present_threshold_mv = (nominal_mv * 15u) / 100u;
	uint32_t high_present_threshold_mv = nominal_mv / 10u;
	uint32_t hyst_mv = (nominal_mv * PPKU_POWER_HYST_PCT) / 100u;
	if (hyst_mv < PPKU_POWER_HYST_MIN_MV) {
		hyst_mv = PPKU_POWER_HYST_MIN_MV;
	}
	uint32_t low_enter_mv = (nominal_mv > lov_present_threshold_mv) ?
				(nominal_mv - lov_present_threshold_mv) : 0u;
	uint32_t high_enter_mv = nominal_mv + high_present_threshold_mv;
	uint32_t low_exit_mv = low_enter_mv + hyst_mv;
	uint32_t high_exit_mv = (high_enter_mv > hyst_mv) ? (high_enter_mv - hyst_mv) : high_enter_mv;
	if (low_exit_mv > high_exit_mv) {
		low_exit_mv = high_exit_mv;
	}

	uint32_t main_mv = (CHANNEL_VAL[4] > 0) ? (uint32_t)CHANNEL_VAL[4] : 0u; /* Основной ввод */
	uint32_t reserve_mv = (CHANNEL_VAL[0] > 0) ? (uint32_t)CHANNEL_VAL[0] : 0u; /* Резервный ввод */
	uint8_t reserve_required = (PPKYConfig.power_input == 2u) ? 1u : 0u; /* 2 = используем оба ввода */

	if (App_PpkuInputFaultHyst(main_mv, (uint8_t)(s_ppku_input_fault_latched & 0x01u),
				   low_enter_mv, high_enter_mv, low_exit_mv, high_exit_mv) != 0u) {
		ppku_input_fault_mask |= 0x01u; /* ПИТАНИЕ 1 */
	}
	if (reserve_required) {
		if (App_PpkuInputFaultHyst(reserve_mv, (uint8_t)((s_ppku_input_fault_latched >> 1) & 0x01u),
					   low_enter_mv, high_enter_mv, low_exit_mv, high_exit_mv) != 0u) {
			ppku_input_fault_mask |= 0x02u; /* ПИТАНИЕ 2 */
		}
	} else {
		/* Резерв не используется — не удерживаем старую ошибку канала 2. */
		ppku_input_fault_mask &= (uint8_t)~0x02u;
	}
	s_ppku_input_fault_latched = ppku_input_fault_mask;

	for (uint8_t i = 0u; i < POWER_NUM_CHANNELS; i++) {
		if (Power[i] != nullptr && Power[i]->IsError()) {
			power_fault_mask |= (uint8_t)(1u << i);
		}
	}
	Warning_SetPowerFaultMask(power_fault_mask);
	Warning_SetPpkuInputFaultMask(ppku_input_fault_mask);

	if (ppku_input_fault_mask != 0u) {
		led_power_is_toogle = 1;
	} else {
		Led_Set(LED_POWER, 1);
		led_power_toogle_cnt = LED_POWER_TOOGLE_PERIOD_MS;
		led_power_is_toogle = 0;
	}

	if (led_power_is_toogle) {
		if (led_power_toogle_cnt) {
			if (led_power_toogle_cnt == (LED_POWER_TOOGLE_PERIOD_MS / 2)) {
				Led_Set(LED_POWER, 1);
			}
			led_power_toogle_cnt--;
		} else {
			Led_Set(LED_POWER, 0);
			led_power_toogle_cnt = LED_POWER_TOOGLE_PERIOD_MS;
		}
	}

	/* LED_ERR — только неисправность (WarningProcess1ms). ВНИМАНИЕ — на LED_FIRE. */
	uint8_t has_fault = (power_fault_mask != 0u || ppku_input_fault_mask != 0u) ? 1u :
			    (Warning_HasActiveFault() || Warning_HasActiveAttention()) ? 1u : 0u;
	if (!has_fault && !Fire_IsActive()) {
		Led_Set(LED_NORM, 1u);
	} else {
		Led_Set(LED_NORM, 0u);
	}
}

void AppTimer1ms() {
	uint32_t now = HAL_GetTick();
	ConfigSync_Process1ms(now);
	AppProcess(now);
	Fire_Timer1ms();
	BackendProcess();

	/* Grace перед warning/relay/log: счётчик в мс остаётся здесь. */
	if (warning_process_delay) {
		warning_process_delay--;
	}

	if (!MenuUi_IsConfigSessionActive()) {
		CheckMkuConfigMismatch();
	}

	counter1s++;

	if (counter1s >= 1000) {
		counter1s = 0;
		RtcCache_Tick1s();
		AppSetStatus();
		status_sec_cnt++;
	}
}

void AppTimer10ms() {
	uint32_t now = HAL_GetTick();

	/* Чтение кнопок делаем реже, чтобы не перегружать I2C.
	 * Теперь Button_Process вызывается раз в ~с (при шаге AppTimer10ms ~10 мс). */
	static uint8_t button_acc = 0;
	button_acc++;
	if (button_acc >= 1u) {
		button_acc = 0;
		Button_Process();
	}

	ConfigIgnBlockSync_Process1ms(now);
	if (!MenuUi_IsConfigSessionActive()) {
		ConfigMonitor_Process1ms(now);
		Position_EvaluateMismatch(now);
		g_position_fault_mask = Position_GetFaultMask();
	}
	RefreshActiveDevices(now);
	if (!MenuUi_IsConfigSessionActive()) {
		Warning_SetMkuPositionFaultMask(g_position_fault_mask);
	}
	App_UpdatePowerFaultIndication(now);
	EventLog_ProcessTelemetrySample(now);
	EspManager_Process(now);
	MenuConfig_Process1ms(now);

	if (warning_process_delay == 0) {
		WarningProcess1ms();
		RelayAuto_Process();
		if (PPKYConfig.rs485_on != 0u) {
			LogTransport_Process();
		}
	}

	Fire_Timer10ms();
	Beeper_Process();
	Led_Process();
	RsPanelMaster_Process10ms(&g_rs_panel_master, HAL_GetTick());
}



void SetApp(uint32_t dst_adr, uint32_t src_adr, uint32_t sz) {

}

/* Установка системных времени и даты ППКУ по команде ServiceCmd_SetSystemTime.
 * Формат MsgData:
 *  [0] BCD HH
 *  [1] BCD MM
 *  [2] BCD SS
 *  [3] BCD YY (0..99)
 *  [4] BCD MM (1..12)
 *  [5] BCD DD (1..31)
 */
void RcvSetSystemTime(uint8_t *MsgData) {
	RTC_TimeTypeDef t = {0};
	t.Hours   = MsgData[0];
	t.Minutes = MsgData[1];
	t.Seconds = MsgData[2];
	t.SubSeconds = 0;
	RTC_DateTypeDef d;
	if (HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BCD) != HAL_OK) {
		return;
	}
	// Обновляем дату из команды, формат RTC: BCD YY/MM/DD
	d.Year  = MsgData[3];
	d.Month = MsgData[4];
	d.Date  = MsgData[5];
	if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BCD) != HAL_OK) {
		return;
	}
	HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BCD);
	RtcCache_Refresh();
}
/*
uint32_t GetID() {
	uint32_t idPart1 = STM32_UUID[0];
	uint32_t idPart2 = STM32_UUID[1];
	uint32_t idPart3 = STM32_UUID[2];
	return (idPart1 ^ idPart2 ^ idPart3);
}
*/
void ResetMCU() {
	NVIC_SystemReset();
}




// посылки от устройств
void ListenerCommandCB(uint32_t MsgID, uint8_t *MsgData) {
	uint32_t now = HAL_GetTick();
	UpdateActiveDeviceList(MsgID, now);
	ConfigSync_OnListenerMessage(MsgID, MsgData);

	/* Обновляем CAN-состояние МКУ и статусы его виртуальных устройств */
	UpdateMcuCanStatus(MsgID, MsgData);
	UpdateActiveVirtualDevices(MsgID, MsgData, now);

	uint8_t Command = MsgData[0];
	if(Command >= ServiceCmd_Fire_SetStatusFire && Command <= ServiceCmd_Fire_SetReplyResumeExtinguishmentTimer) {
		RelayAuto_OnFireServiceCmd(MsgID, Command);
		if(Command == ServiceCmd_Fire_SetStatusFire) {
			Fire_OnStatusFire(MsgID, MsgData);
		} else if (Command == ServiceCmd_Fire_ReplyStatusFire) {
			Fire_OnReplyStatusFire(MsgID);
		} else if (Command == ServiceCmd_Fire_StopExtinguishment) {
			Fire_OnStopExtinguishment(MsgID);
		} else if (Command == ServiceCmd_Fire_SetReplyStartExtinguishment) {
			Fire_OnReplyStartExtinguishment(MsgID);
		} else if (Command == ServiceCmd_Fire_SetReplyStopExtinguishment) {
			Fire_OnReplyStopExtinguishment(MsgID);
		} else if (Command == ServiceCmd_Fire_PauseExtinguishmentTimer) {
			Fire_OnPauseExtinguishmentTimer(MsgID);
		} else if (Command == ServiceCmd_Fire_ResumeExtinguishmentTimer) {
			Fire_OnResumeExtinguishmentTimer(MsgID);
		} else if (Command == ServiceCmd_Fire_SetReplyPauseExtinguishmentTimer) {
			Fire_OnReplyPauseExtinguishmentTimer(MsgID);
		} else if (Command == ServiceCmd_Fire_SetReplyResumeExtinguishmentTimer) {
			Fire_OnReplyResumeExtinguishmentTimer(MsgID);
		}
	}
}

extern "C" void Fire_UiUpdate(uint8_t active, uint8_t mode, uint8_t remaining_s, uint8_t n_zones,
			      char (*zone_names)[ZONE_NAME_SIZE + 1]) {
	App_OnFireUiUpdate(active, mode, remaining_s, n_zones, zone_names);
}

extern "C" void Warning_UiUpdate(uint8_t active, uint8_t n_items,
				 char (*big_titles)[WARNING_TITLE_LEN],
				 char (*details)[ZONE_NAME_SIZE + 1]) {
	App_OnWarningUiUpdate(active, n_items, big_titles, details);
}










