/* Demo payload. 32 bytes (= 16 halfwords) of identifiable pattern laid out
 * little-endian, so a post-flash hex dump reads: 34 12 78 56 ...
 * For deployment, replace the whole file with:
 *     xxd -i my_target.bin > target_blob.c
 * and rename the emitted array → TARGET_BLOB, its length → TARGET_BLOB_BYTES. */
#include "target_blob.h"

const uint8_t TARGET_BLOB[] = {
    0x34, 0x12,  0x78, 0x56,  0xBC, 0x9A,  0xF0, 0xDE,
    0x1E, 0x0F,  0x3C, 0x2D,  0x5A, 0x4B,  0x78, 0x69,
    0x96, 0x87,  0xB4, 0xA5,  0xD2, 0xC3,  0xF0, 0xE1,
    0xAA, 0xAA,  0x55, 0x55,  0xFF, 0xFF,  0xFE, 0xCA,
};

const uint32_t TARGET_BLOB_BYTES = sizeof(TARGET_BLOB);
