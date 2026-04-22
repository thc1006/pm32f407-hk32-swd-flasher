# pm32f407-hk32-swd-flasher

A tiny (~1.5 KB) bit-banged SWD master firmware that runs on a **PM32F407** (Panjit's STM32F407 clone) and programs Flash on a **PM30225V-0401 / HK32F030** target via 2 GPIO pins.

Why this exists: the PM30225V is a HK32F030 motor-driver IC with an STM32F030-clone debug interface that has a couple of undocumented SWD quirks. Commercial programmers (J-Link, ST-Link) handle these internally, but rolling your own bit-bang forces you to discover them the hard way. This repo captures the working implementation plus the debug journey.

## Status

Verified end-to-end on 2026-04-22:

- ARM ADIv5 SWD protocol (DP + AP transactions)
- Debug power-up via CTRL/STAT (`0xF0000040` readback)
- Arbitrary target memory read/write (SRAM roundtrip of `0xDEADBEEF`)
- Flash unlock, erase, program, verify (halfword `0x1234` at `0x08000000` → readback `0xFFFF1234`)

See `docs/known_issues.md` for rough edges you should fix before using this in anything serious.

## Hardware wiring

```
PC ──USB──┬──> J-Link (option A, dev-time) ──SWD──> PM32F407 (J2/J45)
          └──> CH340C on PM32F407 EVB       ──UART──> PM32F407 boot ROM  (option B, no J-Link)

PM32F407 PC0 (CN3 Pin 15) ──> KXB P3 Pin 1 (SWDIO)
PM32F407 PC2 (CN3 Pin 29) ──> KXB P3 Pin 2 (SWCLK)
PM32F407 GND              ──> KXB P3 Pin 3 (GND) + KXB P1 Pin 4
PM32F407 5V  (J87 Pin 2)  ──> KXB P1 Pin 5 (VIN)
```

## Build

Requires `arm-none-eabi-gcc` 14.2+. On Windows, install inside WSL (no sudo needed):

```bash
wsl -d Ubuntu
cd ~ && mkdir -p toolchain && cd toolchain
wget https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz
tar -xJf arm-gnu-toolchain-*.tar.xz
```

Then from the repo root:

```bash
python tools/build.py
# → firmware/build/fw.bin  (~1.5 KB)
```

## Flash PM32F407 (two paths)

### Path A — J-Link (development)

```bash
"C:/Keil_v5/ARM/Segger/JLink.exe" -device STM32F407ZG -if SWD -speed 1000 \
    -autoconnect 1 -CommandFile tools/jlink/deploy.jlink
```

The J-Link script flashes `fw.bin`, runs it, halts after 500 ms, and dumps the `g_result[16]` array at `0x20000000` — which is where the firmware records SWD ACKs and values read back from the PM30225V target.

### Path B — UART (no J-Link needed)

PM32F407 EVB's CH340C is wired to the chip's built-in ROM bootloader via auto-ISP DTR/RTS. `stm32flash` handles the BOOT0 toggle automatically:

```bash
stm32flash -f F4 -P COM3 -w firmware/build/fw.bin
```

After flashing, PM32F407 reboots and autonomously runs the SWD sequence against the target. Output visibility without J-Link requires a UART readout — see `docs/hardware.md` for wiring a second USB-UART to the target feedback path if you want live progress.

## Repo layout

```
firmware/         C source for PM32F407
  main.c          current monolithic implementation (will refactor into modules)
  gpio.h          SWDIO/SWCLK pin assignments (PC0 / PC2)
  target.h        PM30225V register map (FLASH controller, debug regs)
  startup.c       minimal Cortex-M4 reset handler
  linker.ld       memory map (Flash 1 MB, SRAM 128 KB)
tools/
  build.py        WSL-native arm-none-eabi cross-compile
  jlink/*.jlink   J-Link Commander scripts
docs/
  known_issues.md the things listed in the code review
  hardware.md     wiring detail + BOM
  swd_quirks.md   HK32F030 SWD quirks we had to work around
```

## License

MIT — see `LICENSE`.
