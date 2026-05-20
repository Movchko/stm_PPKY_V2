#include "power_control.h"
#include "app.hpp"
#include "button.h"
#include "beeper.h"
#include "device_config.h"
#include "led.h"
#include "backend.h"
#include "service.h"
#include "config_sync.hpp"
#include "gui/common/FrontendHeap.hpp"
#include "fire.h"
#include "warning.h"



struct PPKYCfg PPKYConfig;       // локальная (рабочая) конфигурация
struct PPKYCfg SavedPPKYConfig; // копия сохранённой конфигурации из Flash

extern SPIF_HandleTypeDef hFlash;

PControl *Power[2];



ActiveDeviceInfo g_active_devices[NUM_ACTIVE_DEVICE];
uint8_t g_active_devices_count = 0;
uint8_t g_mku_mismatch_flag = 0;
static uint8_t g_cfg_crc_mismatch_flag = 0u;

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

extern UART_HandleTypeDef huart2;

RTC_TimeTypeDef cur_time = {0};
RTC_DateTypeDef cur_date;

static void AddrAuto_ClearActiveDevices(void) {
	memset(g_active_devices, 0, sizeof(g_active_devices));
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

			// Перезапустить питание на обоих каналах (короткое выключение/включение)
			for (uint8_t i = 0; i < 2; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, false);
				}
			}
			HAL_Delay(500);
			for (uint8_t i = 0; i < 2; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, true);
				}
			}

			g_addr_auto_state = ADDR_AUTO_IDLE;
		}
		break;
	}
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
			for (uint8_t i = 0; i < 2; i++) {
				if (Power[i] != nullptr) {
					Power[i]->PControlSetOut(i, false);
				}
			}
			HAL_Delay(1000);
			for (uint8_t i = 0; i < 2; i++) {
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
		 * status_params[1..2] = resistance (LE)
		 * status_params[3] = max_temp_tc (int8)
		 * status_params[4] = max_fault_mask (bitmask)
		 * status_params[5] = max_internal_temp (int8) */
		m->vdevs[v_idx].line_state = m->vdevs[v_idx].status_params[0];
		m->vdevs[v_idx].resistance_ohm =
			(uint16_t)m->vdevs[v_idx].status_params[1] |
			((uint16_t)m->vdevs[v_idx].status_params[2] << 8);
		m->vdevs[v_idx].max_temp_c = (int16_t)(int8_t)m->vdevs[v_idx].status_params[3];
		m->vdevs[v_idx].max_fault_mask = m->vdevs[v_idx].status_params[4];
		m->vdevs[v_idx].max_internal_temp_c = (int16_t)(int8_t)m->vdevs[v_idx].status_params[5];
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
	uint8_t st = 0;
	st = HAL_GPIO_ReadPin(POWER_ST_PORT[ch], POWER_ST_PIN[ch]);
	return st;
}

uint32_t PControlGetADCCB(uint8_t ch) {
	// ch = 0 → ток канала 1, ch = 1 → ток канала 2
	// CHANNEL_VAL[1], CHANNEL_VAL[2] — токи в мА (или код АЦП/пересчитанное значение)
	switch (ch) {
	case 0:
		return CHANNEL_VAL[1];
	case 1:
		return CHANNEL_VAL[2];
	default:
		return 0u;
	}
}

void PControlSetOutCB(uint8_t ch, uint8_t out) {
	HAL_GPIO_WritePin(POWER_OUT_PORT[ch], POWER_OUT_PIN[ch], (GPIO_PinState)out);
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
	ConfigSync_Init(&PPKYConfig, g_active_devices, &g_active_devices_count, SaveConfig, &g_cfg_crc_mismatch_flag);

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

	// Сообщаем модели, какую функцию вызывать при смене состояния звука
	FrontendHeap::getInstance().model.setSoundToggledCallback(Beeper_SoundOnOff);

	for(uint8_t i = 0; i < 2; i++) {
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



	PPKYConfig.fire_and[0] = 1;
	PPKYConfig.fire_and[1] = 1;
	sizesctruct = sizeof(PPKYConfig);

}

extern "C" void PControl_OnStatusFault(uint8_t ch, uint32_t now_ms) {
	if (ch < 2 && Power[ch] != nullptr) {
		Power[ch]->OnStatusFault(now_ms);
	}
}

void AppProcess(uint32_t now_ms) {
	if (isAppInit == false)
		return;
	for (uint8_t i = 0; i < 2; i++) {
		if (Power[i] == nullptr)
			continue;

		Power[i]->Process(now_ms);

	}
	// Неблокирующая машина состояний автозадания адресов по команде 10
	AddrAuto_Process(now_ms);
}

uint32_t counter1s = 0;

uint32_t warning_process_delay = 5000;

static void App_UpdatePowerFaultIndication(uint32_t now_ms)
{
	static uint8_t prev_power_fault_mask = 0u;
	uint8_t power_fault_mask = 0u;       /* Ошибки выходов power-модуля (внешнее питание МКУ). */
	uint8_t ppku_input_fault_mask = 0u;  /* Ошибки входов питания ППКУ. */

	/* Для "пропадания питания" используем порог присутствия 20% от номинала. */
	uint32_t nominal_mv = ((PPKYConfig.power_value != 0u) ? (uint32_t)PPKYConfig.power_value : 24u) * 1000u;
	uint32_t present_threshold_mv = nominal_mv / 5;
	uint32_t main_mv = (CHANNEL_VAL[4] > 0) ? (uint32_t)CHANNEL_VAL[4] : 0u; /* Основной ввод */
	uint32_t reserve_mv = (CHANNEL_VAL[0] > 0) ? (uint32_t)CHANNEL_VAL[0] : 0u; /* Резервный ввод */
	uint8_t reserve_required = (PPKYConfig.power_input == 0u) ? 1u : 0u; /* 0 = используем оба ввода */

	if (main_mv < present_threshold_mv) {
		ppku_input_fault_mask |= 0x01u; /* ПИТАНИЕ 1 */
	}
	if (reserve_required && reserve_mv < present_threshold_mv) {
		ppku_input_fault_mask |= 0x02u; /* ПИТАНИЕ 2 */
	}

	for (uint8_t i = 0u; i < 2u; i++) {
		if (Power[i] != nullptr && Power[i]->IsError()) {
			power_fault_mask |= (uint8_t)(1u << i);
		}
	}
	if (power_fault_mask > prev_power_fault_mask) {
		Led_ForceStatusBright(LED_ERR);
	}
	prev_power_fault_mask = power_fault_mask;
	Warning_SetPowerFaultMask(power_fault_mask);
	Warning_SetPpkuInputFaultMask(ppku_input_fault_mask);

	/* При отсутствии основного ввода индикатор питания должен гаснуть. */
	Led_Set(LED_POWER, ((ppku_input_fault_mask & 0x01u) != 0u) ? 0u : 1u);

	uint8_t has_fault = (power_fault_mask != 0u || ppku_input_fault_mask != 0u) ? 1u : Warning_HasActiveFault();
	Led_Set(LED_ERR, has_fault ? 1u : 0u);
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
	RefreshActiveDevices(now);
	CheckMkuConfigMismatch();
	App_UpdatePowerFaultIndication(now);
	Fire_Timer1ms();

	BackendProcess();
	if(warning_process_delay)
		warning_process_delay--;
	else
		WarningProcess1ms();

	counter1s++;

	if(counter1s >= 1000) {
		counter1s = 0;
		AppSetStatus();
		status_sec_cnt++;
	}
}

void AppTimer10ms() {
	/* Чтение кнопок делаем реже, чтобы не перегружать I2C.
	 * Теперь Button_Process вызывается раз в ~с (при шаге AppTimer10ms ~10 мс). */
	static uint8_t button_acc = 0;
	button_acc++;
	if (button_acc >= 1u) {
		button_acc = 0;
		Button_Process();
	}
	Fire_Timer10ms();
	Beeper_Process();
	Led_Process();
	//for(uint8_t i = 0; i < 2; i++) {
	//st[i] = Power[i]->PControlGetST(i);
	//}

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
		if(Command == ServiceCmd_Fire_SetStatusFire) {
			Fire_OnStatusFire(MsgID);
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
	FrontendHeap::getInstance().model.setFireStatusFromApp(
		active != 0, mode, 0xFFu, remaining_s, n_zones, zone_names);
}

extern "C" void Warning_UiUpdate(uint8_t active, uint8_t n_items,
				 char (*big_titles)[WARNING_TITLE_LEN],
				 char (*details)[ZONE_NAME_SIZE + 1]) {
	FrontendHeap::getInstance().model.setWarningStatusFromApp(
		active != 0, n_items, big_titles, details);
}










