/* Minimal Cortex-M4 startup for PM32F407 */
#include <stdint.h>

extern uint32_t _sdata, _edata, _sidata, _sbss, _ebss, _estack;
extern int main(void);

void Default_Handler(void) { while (1) {} }

void Reset_Handler(void) {
    /* Copy .data from FLASH to SRAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;

    /* Zero .bss */
    dst = &_sbss;
    while (dst < &_ebss) *dst++ = 0;

    main();
    while (1) {}
}

/* Minimal vector table — first 16 entries. Unused handlers point to Default_Handler.
   For a simple programmer we don't need interrupts. */
__attribute__((section(".isr_vector"), used))
const uint32_t isr_vector[] = {
    (uint32_t)&_estack,            /* 0: Initial stack pointer */
    (uint32_t)Reset_Handler,        /* 1: Reset handler */
    (uint32_t)Default_Handler,      /* 2: NMI */
    (uint32_t)Default_Handler,      /* 3: HardFault */
    (uint32_t)Default_Handler,      /* 4: MemManage */
    (uint32_t)Default_Handler,      /* 5: BusFault */
    (uint32_t)Default_Handler,      /* 6: UsageFault */
    0, 0, 0, 0,                     /* 7-10: Reserved */
    (uint32_t)Default_Handler,      /* 11: SVCall */
    (uint32_t)Default_Handler,      /* 12: DebugMonitor */
    0,                              /* 13: Reserved */
    (uint32_t)Default_Handler,      /* 14: PendSV */
    (uint32_t)Default_Handler,      /* 15: SysTick */
};
