#include "app.hpp"
#include "device_config.h"
#include "device.hpp"
#include "device_cfg_common.h"
#include "backend.h"

extern struct PPKYCfg PPKYConfig;       // локальная (рабочая) конфигурация
extern struct PPKYCfg SavedPPKYConfig; // копия сохранённой конфигурации из Flash
extern SPIF_HandleTypeDef hFlash;

void FlashWriteData(uint8_t *ConfigPtr, uint32_t ConfigSize);

void ReadSavedConfig() {
	/* Важно: SPIF_ReadAddress по большому объёму может работать нестабильно,
	 * поэтому читаем кусками фиксированного размера. */
	const uint32_t CHUNK_BYTES = 256u;
	const uint32_t size = GetConfigSize();
	uint32_t remaining = size;
	uint32_t offset = 0u;

	/* защита от выхода за размер буфера */
	if (remaining > sizeof(SavedPPKYConfig)) {
		remaining = sizeof(SavedPPKYConfig);
	}

	while (remaining > 0u) {
		uint32_t chunk = CHUNK_BYTES;
		if (chunk > remaining) {
			chunk = remaining;
		}

		/* Читаем chunk байт по адресу (внутри SPIF есть блокировка) */
		if (SPIF_ReadAddress(&hFlash,
				             SPIF_SectorToAddress(FLASH_CFG_START_SECTOR) + sizeof(PPKYConfigHeader) + offset,
				             ((uint8_t *)&SavedPPKYConfig) + offset,
				             chunk) == false) {
			continue;
		}

		offset += chunk;
		remaining -= chunk;
	}
}

// Сохранение локальной конфигурации в Flash и обновление копии SavedPPKYConfig
void SaveConfig() {
	uint32_t size = GetConfigSize();
	FlashWriteData((uint8_t *)&PPKYConfig, size);

	// Читаем обратно из Flash в SavedPPKYConfig — проверяем, что запись прошла
	uint32_t cfg_addr = SPIF_SectorToAddress(FLASH_CFG_START_SECTOR);
	PPKYConfigHeader hdr;
	SPIF_ReadAddress(&hFlash, cfg_addr, (uint8_t *)&hdr, sizeof(hdr));

	if ((hdr.magic == PPKY_CFG_HEADER_MAGIC) && (hdr.size == size)) {
		ReadSavedConfig();

	} else {
		// Что-то пошло не так, оставляем SavedPPKYConfig равным локальной конфигурации
		SavedPPKYConfig = PPKYConfig;
	}
}

// Запись 4-байтового слова в локальную конфигурацию (big-endian)
void SetConfigWord(uint16_t num, uint32_t word) { // set 4 bytes
	uint32_t byte_index = (uint32_t)num * 4U;
	uint32_t cfg_size = GetConfigSize();

	if (byte_index + 4U > cfg_size) {
		// За пределами диапазона – игнорируем
		return;
	}

	uint8_t *p = (uint8_t *)&PPKYConfig;
	p[byte_index + 0] = (uint8_t)((word >> 24) & 0xFF);
	p[byte_index + 1] = (uint8_t)((word >> 16) & 0xFF);
	p[byte_index + 2] = (uint8_t)((word >> 8)  & 0xFF);
	p[byte_index + 3] = (uint8_t)((word >> 0)  & 0xFF);
}

// Размер конфигурации (в байтах)
uint32_t GetConfigSize() { // get config size in bytes
	return (uint32_t)sizeof(PPKYConfig);
}

// Чтение 4-байтового слова из локальной конфигурации (big-endian)
uint32_t GetConfigWord(uint16_t num) { // get 4 bytes
	uint32_t byte_index = (uint32_t)num * 4U;
	uint32_t cfg_size = GetConfigSize();

	if (byte_index + 4U > cfg_size) {
		// За пределами диапазона – возвращаем 0
		return 0;
	}

	uint8_t *p = (uint8_t *)&PPKYConfig;
	uint32_t word = 0;
	word |= ((uint32_t)p[byte_index + 0] << 24);
	word |= ((uint32_t)p[byte_index + 1] << 16);
	word |= ((uint32_t)p[byte_index + 2] << 8);
	word |= ((uint32_t)p[byte_index + 3] << 0);

	return word;
}

/** Простой LCG для псевдослучайных ID МКУ (без rand) */
static uint32_t mku_id_seed(uint32_t seed, uint32_t i) {
	return seed + i * 7919u;  /* простое число для разброса */
}

void FillConfigTemplate(void) {
	/* Сохраняем ID ППКУ */
	// Заполняем UniqId из уникального идентификатора STM
	uint32_t uid0 = HAL_GetUIDw0();
	uint32_t uid1 = HAL_GetUIDw1();
	uint32_t uid2 = HAL_GetUIDw2();

	PPKYConfig.UId.UId0 = uid0;
	PPKYConfig.UId.UId1 = uid1;
	PPKYConfig.UId.UId2 = uid2;
	PPKYConfig.UId.UId3 = HAL_GetDEVID();
	PPKYConfig.UId.UId4 = 1;

	PPKYConfig.UId.devId.zone  = 0;
	PPKYConfig.UId.devId.l_adr = 0;

	uint8_t hadr = (uint8_t)(uid0 & 0xFF);
	if (hadr == 0) {
		hadr = (uint8_t)(uid1 & 0xFF);
		if (hadr == 0) {
			hadr = 1; // на всякий случай, чтобы не был 0
		}
	}
	PPKYConfig.UId.devId.h_adr = hadr;
	PPKYConfig.UId.devId.d_type = DEVICE_PPKY_TYPE;

	// Примеры значений по умолчанию
	PPKYConfig.beep = 1; // звук включен

	/* Обнуляем карту МКУ и зоны */
	memset(&PPKYConfig.CfgDevices, 0, sizeof(PPKYConfig.CfgDevices));
	for (uint16_t z = 0; z < ZONE_NUMBER; z++) {
		memset(PPKYConfig.zone_name[z], 0, ZONE_NAME_SIZE);
	}

	uint32_t seed = HAL_GetUIDw0() ^ HAL_GetUIDw1();

	/* Шаблон: 9 МКУ, H_adr 1..9
	 * Зона 1: МКУ_ТС (h_adr=1), МКУ_игнитер (2), МКУ_игнитер (3)
	 * Зона 2: МКУ_ТС (4), МКУ_игнитер (5), МКУ_игнитер (6)
	 * Зона 3: МКУ_ТС (7), МКУ_игнитер (8), МКУ_игнитер (9) */
	const uint8_t zone_map[]   = {1, 1, 1, 2, 2, 2, 3, 3, 3};
	const uint8_t h_adr_map[]  = {1, 2, 3, 4, 5, 6, 7, 8, 9};
	const uint8_t mcu_type[]   = {DEVICE_MCU_TC_TYPE, DEVICE_MCU_IGN_TYPE, DEVICE_MCU_IGN_TYPE,
	                              DEVICE_MCU_TC_TYPE, DEVICE_MCU_IGN_TYPE, DEVICE_MCU_IGN_TYPE,
	                              DEVICE_MCU_TC_TYPE, DEVICE_MCU_IGN_TYPE, DEVICE_MCU_IGN_TYPE};

	for (uint8_t i = 0; i < 9u; i++) {
		MKUCfg *m = &PPKYConfig.CfgDevices[i];
		m->UId.UId0 = mku_id_seed(0x10000000u, i);
		m->UId.UId1 = mku_id_seed(0x20000000u, i);
		m->UId.UId2 = mku_id_seed(0x30000000u, i);
		m->UId.UId3 = mku_id_seed(seed, i);
		m->UId.UId4 = (uint32_t)(i + 1u);

		m->UId.devId.zone  = zone_map[i] & 0x7Fu;
		m->UId.devId.h_adr = h_adr_map[i];
		m->UId.devId.l_adr = 0;
		m->UId.devId.d_type = mcu_type[i];

		if (mcu_type[i] == DEVICE_MCU_IGN_TYPE) {
			m->VDtype[0] = DT_IGN;
			//m->VDtype[1] = DT_DPT;
		} else {
			m->VDtype[0] = DT_DPT;
		}
	}

	/* Имена зон */
	strncpy((char *)PPKYConfig.zone_name[0], "Е-ПАНЕЛЬ", ZONE_NAME_SIZE - 1);
	PPKYConfig.zone_name[0][ZONE_NAME_SIZE - 1] = '\0';
	strncpy((char *)PPKYConfig.zone_name[1], "КОНДИЦИОНЕР", ZONE_NAME_SIZE - 1);
	PPKYConfig.zone_name[1][ZONE_NAME_SIZE - 1] = '\0';
	strncpy((char *)PPKYConfig.zone_name[2], "МОТОРНЫЙ ОТСЕК", ZONE_NAME_SIZE - 1);
	PPKYConfig.zone_name[2][ZONE_NAME_SIZE - 1] = '\0';

	/* Восстанавливаем ID ППКУ и beep */
	//PPKYConfig.UId = ppky_uid;
	//PPKYConfig.beep = ppky_beep;
}

void ResetConfig() {
	// Установка конфигурации по умолчанию в локальный буфер (PPKYConfig)
	memset(&PPKYConfig, 0, sizeof(PPKYConfig));
}

void DefaultConfig() {
	// Установка конфигурации по умолчанию в локальный буфер (PPKYConfig)
	memset(&PPKYConfig, 0, sizeof(PPKYConfig));

	// Заполняем UniqId из уникального идентификатора STM
	uint32_t uid0 = HAL_GetUIDw0();
	uint32_t uid1 = HAL_GetUIDw1();
	uint32_t uid2 = HAL_GetUIDw2();

	PPKYConfig.UId.UId0 = uid0;
	PPKYConfig.UId.UId1 = uid1;
	PPKYConfig.UId.UId2 = uid2;
	PPKYConfig.UId.UId3 = HAL_GetDEVID();
	PPKYConfig.UId.UId4 = 1;

	PPKYConfig.UId.devId.zone  = 0;
	PPKYConfig.UId.devId.l_adr = 0;

	uint8_t hadr = (uint8_t)(uid0 & 0xFF);
	if (hadr == 0) {
		hadr = (uint8_t)(uid1 & 0xFF);
		if (hadr == 0) {
			hadr = 1; // на всякий случай, чтобы не был 0
		}
	}
	PPKYConfig.UId.devId.h_adr = hadr;
	PPKYConfig.UId.devId.d_type = DEVICE_PPKY_TYPE;

	// Примеры значений по умолчанию
	PPKYConfig.beep = 1; // звук включен

	// Имена зон очищаем (пустые строки)
	for (uint16_t i = 0; i < ZONE_NUMBER; i++) {
		memset(PPKYConfig.zone_name[i], 0, ZONE_NAME_SIZE);
	}

	// reserv оставляем нулевым
}

void FlashWriteData(uint8_t *ConfigPtr, uint32_t ConfigSize) {
	// Конфигурация: FLASH_CFG_START_SECTOR, заголовок PPKYConfigHeader, затем PPKYCfg.
	// Стираем только нужное кол-во секторов (ускоряет сохранение).
	uint32_t cfg_addr = SPIF_SectorToAddress(FLASH_CFG_START_SECTOR);

	for (uint32_t s = 0; s < FLASH_CFG_SECTORS_USED; s++) {
		SPIF_EraseSector(&hFlash, FLASH_CFG_START_SECTOR + s);
	}

	PPKYConfigHeader hdr;
	hdr.magic   = PPKY_CFG_HEADER_MAGIC;
	hdr.version = 1;
	hdr.size    = ConfigSize;

	// Сначала пишем заголовок
	SPIF_WriteAddress(&hFlash, cfg_addr, (uint8_t *)&hdr, sizeof(hdr));
	// Затем полезные данные конфигурации сразу после заголовка
	SPIF_WriteAddress(&hFlash, cfg_addr + sizeof(PPKYConfigHeader), ConfigPtr, ConfigSize);
}
