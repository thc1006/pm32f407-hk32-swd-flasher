/* PM32F407 SWD probe — interactive memory read/write over UART.
 *
 * Sibling tool to the flasher main.c. Lets a host Python script
 * (tools/probe.py) arbitrarily read + write target (PM30225V) memory via
 * SWD with no reflash per diagnostic iteration.
 *
 * Build: swap this file in place of firmware/main.c (e.g.
 *     cp firmware/probe_main.c firmware/main.c
 * ), then run `python3 tools/build.py`. target_blob.c is unused by this
 * build but must still be present because tools/build.py hard-codes the
 * source list. target.h is shared with the flasher.
 *
 *   USART1 on PA9 (TX) / PA10 (RX), AF7, 115200 8N1
 *     routed through onboard CH340C → /dev/ttyUSB* or COMx on host
 *   SWD on PC0 (SWDIO) / PC2 (SWCLK) — same bit-bang wiring as the flasher
 *
 * Protocol (\n-terminated lines, no CR required):
 *     R <hex-addr>              → "R <addr> <val>\r\n"    (word read)
 *     W <hex-addr> <hex-val>    → "W <addr> <val> <tack><dack>\r\n"
 *     H                         → halt target, report DHCSR
 *     G                         → resume target, report DHCSR
 *     I                         → read DP IDCODE
 *     ?                         → banner
 */
#include <stdint.h>

/* ---------- PM32F407 peripheral map (STM32F407-compatible) ---------- */
#define RCC_AHB1ENR     (*(volatile uint32_t *)0x40023830)
#define RCC_APB2ENR     (*(volatile uint32_t *)0x40023844)
#define GPIOA_MODER     (*(volatile uint32_t *)0x40020000)
#define GPIOA_AFRH      (*(volatile uint32_t *)0x40020024)
#define GPIOC_MODER     (*(volatile uint32_t *)0x40020800)
#define GPIOC_IDR       (*(volatile uint32_t *)0x40020810)
#define GPIOC_BSRR      (*(volatile uint32_t *)0x40020818)

#define USART1_BASE     0x40011000u
#define USART1_SR       (*(volatile uint32_t *)(USART1_BASE + 0x00))
#define USART1_DR       (*(volatile uint32_t *)(USART1_BASE + 0x04))
#define USART1_BRR      (*(volatile uint32_t *)(USART1_BASE + 0x08))
#define USART1_CR1      (*(volatile uint32_t *)(USART1_BASE + 0x0C))

#define USART_SR_RXNE   (1u << 5)
#define USART_SR_TXE    (1u << 7)
#define USART_CR1_UE    (1u << 13)
#define USART_CR1_TE    (1u << 3)
#define USART_CR1_RE    (1u << 2)

#define GPIOA_EN        (1u << 0)
#define GPIOC_EN        (1u << 2)
#define USART1EN        (1u << 4)

#define SWDIO_PIN       0
#define SWCLK_PIN       2
#define SWDIO_MASK      (1u << SWDIO_PIN)
#define SWCLK_MASK      (1u << SWCLK_PIN)

/* ---------- SWD bit-bang (unchanged from flasher) ---------- */

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
static inline void swd_delay(void) {
    __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop;");
}
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

static void swd_line_reset(void) {
    swdio_output(); swdio_high();
    for (int i = 0; i < 60; i++) swd_clk();
    swdio_low();
    for (int i = 0; i < 4; i++) swd_clk();
}
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
static uint32_t swd_read_txn_once(int apndp, int addr23, uint32_t *data_out) {
    uint8_t req = swd_request(apndp, 1, addr23);
    swdio_output();
    swd_write_bits_lsb(req, 8);
    swdio_input();
    uint32_t ack = swd_read_bits_lsb(3);
    uint32_t data = swd_read_bits_lsb(32);
    (void)swd_read_bit();
    swdio_output(); swdio_low(); swd_clk();
    if (ack == 1) *data_out = data;
    return ack;
}
static uint32_t swd_read_txn(int apndp, int addr23, uint32_t *data_out) {
    for (int i = 0; i < 8; i++) {
        uint32_t ack = swd_read_txn_once(apndp, addr23, data_out);
        if (ack != 2) return ack;  /* not WAIT → return OK/FAULT immediately */
        swdio_output(); swdio_low();
        for (int j = 0; j < 4; j++) swd_clk();
    }
    return 2;
}
static uint32_t swd_write_txn_raw(int apndp, int addr23, uint32_t wire_data, int parity_bit, int real_parity) {
    uint8_t req = swd_request(apndp, 0, addr23);
    swdio_output();
    swd_write_bits_lsb(req, 8);
    swdio_input();
    uint32_t ack = swd_read_bits_lsb(3);
    swd_clk();
    swdio_output();
    swd_write_bits_lsb(wire_data, 32);
    swd_write_bit(parity_bit);
    swd_write_bit(real_parity);
    return ack;
}
static uint32_t swd_write_txn(int apndp, int addr23, uint32_t data) {
    uint32_t wire_data = (data & 0x7FFFFFFF) << 1;
    int parity_bit = (data >> 31) & 1;
    int real_parity = parity32(data);
    for (int i = 0; i < 8; i++) {
        uint32_t ack = swd_write_txn_raw(apndp, addr23, wire_data, parity_bit, real_parity);
        if (ack != 2) return ack;
        swdio_output(); swdio_low();
        for (int j = 0; j < 4; j++) swd_clk();
    }
    return 2;
}
static void swd_idle(int n) {
    swdio_output(); swdio_low();
    for (int i = 0; i < n; i++) swd_clk();
}

/* AP memory read: TAR → DRW (posts) → DP RDBUFF (returns value). */
static uint32_t swd_ap_mem_read(uint32_t addr, uint32_t *ack_tar, uint32_t *ack_drw, uint32_t *ack_rdbuff) {
    uint32_t data = 0;
    *ack_tar = swd_write_txn(1, 0b01, addr);
    swd_idle(8);
    *ack_drw = swd_read_txn(1, 0b11, &data);
    swd_idle(8);
    *ack_rdbuff = swd_read_txn(0, 0b11, &data);
    swd_idle(8);
    return data;
}

/* ---------- USART1 driver (115200 8N1) ---------- */

static void uart_init(void) {
    RCC_AHB1ENR |= GPIOA_EN;
    RCC_APB2ENR |= USART1EN;
    /* PA9, PA10 → AF mode */
    uint32_t m = GPIOA_MODER;
    m &= ~((3u << (9*2)) | (3u << (10*2)));
    m |=   (2u << (9*2)) | (2u << (10*2));
    GPIOA_MODER = m;
    /* AFRH: PA9=AF7 (USART1_TX), PA10=AF7 (USART1_RX) */
    uint32_t a = GPIOA_AFRH;
    a &= ~((0xFu << ((9-8)*4)) | (0xFu << ((10-8)*4)));
    a |=   (7u   << ((9-8)*4)) | (7u   << ((10-8)*4));
    GPIOA_AFRH = a;
    /* Baud: APB2=16 MHz, OVER8=0 → USARTDIV = 16e6 / (16*115200) = 8.6805
     *   DIV_Mantissa=8, DIV_Fraction=round(0.6805*16)=11 → BRR = (8<<4)|11 = 0x8B */
    USART1_BRR = 0x008Bu;
    USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void uart_putc(char c) {
    while (!(USART1_SR & USART_SR_TXE)) { }
    USART1_DR = (uint32_t)(unsigned char)c;
}
static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}
static void uart_print_hex8(uint32_t v) {
    for (int i = 7; i >= 0; i--) {
        int n = (int)((v >> (i*4)) & 0xFu);
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}
static int uart_getc(void) {
    while (!(USART1_SR & USART_SR_RXNE)) { }
    return (int)(USART1_DR & 0xFFu);
}
static int uart_readline(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        int c = uart_getc();
        uart_putc((char)c);  /* echo */
        if (c == '\r') continue;
        if (c == '\n') { uart_putc('\n'); break; }
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

/* ---------- hex parsing ---------- */

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static uint32_t parse_hex(const char **pp) {
    const char *s = skip_ws(*pp);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int any = 0;
    while (1) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint32_t)d;
        any = 1;
        s++;
    }
    *pp = s;
    (void)any;
    return v;
}

/* ---------- SWD session bring-up ---------- */

static void swd_bringup(void) {
    /* Enable GPIOC + set PC0/PC2 as outputs */
    RCC_AHB1ENR |= GPIOC_EN;
    uint32_t m = GPIOC_MODER;
    m &= ~((3u << (SWDIO_PIN*2)) | (3u << (SWCLK_PIN*2)));
    m |=   (1u << (SWDIO_PIN*2)) | (1u << (SWCLK_PIN*2));
    GPIOC_MODER = m;

    /* SWD init — IDCODE, abort, select, power up CTRL/STAT, AP CSW=word. */
    swd_line_reset();
    swd_jtag_to_swd();
    swd_line_reset();
    uint32_t data;
    (void)swd_read_txn(0, 0b00, &data);           /* IDCODE */
    swd_idle(16);
    (void)swd_write_txn(0, 0b00, 0x0000001Eu);    /* ABORT */
    swd_idle(16);
    (void)swd_write_txn(0, 0b10, 0x00000000u);    /* SELECT bank 0 */
    swd_idle(16);
    (void)swd_write_txn(0, 0b01, 0x50000000u);    /* CTRL/STAT power up */
    swd_idle(16);
    (void)swd_read_txn(0, 0b01, &data);           /* readback */
    swd_idle(16);
    (void)swd_write_txn(1, 0b00, 0x03000042u);    /* AP CSW = word, AddrInc off */
    swd_idle(16);
}

/* ---------- main command loop ---------- */

int main(void) {
    uart_init();
    uart_puts("PROBE-BOOT\r\n");
    swd_bringup();

    /* Quick sanity: report IDCODE at boot */
    uint32_t idcode = 0;
    (void)swd_read_txn(0, 0b00, &idcode);
    swd_idle(16);
    uart_puts("READY idcode=");
    uart_print_hex8(idcode);
    uart_puts("\r\n");

    char line[48];
    uint32_t at, ad, ar;
    while (1) {
        uart_readline(line, (int)sizeof(line));
        char cmd = line[0];
        const char *rest = &line[1];

        if (cmd == 'R' || cmd == 'r') {
            uint32_t addr = parse_hex(&rest);
            uint32_t val = swd_ap_mem_read(addr, &at, &ad, &ar);
            uart_puts("R ");
            uart_print_hex8(addr);
            uart_putc(' ');
            uart_print_hex8(val);
            uart_puts(" ack=");
            uart_putc((char)('0' + (at & 7)));
            uart_putc((char)('0' + (ad & 7)));
            uart_putc((char)('0' + (ar & 7)));
            uart_puts("\r\n");
        } else if (cmd == 'W' || cmd == 'w') {
            uint32_t addr = parse_hex(&rest);
            uint32_t val = parse_hex(&rest);
            uint32_t tack = swd_write_txn(1, 0b01, addr);
            swd_idle(8);
            uint32_t dack = swd_write_txn(1, 0b11, val);
            swd_idle(8);
            uart_puts("W ");
            uart_print_hex8(addr);
            uart_putc(' ');
            uart_print_hex8(val);
            uart_puts(" ack=");
            uart_putc((char)('0' + (tack & 7)));
            uart_putc((char)('0' + (dack & 7)));
            uart_puts("\r\n");
        } else if (cmd == 'H' || cmd == 'h') {
            (void)swd_write_txn(1, 0b01, 0xE000EDF0u);
            swd_idle(8);
            (void)swd_write_txn(1, 0b11, 0xA05F0003u);  /* DBGKEY | C_HALT | C_DEBUGEN */
            swd_idle(16);
            uint32_t d = swd_ap_mem_read(0xE000EDF0u, &at, &ad, &ar);
            uart_puts("H dhcsr=");
            uart_print_hex8(d);
            uart_puts("\r\n");
        } else if (cmd == 'G' || cmd == 'g') {
            (void)swd_write_txn(1, 0b01, 0xE000EDF0u);
            swd_idle(8);
            (void)swd_write_txn(1, 0b11, 0xA05F0001u);  /* DBGKEY | C_DEBUGEN (clear C_HALT) */
            swd_idle(16);
            uint32_t d = swd_ap_mem_read(0xE000EDF0u, &at, &ad, &ar);
            uart_puts("G dhcsr=");
            uart_print_hex8(d);
            uart_puts("\r\n");
        } else if (cmd == 'I' || cmd == 'i') {
            uint32_t v = 0;
            uint32_t a = swd_read_txn(0, 0b00, &v);
            swd_idle(16);
            uart_puts("I idcode=");
            uart_print_hex8(v);
            uart_puts(" ack=");
            uart_putc((char)('0' + (a & 7)));
            uart_puts("\r\n");
        } else if (cmd == 'S' || cmd == 's') {
            /* Open-loop 6-step commutation via SWD. Caller must first:
             *   - Halt target (H) so vendor FSM doesn't fight us
             *   - Ensure TPOE=1 on target ATU (manual write to TPPS)
             * We inject into the already-configured ATU (period, dead-time,
             * output mode set by vendor demo's init code). We just rewrite
             * CR0A/B, CR1A/B, CR2A/B each step to commutate phases.
             *   S <hex_revs> <hex_step_delay_nops>
             * Typical safe starter: S 64 30D40  (100 revs, ~60ms/step). */
            uint32_t revs = parse_hex(&rest);
            uint32_t delay_nops = parse_hex(&rest);
            if (revs == 0) revs = 100;
            if (delay_nops == 0) delay_nops = 200000;

            /* REVERSED 6-step table — vendor chip U/V/W label may not match
             * B1044 motor cable U/V/W; reverse order swaps rotation direction.
             * Moderate differential (70/30) for thermal safety. */
            static const uint32_t step_table[6][3] = {
                { 750,  450, 1050},
                { 450,  750, 1050},
                { 450, 1050,  750},
                { 750, 1050,  450},
                {1050,  750,  450},
                {1050,  450,  750},
            };
            uart_puts("S start\r\n");
            for (uint32_t r = 0; r < revs; r++) {
                for (int st = 0; st < 6; st++) {
                    uint32_t u = step_table[st][0];
                    uint32_t v = step_table[st][1];
                    uint32_t w = step_table[st][2];
                    /* Complementary: write BOTH A and B with same value.
                     * Dead-time handled by ATU's DBTA/DBTB (set by vendor). */
                    swd_write_txn(1, 0b01, 0x40004408u); swd_idle(2);  /* CR0A */
                    swd_write_txn(1, 0b11, u);           swd_idle(2);
                    swd_write_txn(1, 0b01, 0x40004420u); swd_idle(2);  /* CR0B */
                    swd_write_txn(1, 0b11, u);           swd_idle(2);
                    swd_write_txn(1, 0b01, 0x4000440Cu); swd_idle(2);  /* CR1A */
                    swd_write_txn(1, 0b11, v);           swd_idle(2);
                    swd_write_txn(1, 0b01, 0x40004424u); swd_idle(2);  /* CR1B */
                    swd_write_txn(1, 0b11, v);           swd_idle(2);
                    swd_write_txn(1, 0b01, 0x40004410u); swd_idle(2);  /* CR2A */
                    swd_write_txn(1, 0b11, w);           swd_idle(2);
                    swd_write_txn(1, 0b01, 0x40004428u); swd_idle(2);  /* CR2B */
                    swd_write_txn(1, 0b11, w);           swd_idle(2);
                    for (volatile uint32_t d = delay_nops; d; d--) {
                        __asm__ volatile("nop");
                    }
                }
            }
            /* Neutral (50/50) */
            for (int ch = 0; ch < 3; ch++) {
                swd_write_txn(1, 0b01, 0x40004408u + ch*4); swd_idle(2);
                swd_write_txn(1, 0b11, 750);                swd_idle(2);
                swd_write_txn(1, 0b01, 0x40004420u + ch*4); swd_idle(2);
                swd_write_txn(1, 0b11, 750);                swd_idle(2);
            }
            uart_puts("S done\r\n");
        } else if (cmd == 'E' || cmd == 'e') {
            /* Enable TPOE (PWM output) via TRWPT-unlocked TPPS write. */
            swd_write_txn(1, 0b01, 0x40004494u); swd_idle(2);
            swd_write_txn(1, 0b11, 0x0000A5A5u); swd_idle(2);
            uint32_t t = swd_ap_mem_read(0x40004480u, &at, &ad, &ar);
            swd_write_txn(1, 0b01, 0x40004480u); swd_idle(2);
            swd_write_txn(1, 0b11, t | (1u << 14)); swd_idle(2);
            swd_write_txn(1, 0b01, 0x40004494u); swd_idle(2);
            swd_write_txn(1, 0b11, 0x0000000Au); swd_idle(2);
            uint32_t r = swd_ap_mem_read(0x40004480u, &at, &ad, &ar);
            uart_puts("E tpps=");
            uart_print_hex8(r);
            uart_puts("\r\n");
        } else if (cmd == '?' || cmd == 0 || cmd == ' ') {
            uart_puts("probe v2 | R|W|H|G|I | E enable PWM | S revs delay_nops\r\n");
        } else {
            uart_puts("? unknown cmd '");
            uart_putc(cmd);
            uart_puts("'\r\n");
        }
    }
}
