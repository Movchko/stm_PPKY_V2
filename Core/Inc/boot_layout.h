#ifndef BOOT_LAYOUT_H_
#define BOOT_LAYOUT_H_

#include <stdint.h>

/* --- Internal flash (STM32H523RE, 512 KB, dual-bank, 8 KB sectors) --- */
#define BOOT_FLASH_BASE              0x08000000u
/* text+data = 28220; +20% = 33864. 4 сектора = 32768 (<20%), 5 секторов = 40 КБ. */
#define BOOTLOADER_CODE_SIZE         0x0000A000u  /* 40 KB: sectors 0..4 */
#define BOOTLOADER_REGION_SIZE       0x0000C000u  /* 48 KB: code + footer sector 5 */
#define BOOT_WD_QWORD_ADDR           (BOOT_FLASH_BASE + BOOTLOADER_CODE_SIZE - 16u) /* 0x08009FF0 */
#define MAIN_APP_START_ADDR          0x0800C000u
#define MAIN_APP_SIZE                0x00074000u  /* 464 KB */
#define FLASH_FOOTER_SZ              64u
#define MAIN_FOOTER_ADDR             (MAIN_APP_START_ADDR - FLASH_FOOTER_SZ) /* 0x0800BFC0 */
#define BOOT_PROGRAM_WD_ADDR         (MAIN_FOOTER_ADDR - 4u)                 /* 0x0800BFBC */
#define BOOT_PROGRAM_WD_QWORD_ADDR   (BOOT_PROGRAM_WD_ADDR & ~0xFu)          /* 0x0800BFB0 */

#define BOOT_APP_FOOTER_SECTOR       5u
#define BOOT_APP_BANK1_SECTOR        BOOT_APP_FOOTER_SECTOR
#define BOOT_APP_BANK1_NB_SECTORS    (32u - BOOT_APP_BANK1_SECTOR)
#define BOOT_APP_BANK2_SECTOR        0u
#define BOOT_APP_BANK2_NB_SECTORS    32u

#define BOOT_CRC_START               0x1111u
#define WATCHDOG                     0xAABBCCDDu
#define BOOT_BLANK_FOOTER_CRC        0x6e45ea5du

/* --- External W25Q128 (16 MB, 4 KB sector, 64 KB block) --- */
#define SPI_FLASH_SECTOR_COUNT       4096u
#define FLASH_CFG_ERASE_BLOCKS       2u
#define FLASH_FW_SLOT_SIZE           0x00080000u
#define FLASH_FW_FACTORY_ADDR        0x00020000u
#define FLASH_FW_UPDATE_ADDR         0x000A0000u
#define FLASH_FW_FACTORY_BLOCK       (FLASH_FW_FACTORY_ADDR / 0x10000u)
#define FLASH_FW_UPDATE_BLOCK        (FLASH_FW_UPDATE_ADDR / 0x10000u)
#define FLASH_FW_SLOT_BLOCKS         (FLASH_FW_SLOT_SIZE / 0x10000u)
#define FLASH_LOG_START_SECTOR       288u

#define UPDATE_APP_ADDR              FLASH_FW_UPDATE_ADDR
#define DEFAULT_APP_ADDR             FLASH_FW_FACTORY_ADDR

#endif /* BOOT_LAYOUT_H_ */
