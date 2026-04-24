/* Target payload embedded into the programmer firmware.
 *
 * The programmer streams TARGET_BLOB into the target's Flash starting at
 * TARGET_BLOB_ADDR. The API is byte-based (uint8_t) so the usual
 *     xxd -i my_target.bin > target_blob.c
 * and a light rename (the array → TARGET_BLOB, the len → TARGET_BLOB_BYTES)
 * is all it takes to drop a real HK32F030 image in.
 *
 * Constraints (STM32F0 / HK32F030 FPEC):
 *   - The programmer composes halfwords (16 bits) from byte pairs in little-
 *     endian order (target storage order). An odd trailing byte is padded
 *     with 0xFF at program time — 0xFF leaves the high byte at erased state.
 *   - The address range [TARGET_BLOB_ADDR, +TARGET_BLOB_BYTES) is erased
 *     page-by-page; the programmer uses TGT_FLASH_PAGE_SIZE from
 *     firmware/target.h (HK32F030 is 512 bytes = 0x200, not the 1024 you
 *     would find on genuine STM32F030 — see PR #12).
 *   - Writing halfwords at odd word-offsets requires a byte-lane shift on
 *     the AP DRW; handled internally by flash_program_hw. If you ever
 *     bypass that helper, see PR #14 + docs/swd_quirks.md quirk #4 for
 *     why raw drw_val = (uint32_t)hw silently corrupts every high halfword.
 */
#ifndef TARGET_BLOB_H
#define TARGET_BLOB_H

#include <stdint.h>

#define TARGET_BLOB_ADDR 0x08000000u

extern const uint8_t  TARGET_BLOB[];
extern const uint32_t TARGET_BLOB_BYTES;

#endif /* TARGET_BLOB_H */
