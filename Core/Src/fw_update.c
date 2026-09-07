#include "fw_update.h"
#include "boot_layout.h"
#include "main.h"
#include "spif.h"

extern SPIF_HandleTypeDef hFlash;

/* 0xFFFFFFFF — сессия не начата; иначе номер последнего стёртого 64 КБ блока. */
static uint32_t s_erased_block = 0xFFFFFFFFu;

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

static uint8_t EraseBlockIfNeeded(uint32_t addr)
{
	uint32_t block = SPIF_AddressToBlock(addr);

	if (s_erased_block == block) {
		return 1u;
	}
	if (!SPIF_EraseBlock(&hFlash, block)) {
		return 0u;
	}
	s_erased_block = block;
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
	/* Новая сессия: повтор слова 0 не должен полагаться на прошлый блок. */
	if (num == 0u) {
		s_erased_block = 0xFFFFFFFFu;
	}

	addr = FLASH_FW_UPDATE_ADDR + (num * 4u);
	if (!EraseBlockIfNeeded(addr)) {
		return 0u;
	}
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
	s_erased_block = 0xFFFFFFFFu;
	HAL_Delay(30u);
	NVIC_SystemReset();
	return 1u;
}
