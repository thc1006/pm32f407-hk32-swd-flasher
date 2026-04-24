/* PM32F407 SWD programmer
   Port of swd_bitbang_via_jlink.py to native C on PM32F407.
   Runs SWD master at ~1 MHz (versus ~kHz when proxied through J-Link).
   Results written to SRAM so J-Link, the probe firmware (firmware/probe_main.c),
   or any other SWD master can read them back without needing the UART path.

   SRAM layout at g_result[0..23] (0x20000000 + offset):
     [0]  marker          0xBEEF0001 (firmware started)
     [1]  idcode_ack      ACK for DP IDCODE read (expect 0x1 = OK)
     [2]  idcode          DP IDCODE (expect 0x0BB11477)
     [3]  abort_ack       ACK for DP ABORT write
     [4]  select_ack      ACK for DP SELECT = 0 write
     [5]  ctrlstat        CTRL/STAT readback (expect 0xF0000040 = power up + READOK)
     [6]  select_f0_ack   ACK for SELECT APBANKSEL=0xF
     [7]  ap_posted_stale first posted AP read (discard)
     [8]  ap_idr          AHB-AP IDR (expect ARM signature, e.g. 0x04770021)
     [9]  csw_readback    CSW after our write (expect 0x03000042)
     [10] sram_wr_acks    packed ACKs from SRAM write (TAR<<16 | DRW<<8)
     [11] sram_readback   SRAM roundtrip value (expect 0xDEADBEEF)
     [12] sram_rd_acks    packed ACKs from SRAM read
     [13] flash_cr_locked FLASH_CR before unlock (expect bit 7 = LOCK set)
     [14] flash_cr_unlocked FLASH_CR after unlock (expect bit 7 = 0)
     [15] sr_after_erase  FLASH_SR after page erase (expect EOP bit 5 set)
     [16] flash_readback  Flash[0] after program (expect 0xFFFF1234)
     [17] sr_after_program FLASH_SR after halfword program
     [18] flash_op_status   H3 status code: 0x0000_0001 OK, 0x8000_00EE erase
                            failed (XX = sr bits), 0x4000_00EE program failed
     [19] aircr_reset_acks  H4: SYSRESETREQ write ACKs — (tar<<16 | drw<<24)
     [20] hwcount           H5: halfword count attempted (from TARGET_BLOB_BYTES)
     [21] pages_erased      H5: page erases that completed OK
     [22] verify_mismatches H5: readback halfwords that didn't match blob
     [23] first_mismatch    H5: index of first mismatch (0xFFFFFFFF if none)
*/
#include <stddef.h>
#include <stdint.h>
#include "target.h"
#include "target_blob.h"

/* PM32F407 peripheral map (STM32F407-compatible) */
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830)
#define GPIOC_MODER (*(volatile uint32_t *)0x40020800)
#define GPIOC_IDR   (*(volatile uint32_t *)0x40020810)
#define GPIOC_BSRR  (*(volatile uint32_t *)0x40020818)

#define GPIOC_EN    (1u << 2)
#define SWDIO_PIN   0
#define SWCLK_PIN   2
#define SWDIO_MASK  (1u << SWDIO_PIN)
#define SWCLK_MASK  (1u << SWCLK_PIN)

/* Results — placed in .bss, accessible via J-Link at 0x20000000 + offset.
   Slots 13+ were colliding in the earlier monolithic version; expanded to 24
   so H5 can carry its four summary values without collision. */
volatile uint32_t g_result[24] __attribute__((used));

/* ---------- low-level GPIO + SWD bit-bang ---------- */

static inline void swclk_high(void) { GPIOC_BSRR = SWCLK_MASK; }
static inline void swclk_low(void)  { GPIOC_BSRR = SWCLK_MASK << 16; }
static inline void swdio_high(void) { GPIOC_BSRR = SWDIO_MASK; }
static inline void swdio_low(void)  { GPIOC_BSRR = SWDIO_MASK << 16; }
static inline int  swdio_read(void) { return (GPIOC_IDR >> SWDIO_PIN) & 1; }

static inline void swdio_output(void) {
    uint32_t m = GPIOC_MODER;
    m &= ~(3u << (SWDIO_PIN * 2));
    m |=  (1u << (SWDIO_PIN * 2));
    GPIOC_MODER = m;
}
static inline void swdio_input(void) {
    uint32_t m = GPIOC_MODER;
    m &= ~(3u << (SWDIO_PIN * 2));
    GPIOC_MODER = m;
}

/* Short delay for SWD edge timing — keeps signal stable */
static inline void swd_delay(void) {
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
}

/* One SWD clock cycle: fall, settle, rise, settle */
static inline void swd_clk(void) {
    swclk_low(); swd_delay();
    swclk_high(); swd_delay();
}

static inline void swd_write_bit(int bit) {
    if (bit) swdio_high(); else swdio_low();
    swd_clk();
}

static inline int swd_read_bit(void) {
    swclk_low(); swd_delay();
    swclk_high(); swd_delay();
    return swdio_read();
}

static void swd_write_bits_lsb(uint32_t v, int nbits) {
    for (int i = 0; i < nbits; i++) swd_write_bit((v >> i) & 1);
}

static uint32_t swd_read_bits_lsb(int nbits) {
    uint32_t v = 0;
    for (int i = 0; i < nbits; i++)
        v |= ((uint32_t)swd_read_bit()) << i;
    return v;
}

static int parity32(uint32_t v) {
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
}

/* ---------- SWD protocol ---------- */

/* Line reset: ≥50 clocks SWDIO high, then ≥2 idle */
static void swd_line_reset(void) {
    swdio_output();
    swdio_high();
    for (int i = 0; i < 60; i++) swd_clk();
    swdio_low();
    for (int i = 0; i < 4; i++) swd_clk();
}

/* JTAG-to-SWD switch: 16 bits of 0xE79E LSB-first */
static void swd_jtag_to_swd(void) {
    swdio_output();
    swd_write_bits_lsb(0xE79E, 16);
}

static uint8_t swd_request(int apndp, int rnw, int addr23) {
    int a2 = addr23 & 1;
    int a3 = (addr23 >> 1) & 1;
    int p = apndp ^ rnw ^ a2 ^ a3;
    return (uint8_t)((1) | (apndp << 1) | (rnw << 2) |
                     (a2 << 3) | (a3 << 4) | (p << 5) | (0 << 6) | (1 << 7));
}

/* ACK values (LSB-first encoded, per ARM ADIv5 §B4.2.2) */
#define SWD_ACK_OK      0x1
#define SWD_ACK_WAIT    0x2
#define SWD_ACK_FAULT   0x4
#define SWD_MAX_WAIT_RETRIES        8
#define SWD_WAIT_RETRY_IDLE_CYCLES  4   /* brief idle between WAIT retries */

/* Forward decl: DP ABORT helper used by FAULT-recovery branches below. */
static uint32_t swd_abort_clear_stickies_raw(void);

/* Internal: single SWD read attempt (no retry). On non-OK ACK, the 32 data
   bits clocked after ACK are not meaningful per ADIv5, so we leave *data_out
   unchanged — avoids leaking undefined bits to callers that forget to gate
   on the return value. */
static uint32_t swd_read_txn_once(int apndp, int addr23, uint32_t *data_out) {
    uint8_t req = swd_request(apndp, 1, addr23);
    swdio_output();
    swd_write_bits_lsb(req, 8);
    swdio_input();
    uint32_t ack = swd_read_bits_lsb(3);
    uint32_t data = swd_read_bits_lsb(32);
    (void)swd_read_bit();  /* parity bit, not validated here */
    /* TRN back + 1 idle */
    swdio_output();
    swdio_low();
    swd_clk();
    if (ack == SWD_ACK_OK) {
        *data_out = data;
    }
    return ack;
}

/* SWD read with WAIT retry (ADIv5 §B4.2.1) + FAULT recovery (ABORT clear-
   stickies + one retry, same H2 semantics we apply on the write path). A
   read is just as able to wedge the session on FAULT as a write — without
   recovery here the next AP access would also FAULT regardless of the
   stickies getting cleared on the write side. */
static uint32_t swd_read_txn(int apndp, int addr23, uint32_t *data_out) {
    int fault_recovery_left = 1;
    for (int i = 0; i < SWD_MAX_WAIT_RETRIES; i++) {
        uint32_t ack = swd_read_txn_once(apndp, addr23, data_out);
        if (ack == SWD_ACK_OK) return ack;
        if (ack == SWD_ACK_FAULT && fault_recovery_left > 0) {
            fault_recovery_left--;
            (void)swd_abort_clear_stickies_raw();
            swdio_output(); swdio_low();
            for (int j = 0; j < SWD_WAIT_RETRY_IDLE_CYCLES; j++) swd_clk();
            continue;
        }
        if (ack == SWD_ACK_WAIT) {
            swdio_output(); swdio_low();
            for (int j = 0; j < SWD_WAIT_RETRY_IDLE_CYCLES; j++) swd_clk();
            continue;
        }
        return ack;
    }
    return SWD_ACK_WAIT;
}

/* SWD write — shift-compensated with TRUE parity bit at end.
   Target consumes 38 post-request clocks: 3 ACK + 1 skip/TRN + 32 DATA + 1 DATA[31]_alias + 1 PARITY.
   target.DATA[0..30] = wire[1..31] (from shift), target.DATA[31] = parity_bit slot.
   Target's actual PARITY check happens on one more clock — we MUST send parity(data) there,
   otherwise SWDIO idle=0 fails parity check for odd-parity data → WDATAERR cascade. */
static uint32_t swd_write_txn_raw(int apndp, int addr23, uint32_t wire_data, int parity_bit, int real_parity) {
    uint8_t req = swd_request(apndp, 0, addr23);
    swdio_output();
    swd_write_bits_lsb(req, 8);
    swdio_input();
    uint32_t ack = swd_read_bits_lsb(3);
    swd_clk();                           /* TRN2 */
    swdio_output();
    swd_write_bits_lsb(wire_data, 32);
    swd_write_bit(parity_bit);           /* target.DATA[31] slot */
    swd_write_bit(real_parity);          /* target's PARITY check */
    return ack;
}

/* DP ABORT write to clear sticky error flags. Must use the raw primitive
   (not swd_write_txn) to avoid infinite recursion when called from the
   FAULT-recovery paths of swd_read_txn / swd_write_txn. Returns the ABORT
   write's own ACK so callers can observe when the recovery itself failed. */
static uint32_t swd_abort_clear_stickies_raw(void) {
    /* data = 0x1E = STKCMPCLR(bit 1) | STKERRCLR(2) | WDERRCLR(3) | ORUNERRCLR(4) */
    const uint32_t data = 0x0000001Eu;
    uint32_t wire_data = (data & 0x7FFFFFFFu) << 1;
    int parity_bit = (int)((data >> 31) & 1u);
    int real_parity = parity32(data);
    return swd_write_txn_raw(0, 0b00, wire_data, parity_bit, real_parity);
}

/* Convenience: target.DATA = `data` with shift-compensation, WAIT retry,
   and FAULT recovery (clear stickies + one retry). */
static uint32_t swd_write_txn(int apndp, int addr23, uint32_t data) {
    uint32_t wire_data = (data & 0x7FFFFFFF) << 1;
    int parity_bit = (data >> 31) & 1;
    int real_parity = parity32(data);
    int fault_recovery_left = 1;
    for (int i = 0; i < SWD_MAX_WAIT_RETRIES; i++) {
        uint32_t ack = swd_write_txn_raw(apndp, addr23, wire_data, parity_bit, real_parity);
        if (ack == SWD_ACK_OK) return ack;
        if (ack == SWD_ACK_FAULT && fault_recovery_left > 0) {
            /* Try once more after explicitly clearing STICKYERR/WDATAERR/etc. */
            fault_recovery_left--;
            (void)swd_abort_clear_stickies_raw();
            swdio_output(); swdio_low();
            for (int j = 0; j < SWD_WAIT_RETRY_IDLE_CYCLES; j++) swd_clk();
            continue;
        }
        if (ack == SWD_ACK_WAIT) {
            swdio_output(); swdio_low();
            for (int j = 0; j < SWD_WAIT_RETRY_IDLE_CYCLES; j++) swd_clk();
            continue;
        }
        return ack;   /* FAULT without recovery budget, or garbage ACK */
    }
    return SWD_ACK_WAIT;
}

static void swd_idle(int n) {
    swdio_output();
    swdio_low();
    for (int i = 0; i < n; i++) swd_clk();
}

/* AP memory read — canonical ADIv5 "post + RDBUFF" pattern.
   1) Write AP TAR with target address.
   2) Read AP DRW — posts the memory fetch; returned data is from the PREVIOUS
      AP access (stale / undefined on first access of a session per ADIv5).
   3) Read DP RDBUFF — returns the just-fetched value WITHOUT triggering a new
      AP access. This is what we want for point reads.
   Older versions of this function did a second AP DRW read instead, which
   also "worked" because our CSW has AddrInc off (second post re-fetches the
   same address), but it's a wasted transaction and is wrong the moment
   AddrInc gets enabled for bulk reads later. */
static uint32_t swd_ap_mem_read(uint32_t addr, uint32_t *ack_tar, uint32_t *ack_drw, uint32_t *ack_rdbuff) {
    uint32_t data = 0;

    *ack_tar = swd_write_txn(1, 0b01, addr);
    swd_idle(8);

    *ack_drw = swd_read_txn(1, 0b11, &data);    /* posts fetch; returns stale */
    swd_idle(8);

    *ack_rdbuff = swd_read_txn(0, 0b11, &data); /* DP RDBUFF returns fetched value */
    swd_idle(8);

    return data;
}

/* ---------- Flash programming helpers (H5) ---------- */

/* Poll FLASH_SR until BSY=0 or loop budget exhausted. *bsy_cleared flags
 * whether BSY was actually observed clear (vs. timed out). Caller MUST
 * check it before treating the op as complete. */
static uint32_t flash_poll_sr_until_not_bsy(int *bsy_cleared) {
    uint32_t sr = 0;
    uint32_t at, ad, ar;
    int cleared = 0;
    for (int i = 0; i < 50000; i++) {
        sr = swd_ap_mem_read(TGT_FLASH_SR, &at, &ad, &ar);
        if ((sr & TGT_FLASH_SR_BSY) == 0) { cleared = 1; break; }
    }
    if (bsy_cleared) *bsy_cleared = cleared;
    return sr;
}

/* Non-zero iff caller should treat the op as failed. */
static inline uint32_t flash_op_failure(uint32_t sr, int bsy_cleared) {
    uint32_t bad = sr & (TGT_FLASH_SR_PGERR | TGT_FLASH_SR_WRPERR);
    if (!bsy_cleared) bad |= TGT_FLASH_SR_BSY;
    return bad;
}

/* Erase one 1 KB page. CSW must already be WORD. Leaves CR zeroed. */
static uint32_t flash_erase_page(uint32_t page_addr, int *bsy_cleared) {
    swd_write_txn(1, 0b01, TGT_FLASH_CR);
    swd_write_txn(1, 0b11, TGT_FLASH_CR_PER);
    swd_idle(8);
    swd_write_txn(1, 0b01, TGT_FLASH_AR);
    swd_write_txn(1, 0b11, page_addr);
    swd_idle(8);
    swd_write_txn(1, 0b01, TGT_FLASH_CR);
    swd_write_txn(1, 0b11, TGT_FLASH_CR_PER | TGT_FLASH_CR_STRT);
    swd_idle(8);
    uint32_t sr = flash_poll_sr_until_not_bsy(bsy_cleared);
    swd_write_txn(1, 0b01, TGT_FLASH_CR);
    swd_write_txn(1, 0b11, 0);
    swd_idle(8);
    return sr;
}

/* Program one halfword. CSW must already be HALFWORD. Leaves CR zeroed. */
static uint32_t flash_program_hw(uint32_t addr, uint16_t hw, int *bsy_cleared) {
    /* AHB-AP halfword byte-lane rule: when CSW size=halfword, the active byte
     * lane is selected by addr[1:0]. For an address with bit 1 set (odd
     * halfword offset within a word), the payload must sit in DRW[31:16];
     * otherwise the target receives 0x0000 on that lane and every upper
     * halfword of every word gets silently zeroed. This bug survived the
     * AP verify loop because the verify read uses the same CSW mode and
     * gets the same lane back — a classic "wrote 0, read 0, no mismatch"
     * false-positive. Symptom on HK32F030: target Flash ends up with every
     * high halfword = 0 including the reset vector, so CPU hard-faults on
     * first instruction and Q_Example never boots. */
    uint32_t drw_val = (addr & 2u) ? ((uint32_t)hw << 16) : (uint32_t)hw;
    swd_write_txn(1, 0b01, TGT_FLASH_CR);
    swd_write_txn(1, 0b11, TGT_FLASH_CR_PG);
    swd_idle(8);
    swd_write_txn(1, 0b01, addr);
    swd_write_txn(1, 0b11, drw_val);
    swd_idle(8);
    uint32_t sr = flash_poll_sr_until_not_bsy(bsy_cleared);
    swd_write_txn(1, 0b01, TGT_FLASH_CR);
    swd_write_txn(1, 0b11, 0);
    swd_idle(8);
    return sr;
}

/* ---------- main ---------- */

static inline void delay_busy(volatile uint32_t n) {
    while (n--) __asm__ volatile("nop");
}

int main(void) {
    /* Enable GPIOC clock */
    RCC_AHB1ENR |= GPIOC_EN;

    /* PC0 output (SWDIO), PC2 output (SWCLK) */
    uint32_t moder = GPIOC_MODER;
    moder &= ~((3u << (SWDIO_PIN*2)) | (3u << (SWCLK_PIN*2)));
    moder |=   (1u << (SWDIO_PIN*2)) | (1u << (SWCLK_PIN*2));
    GPIOC_MODER = moder;

    /* Mark that firmware started */
    g_result[0] = 0xBEEF0001;

    /* SWD init sequence */
    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();

    uint32_t data = 0;
    uint32_t ack;

    /* 1. Read DP IDCODE */
    ack = swd_read_txn(0, 0b00, &data);
    g_result[1] = ack;
    g_result[2] = data;
    swd_idle(16);

    /* 2. Write DP ABORT = 0x1E (clear all sticky flags) */
    ack = swd_write_txn(0, 0b00, 0x0000001E);
    g_result[3] = ack;
    swd_idle(16);

    /* 3. Write DP SELECT = 0 (DPBANKSEL=0, APSEL=0, APBANKSEL=0) */
    ack = swd_write_txn(0, 0b10, 0x00000000);
    g_result[4] = ack;
    swd_idle(16);

    /* 4. Power up debug via CTRL/STAT */
    ack = swd_write_txn(0, 0b01, 0x50000000);
    swd_idle(16);
    delay_busy(100000);
    ack = swd_read_txn(0, 0b01, &data);
    g_result[5] = data;   /* CTRL/STAT readback — expect 0xF0000040 */
    swd_idle(8);

    /* 5. SANITY TEST: Read AP IDR (AP bank 0xF, A[3:2]=11 = offset 0xFC = IDR)
          Expected: 0x24770011 (AHB-AP per J-Link earlier probe) */
    ack = swd_write_txn(0, 0b10, 0x000000F0);  /* SELECT: APSEL=0, APBANKSEL=0xF */
    g_result[6] = ack;
    swd_idle(16);

    ack = swd_read_txn(1, 0b11, &data);        /* AP read (posted) */
    g_result[7] = data;                          /* posted (stale/zero) */
    swd_idle(8);

    ack = swd_read_txn(0, 0b11, &data);        /* DP RDBUFF — actual IDR */
    g_result[8] = data;                          /* expected 0x24770011 */
    swd_idle(16);

    /* 6. Re-select AP bank 0 for normal memory access */
    ack = swd_write_txn(0, 0b10, 0x00000000);  /* SELECT back to bank 0 */
    swd_idle(16);

    /* 7. Write AP CSW: word size | MasterType | HPROT | DeviceEn.
     * The upper bits 24 (MasterType) and 25 (HPROT master-like) are required
     * for some AHB-AP implementations to actually issue memory transactions;
     * bit 6 (DeviceEn) gates the memory port. Writing just 0x02 (size only)
     * leaves these three bits clear and the AP silently drops transfers on
     * HK32F030's port. 0x03000042 = Size=Word | DeviceEn | MasterType | HPROT. */
    ack = swd_write_txn(1, 0b00, 0x03000042);
    swd_idle(16);

    /* 8. Read AP CSW to verify */
    swd_read_txn(1, 0b00, &data);              /* posted */
    swd_idle(8);
    swd_read_txn(0, 0b11, &data);              /* RDBUFF */
    g_result[9] = data;                          /* CSW readback */
    swd_idle(16);

    /* 9. CORE TEST: write 0xDEADBEEF to PM30225V SRAM @ 0x20000100, read back */
    uint32_t at, ad, ar;

    /* Write 0xDEADBEEF to target.SRAM[0x20000100] */
    at = swd_write_txn(1, 0b01, 0x20000100);    /* AP TAR */
    swd_idle(8);
    ad = swd_write_txn(1, 0b11, 0xDEADBEEF);    /* AP DRW = data */
    swd_idle(16);
    g_result[10] = (at << 16) | (ad << 8);       /* ACKs of write */

    /* Read back */
    uint32_t rb = swd_ap_mem_read(0x20000100, &at, &ad, &ar);
    g_result[11] = rb;                            /* should be 0xDEADBEEF */
    g_result[12] = (at << 16) | (ad << 8) | ar;

    /* 10. Test HK32 FLASH_CR readback @ 0x40022010 (to see LOCK bit state) */
    uint32_t flash_cr = swd_ap_mem_read(0x40022010, &at, &ad, &ar);
    g_result[13] = flash_cr;                      /* bit 7 = LOCK */

    /* 10b. HALT target CPU via DHCSR before any Flash operation. Without this,
       target CPU continues executing vendor firmware while we erase its Flash —
       if it fetches from a page mid-erase the chip will hardfault and the SWD
       state may wedge. Known issue C1. */
    swd_write_txn(1, 0b01, TGT_DHCSR);
    swd_idle(8);
    swd_write_txn(1, 0b11, TGT_DHCSR_DBGKEY | TGT_DHCSR_C_HALT | TGT_DHCSR_C_DEBUGEN);
    swd_idle(16);

    /* Poll DHCSR.S_HALT (bit 17) until the core reports halted, with a bounded
       retry count so a disconnected / stuck target doesn't freeze us. Each
       iteration costs one AP memory read over bit-banged SWD (~50 µs), so
       2000 iterations gives us ~100 ms — plenty for the CPU to finish its
       current instruction and enter halt. */
    for (int i = 0; i < 2000; i++) {
        uint32_t dhcsr_rb = swd_ap_mem_read(TGT_DHCSR, &at, &ad, &ar);
        if (dhcsr_rb & TGT_DHCSR_S_HALT) break;
    }

    /* 11. Try FLASH unlock: write KEY1, KEY2 to FLASH_KEYR @ 0x40022004 */
    swd_write_txn(1, 0b01, 0x40022004);           /* AP TAR = KEYR */
    swd_idle(8);
    swd_write_txn(1, 0b11, 0x45670123);           /* KEY1 */
    swd_idle(16);
    swd_write_txn(1, 0b01, 0x40022004);
    swd_idle(8);
    swd_write_txn(1, 0b11, 0xCDEF89AB);           /* KEY2 */
    swd_idle(16);
    delay_busy(1000);

    /* Read FLASH_CR again — if unlock worked, LOCK bit (7) should be 0 */
    flash_cr = swd_ap_mem_read(0x40022010, &at, &ad, &ar);
    g_result[14] = flash_cr;                      /* expect bit 7 = 0 after unlock */

    /* Verify FLASH_CR.LOCK actually cleared. Per STM32F0 RM (§3.5.1 FPEC access
     * unlocking) KEY1/KEY2 must be written in strict order; any intervening
     * transaction, a dropped WAIT ACK that re-ordered our writes, or a typo in
     * the key sequence leaves LOCK=1. Once in that state, every subsequent CR
     * write is silently ignored — erase and program will appear to "succeed"
     * on the wire (ACK=OK) but leave Flash untouched, producing a confusing
     * "readback is 0xFFFFFFFF" failure far from the actual cause. Bail now. */
    if (flash_cr & TGT_FLASH_CR_LOCK) {
        /* Sentinel in g_result[15] (normally holds sr_prog) that tells the
         * operator this run aborted before touching Flash. Low 16 bits carry
         * the observed FLASH_CR so the LOCK bit is visible without separate
         * inspection. */
        g_result[15] = 0xDEAD0000u | (flash_cr & 0xFFFFu);
        while (1) { __asm__ volatile("nop"); }
    }

    /* 12. H5: erase enough pages to hold the full TARGET_BLOB (1 KB per page).
     *     Halfword count rounded up so an odd trailing byte still lands in a
     *     fully erased page. Bail on first erase failure so we don't program
     *     over a bad erase. */
    uint32_t hwcount = (TARGET_BLOB_BYTES + 1u) / 2u;
    uint32_t pages_needed =
        (TARGET_BLOB_BYTES + TGT_FLASH_PAGE_SIZE - 1) / TGT_FLASH_PAGE_SIZE;
    uint32_t pages_erased = 0;
    uint32_t erase_errs = 0;
    uint32_t last_sr_erase = 0;
    for (uint32_t p = 0; p < pages_needed; p++) {
        uint32_t page_addr = TARGET_BLOB_ADDR + p * TGT_FLASH_PAGE_SIZE;
        int bsy_cleared = 0;
        last_sr_erase = flash_erase_page(page_addr, &bsy_cleared);
        erase_errs = flash_op_failure(last_sr_erase, bsy_cleared);
        if (erase_errs) break;
        pages_erased++;
    }
    g_result[15] = last_sr_erase;
    g_result[20] = hwcount;
    g_result[21] = pages_erased;
    g_result[22] = 0;
    g_result[23] = 0xFFFFFFFFu;
    g_result[18] = 0;

    uint32_t sr_prog = 0;
    uint32_t prog_errs = 0;

    if (erase_errs != 0) {
        /* Erase failed — skip program + verify. */
        g_result[18] = 0x80000000u | ((erase_errs & 0xFFu) << 8);
        g_result[16] = 0xDEADDEADu;
        g_result[17] = 0xDEADDEADu;
    } else {
        /* 13. Switch AP CSW to HALFWORD — STM32F0 requires halfword program.
         * 0x03000041 = Size=Halfword | DeviceEn | MasterType | HPROT,
         * preserving the context bits from step 7. Writing bare 0x01 again
         * clears them and memory transfers silently drop. */
        swd_write_txn(1, 0b00, 0x03000041);
        swd_idle(8);

        /* 14. Loop halfword-program across blob. Each halfword is composed
         *     from TWO bytes little-endian; odd trailing byte pads 0xFF. */
        for (uint32_t i = 0; i < hwcount; i++) {
            uint32_t byte_idx = i * 2u;
            uint16_t lo = TARGET_BLOB[byte_idx];
            uint16_t hi = (byte_idx + 1u < TARGET_BLOB_BYTES)
                          ? TARGET_BLOB[byte_idx + 1u]
                          : 0xFFu;
            uint16_t hw = (uint16_t)(lo | (hi << 8));
            int bsy_cleared = 0;
            sr_prog = flash_program_hw(TARGET_BLOB_ADDR + byte_idx, hw, &bsy_cleared);
            prog_errs = flash_op_failure(sr_prog, bsy_cleared);
            if (prog_errs) {
                g_result[23] = i;
                break;
            }
        }

        /* Switch CSW back to WORD for readback (preserve MasterType/HPROT/DeviceEn). */
        swd_write_txn(1, 0b00, 0x03000042);
        swd_idle(8);

        g_result[17] = sr_prog;
        g_result[18] = prog_errs ? (0x40000000u | (prog_errs & 0xFFu)) : 0x00000001u;

        if (!prog_errs) {
            /* 15. Verify: walk blob word-at-a-time so each target word is
             *     fetched once. Compare both halfwords against the source. */
            uint32_t mismatches = 0;
            uint32_t first_mismatch = 0xFFFFFFFFu;
            uint32_t i = 0;
            while (i + 1u < hwcount) {
                uint32_t word_addr = TARGET_BLOB_ADDR + i * 2u;
                uint32_t w = swd_ap_mem_read(word_addr, &at, &ad, &ar);
                uint16_t want_lo = (uint16_t)TARGET_BLOB[i * 2u]
                                 | (uint16_t)(TARGET_BLOB[i * 2u + 1u] << 8);
                uint16_t want_hi = (uint16_t)TARGET_BLOB[(i + 1u) * 2u]
                                 | (uint16_t)(TARGET_BLOB[(i + 1u) * 2u + 1u] << 8);
                uint16_t got_lo = (uint16_t)(w & 0xFFFFu);
                uint16_t got_hi = (uint16_t)(w >> 16);
                if (got_lo != want_lo) {
                    mismatches++;
                    if (first_mismatch == 0xFFFFFFFFu) first_mismatch = i;
                }
                if (got_hi != want_hi) {
                    mismatches++;
                    if (first_mismatch == 0xFFFFFFFFu) first_mismatch = i + 1u;
                }
                i += 2u;
            }
            if (i < hwcount) {
                /* Odd trailing halfword in low half of its word. */
                uint32_t word_addr = TARGET_BLOB_ADDR + i * 2u;
                uint32_t w = swd_ap_mem_read(word_addr, &at, &ad, &ar);
                uint32_t byte_idx = i * 2u;
                uint16_t want = (uint16_t)TARGET_BLOB[byte_idx]
                              | (uint16_t)((byte_idx + 1u < TARGET_BLOB_BYTES
                                            ? TARGET_BLOB[byte_idx + 1u] : 0xFFu) << 8);
                uint16_t got = (uint16_t)(w & 0xFFFFu);
                if (got != want) {
                    mismatches++;
                    if (first_mismatch == 0xFFFFFFFFu) first_mismatch = i;
                }
            }
            g_result[16] = (hwcount > 0)
                ? swd_ap_mem_read(TARGET_BLOB_ADDR, &at, &ad, &ar)
                : 0u;
            g_result[22] = mismatches;
            g_result[23] = first_mismatch;
        } else {
            g_result[16] = 0xDEADDEADu;
        }
    }

    /* H4: soft-reset the target so it starts running the new firmware. Without
     * this the Cortex-M0 keeps executing whatever instruction fetch it cached
     * before we rewrote Flash. swd_write_txn still clocks the 3-bit ACK phase;
     * we just don't retry or wait for posted-write completion because the
     * target's debug domain drops sync as soon as SYSRESETREQ fires. Capture
     * both ACKs in g_result[19] so a silent "reset never went out" is visible
     * in SRAM rather than looking identical to a successful reset. */
    uint32_t aircr_tar_ack = swd_write_txn(1, 0b01, TGT_AIRCR);
    swd_idle(8);
    uint32_t aircr_drw_ack = swd_write_txn(1, 0b11, TGT_AIRCR_SYSRESET);
    swd_idle(16);
    g_result[19] = ((aircr_tar_ack & 0xFFu) << 16) | ((aircr_drw_ack & 0xFFu) << 24);

    /* Idle loop */
    while (1) { __asm__ volatile("nop"); }
    return 0;
}
