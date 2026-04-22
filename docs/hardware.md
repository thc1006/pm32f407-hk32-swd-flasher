# Hardware setup

## Bill of materials

| Item | Role | Notes |
|---|---|---|
| PM32F407 dev board | Hosts the bit-bang SWD master firmware | Panjit STM32F407 clone, LQFP144 at 168 MHz (we run at 16 MHz HSI for this flasher) |
| PM30225V-0401 (KXB EVB) | Flash target | HK32F030 silicon, Cortex-M0, 32 KB Flash |
| J-Link EDU Mini V2 | Flash PM32F407 (option A) | Optional — only for dev iteration |
| CH340C on PM32F407 EVB | Flash PM32F407 (option B) | Already populated on Panjit eval board |
| 6 × Dupont jumpers | Signal wiring | 4 × SWD + 2 × power |
| 5 V USB supply | Power PM32F407 + KXB | e.g. laptop USB, benchtop supply |

## Signal wiring

```
PM32F407 CN3 header           KXB EVB P3 header (SWD, 4-pin, 0.1")
─────────────────────         ──────────────────
Pin 15 (PC0)        ────────▶ Pin 1 (SWDIO)
Pin 29 (PC2)        ────────▶ Pin 2 (SWCLK)
GND (Pin 57/58)    ─────────▶ Pin 3 (GND)

PM32F407 J87 (power)          KXB EVB P1 header (power, 5-pin, 0.1")
────────────────              ──────────────────
Pin 2 (5V)         ─────────▶ Pin 5 (VIN) — silk top
Pin 7 (GND)        ─────────▶ Pin 4 (GND)
```

P1 pin numbering on KXB EVB is `VIN / GND / VDD / UART1_TX / UART1_RX` top-to-bottom. P3 pin numbering is `VDD / GND / SWCLK / SWDIO` top-to-bottom. We do NOT connect the `VDD` or UART pins for the flashing use case.

## J-Link to PM32F407 (option A, development)

J-Link EDU Mini has a 10-pin 0.05" Cortex-M debug connector. Pin 1 is next to the keying feature (a bump or notch on one long side of the shroud — not a triangle on the silk, which often isn't present).

```
J-Link 10-pin                  PM32F407 J2 (5-pin SIP SWD)
─────────────                  ──────────────────────────
Pin 1 (VTref)    ───────────▶  Pin 5 (3V3)
Pin 2 (SWDIO)    ───────────▶  Pin 4 (SWDIO = PA13)
Pin 3 (GND)      ───────────▶  Pin 3 (GND)
Pin 4 (SWCLK)    ───────────▶  Pin 2 (SWCLK = PA14)
```

J2 is a standard 5-pin 0.1" SIP so Dupont jumpers work directly. The same underlying PA13/PA14 are also on J45 (standard ARM 20-pin 0.1" JTAG), so either works.

## CH340C to PM32F407 (option B, deployment)

Just plug USB into the PM32F407 EVB. The onboard CH340C is wired via `J80`/`J81` jumpers to USART1 (PA9/PA10), and DTR/RTS drive the auto-ISP circuit (`Q3 SS8050` + `Q5 SS8550`) that toggles BOOT0 + RESET. `stm32flash -f F4` drives the whole sequence automatically.

Confirm `J80` and `J81` jumpers are populated. If absent, PA9/PA10 are disconnected from the USB-UART path.

## Power considerations

The PM32F407's onboard 5 V regulator provides up to ~500 mA — plenty for the KXB (the PM30225V chip itself draws a few mA, and its MOSFET stage is only powered through `P1 VIN`, which is separate). Sharing GND via one jumper is sufficient.

Do NOT connect J-Link's own 5 V/Vsupply rail (pin 2 on 10-pin, or pin 19 on 20-pin) to the target — J-Link EDU Mini doesn't provide target power and the pin is a sense-only input.
