/* PM32F407 GPIO bit-bang pin definitions for SWD master.
 *
 * The flasher lives on PM32F407 (STM32F407-compatible Panjit MCU) and
 * drives SWD on two GPIO pins, wired to the target (KXB EVB P3 header).
 *
 * Pin choice rationale: PC0 and PC2 are both on CN3 (easy access via
 * standard 2.54mm Dupont), not shared with any onboard peripherals
 * (not ethernet, not I²S audio, not USB OTG), and Port C clock is
 * cheap to enable on its own.
 */
#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

/* Pin assignments — change here if you wire to different GPIOs */
#define SWDIO_PIN   0               /* PC0 -> KXB P3 Pin 1 (SWDIO) */
#define SWCLK_PIN   2               /* PC2 -> KXB P3 Pin 2 (SWCLK) */

#define SWDIO_MASK  (1u << SWDIO_PIN)
#define SWCLK_MASK  (1u << SWCLK_PIN)

/* PM32F407 peripheral register map (STM32F407-compatible) */
#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830)
#define GPIOC_MODER (*(volatile uint32_t *)0x40020800)
#define GPIOC_IDR   (*(volatile uint32_t *)0x40020810)
#define GPIOC_BSRR  (*(volatile uint32_t *)0x40020818)
#define GPIOC_EN    (1u << 2)       /* RCC_AHB1ENR.GPIOCEN */

#endif /* GPIO_H */
