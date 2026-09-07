#include "fw_update.h"
#include "boot_layout.h"
#include "main.h"
#include "spif.h"

extern SPIF_HandleTypeDef hFlash;

void Boot_WriteProgramWatchDog(void)
{
	uint32_t quad_word[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, WATCHDOG };

	if (*(volatile uint32_t *)BOOT_PROGRAM_WD_ADDR == WATCHDOG) {
		return;
	}

	(void)HAL_ICACHE_Disable();
	HAL_FLASH_Unlock();
	(void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, BOOT_PROGRAM_WD_QWORD_ADDR, (uint32_t)quad_word);
	HAL_FLASH_Lock();
	(void)HAL_ICACHE_Invalidate();
	(void)HAL_ICACHE_Enable();
}

static uint8_t EraseUpdateSlot(void)
{
	uint32_t i;
	for (i = 0; i < FLASH_FW_SLOT_BLOCKS; i++) {
		if (!SPIF_EraseBlock(&hFlash, FLASH_FW_UPDATE_BLOCK + i)) {
			return 0u;
		}
	}
	return 1u;
}

uint8_t SetUpdateWord(uint32_t num, uint32_t word)
{
	const uint32_t max_words = FLASH_FW_SLOT_SIZE / 4u;
	uint32_t addr;
	uint32_t le = word;

	if (num >= max_words) {
		return 0u;
	}
	if (num == 0u) {
		if (!EraseUpdateSlot()) {
			return 0u;
		}
	}

	addr = FLASH_FW_UPDATE_ADDR + (num * 4u);
	if (!SPIF_WriteAddress(&hFlash, addr, (uint8_t *)&le, sizeof(le))) {
		return 0u;
	}
	return 1u;
}

uint8_t GetUpdateWord(uint32_t num, uint32_t *word)
{
	const uint32_t max_words = FLASH_FW_SLOT_SIZE / 4u;
	uint32_t addr;
	uint32_t le = 0;

	if ((word == NULL) || (num >= max_words)) {
		return 0u;
	}
	addr = FLASH_FW_UPDATE_ADDR + (num * 4u);
	if (!SPIF_ReadAddress(&hFlash, addr, (uint8_t *)&le, sizeof(le))) {
		return 0u;
	}
	*word = le;
	return 1u;
}

uint8_t FinishUpdateTransmit(void)
{
	HAL_Delay(30u);
	NVIC_SystemReset();
	return 1u;
}
