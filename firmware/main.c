/* PM32F407 SWD programmer - MILESTONE 3
   Port of swd_bitbang_via_jlink.py to native C on PM32F407.
   Runs SWD master at ~MHz (not kHz like J-Link proxied).
   Results written to SRAM so J-Link can read them back without UART.

   SRAM layout at g_result[0..19] (0x20000000 + offset):
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
     [18..19] reserved
*/
#include <stdint.h>
#include "target.h"

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
   Slots 13+ were colliding in the earlier monolithic version; expanded to 20. */
volatile uint32_t g_result[20] __attribute__((used));

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

    /* 7. Write AP CSW = 0x00000002 (word size, no auto-inc) */
    ack = swd_write_txn(1, 0b00, 0x00000002);
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

    /* 12. ERASE page 0 of Flash, with SR checks between each step */

    /* Read FLASH_SR pre-erase */
    uint32_t sr_pre = swd_ap_mem_read(0x4002200C, &at, &ad, &ar);

    /* Set CR.PER (bit 1) */
    swd_write_txn(1, 0b01, 0x40022010);
    swd_write_txn(1, 0b11, 0x00000002);
    swd_idle(8);

    /* Set FLASH_AR */
    swd_write_txn(1, 0b01, 0x40022014);
    swd_write_txn(1, 0b11, 0x08000000);
    swd_idle(8);

    /* Set CR = PER | STRT (0x42) */
    swd_write_txn(1, 0b01, 0x40022010);
    swd_write_txn(1, 0b11, 0x00000042);
    swd_idle(8);
    delay_busy(100000);   /* Flash erase can take ms */

    /* Poll BSY */
    uint32_t sr_erase = 0;
    for (int i = 0; i < 50000; i++) {
        sr_erase = swd_ap_mem_read(0x4002200C, &at, &ad, &ar);
        if ((sr_erase & 1) == 0) break;
    }
    g_result[15] = sr_erase;   /* expect EOP=bit5=1, BSY=0 */

    /* Clear CR */
    swd_write_txn(1, 0b01, 0x40022010);
    swd_write_txn(1, 0b11, 0x00000000);
    swd_idle(8);

    /* 13. Switch AP CSW to HALFWORD (Size=001) — STM32F0 requires halfword writes to Flash */
    swd_write_txn(1, 0b00, 0x00000001);
    swd_idle(8);

    /* Set CR.PG (bit 0) */
    swd_write_txn(1, 0b01, 0x40022010);
    swd_write_txn(1, 0b11, 0x00000001);
    swd_idle(8);

    /* Write halfword 0x1234 at Flash[0] */
    swd_write_txn(1, 0b01, 0x08000000);
    swd_write_txn(1, 0b11, 0x00001234);
    swd_idle(8);
    delay_busy(10000);

    /* Poll BSY */
    uint32_t sr_prog = 0;
    for (int i = 0; i < 50000; i++) {
        sr_prog = swd_ap_mem_read(0x4002200C, &at, &ad, &ar);
        if ((sr_prog & 1) == 0) break;
    }

    /* Clear CR */
    swd_write_txn(1, 0b01, 0x40022010);
    swd_write_txn(1, 0b11, 0x00000000);
    swd_idle(8);

    /* Switch CSW back to WORD for readback */
    swd_write_txn(1, 0b00, 0x00000002);
    swd_idle(8);

    /* 14. Read Flash[0] back — expect 0xFFFF1234 (0x1234 in low halfword, erased 0xFFFF upper) */
    g_result[16] = swd_ap_mem_read(0x08000000, &at, &ad, &ar);

    /* 15. Final FLASH_SR */
    g_result[17] = sr_prog;

    /* Idle loop */
    while (1) { __asm__ volatile("nop"); }
    return 0;
}
