/* Target-side register map for PM30225V-0401 (HK32F030 silicon, STM32F030 clone).
 *
 * These addresses are accessed via SWD/AP memory transactions, not by this
 * firmware directly — they describe the TARGET chip's memory map.
 *
 * Sources:
 *   - HK32L0xx SDK (Shanghai Hongkai) — FPEC register layout
 *   - STM32F030 reference manual (pin-compatible)
 *   - Empirically verified via J-Link probe: IDCODE=0x0BB11477, AP IDR=0x04770021
 */
#ifndef TARGET_H
#define TARGET_H

/* Flash peripheral (FPEC) — identical to STM32F0 / HK32L0 */
#define TGT_FLASH_BASE      0x40022000
#define TGT_FLASH_ACR       (TGT_FLASH_BASE + 0x00)
#define TGT_FLASH_KEYR      (TGT_FLASH_BASE + 0x04)
#define TGT_FLASH_OPTKEYR   (TGT_FLASH_BASE + 0x08)
#define TGT_FLASH_SR        (TGT_FLASH_BASE + 0x0C)
#define TGT_FLASH_CR        (TGT_FLASH_BASE + 0x10)
#define TGT_FLASH_AR        (TGT_FLASH_BASE + 0x14)
#define TGT_FLASH_OBR       (TGT_FLASH_BASE + 0x1C)

#define TGT_FLASH_KEY1      0x45670123
#define TGT_FLASH_KEY2      0xCDEF89AB

/* FLASH_CR bits */
#define TGT_FLASH_CR_PG     (1u << 0)   /* Programming */
#define TGT_FLASH_CR_PER    (1u << 1)   /* Page erase */
#define TGT_FLASH_CR_MER    (1u << 2)   /* Mass erase */
#define TGT_FLASH_CR_OPTPG  (1u << 4)   /* Option byte program */
#define TGT_FLASH_CR_OPTER  (1u << 5)   /* Option byte erase */
#define TGT_FLASH_CR_STRT   (1u << 6)   /* Start */
#define TGT_FLASH_CR_LOCK   (1u << 7)   /* Lock */
#define TGT_FLASH_CR_OBL_LAUNCH (1u << 13)

/* FLASH_SR bits */
#define TGT_FLASH_SR_BSY    (1u << 0)
#define TGT_FLASH_SR_PGERR  (1u << 2)
#define TGT_FLASH_SR_WRPERR (1u << 4)
#define TGT_FLASH_SR_EOP    (1u << 5)

/* Memory regions */
#define TGT_FLASH_USER_BASE 0x08000000
#define TGT_FLASH_PAGE_SIZE 0x400       /* 1 KB per page on F0/HK32 family */
#define TGT_SRAM_BASE       0x20000000

/* Cortex-M0 debug registers (reachable via AHB-AP once CTRL/STAT powered up) */
#define TGT_DHCSR           0xE000EDF0
#define TGT_DHCSR_DBGKEY    0xA05F0000
#define TGT_DHCSR_C_HALT    (1u << 1)
#define TGT_DHCSR_C_DEBUGEN (1u << 0)

/* ARM ADIv5 expected constants */
#define EXPECTED_DP_IDCODE  0x0BB11477  /* Cortex-M0 SW-DP v1 */

#endif /* TARGET_H */
