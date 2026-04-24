# pm32f407-hk32-swd-flasher

A tiny (~1.5 KB) bit-banged SWD master firmware that runs on a **PM32F407** (Panjit's STM32F407 clone) and programs Flash on a **PM30225V-0401 / HK32F030** target via 2 GPIO pins.

Why this exists: the PM30225V is a HK32F030 motor-driver IC with an STM32F030-clone debug interface that has a couple of undocumented SWD quirks. Commercial programmers (J-Link, ST-Link) handle these internally, but rolling your own bit-bang forces you to discover them the hard way. This repo captures the working implementation plus the debug journey.

## Status

Initial end-to-end verification on 2026-04-22:

- ARM ADIv5 SWD protocol (DP + AP transactions)
- Debug power-up via CTRL/STAT (`0xF0000040` readback)
- Arbitrary target memory read/write (SRAM roundtrip of `0xDEADBEEF`)
- Flash unlock, erase, program, verify (halfword `0x1234` at `0x08000000` → readback `0xFFFF1234`)

**2026-04-25 — path C motor rotation achieved**, after fixing a silent Flash corruption bug that had been masquerading as clean verify (PR #14 — halfword byte-lane + CSW context bits) and a page-size bug specific to HK32F030 (PR #12 — 0x200 not 0x400):

- Full vendor-demo blob (10.8 KB `PM30225Q_0405B_2R_Example`) flashed and boots cleanly
- Target CPU halted + `ATU.TPPS.TPHMS | TPOE` set via TRWPT unlock + 6-step CR*A/B commutation via SWD drives B1044 BLDC motor on KXB EVB both directions, no J-Link involvement end-to-end
- Interactive SWD probe firmware (PR #13) + Python host client (PR #13 / PR #15) used throughout for live SRAM inspection and issuing the halt + commutation commands

See `docs/known_issues.md` for remaining rough edges (mostly Medium-priority polish; all C1–H5 and byte-lane/CSW critical fixes are merged on main). See the **Motor rotation demo** section below for the USB-only reproduction recipe.

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

## Motor rotation demo (no J-Link required)

Once the flasher has loaded a working vendor demo into PM30225V you can drive
the motor directly over SWD without depending on the vendor's closed-source
FOC startup. Validated 2026-04-25 on PM30225V + B1044 + 1:125 gearbox using
`PM30225Q_0405B_2R_Example` (the KXB-matched variant — see `swd_quirks.md`
for why the other variants don't work).

### One-time setup

1. Flash the **probe firmware** to PM32F407. Either:
   - via J-Link: `cp firmware/probe_main.c firmware/main.c && python tools/build.py`
     then `JLink.exe -device STM32F407ZG -CommandFile tools/jlink/deploy.jlink`
   - or via USB ROM bootloader: `stm32loader -p COM4 -V firmware/build/fw.bin`
     (requires `pip install stm32loader`; the EVB's CH340C wires DTR→BOOT0
     and RTS→NRST, so ESP-style auto-ISP works)

2. Wire SWD between PM32F407 and KXB EVB:
   ```
   PM32F407 PC0 ─→ KXB P3 SWDIO
   PM32F407 PC2 ─→ KXB P3 SWCLK
   PM32F407 GND ─→ KXB P3 GND
   ```

3. Power KXB: 9–12 V to P1 Pin 5 (VIN). Pull P4 Pin 5 (PA11 = VSP ADC) to
   VDD or a pot — without a non-zero VSP the vendor FSM never leaves STANDBY,
   though our halt-mode drive bypasses this anyway.

### Drive the motor

```bash
python tools/demo_direct_drive.py --port COM4 --reset-mcu --seconds 18
# 18 s of stable forward rotation at 85/15 duty, 30 ms/step

python tools/demo_direct_drive.py --port COM4 --reset-mcu --reverse --seconds 18
# reverse direction

python tools/demo_direct_drive.py --port COM4 --duty 95 --step-ms 40 --seconds 24
# stronger torque (~+30%), useful for loaded tests like a tendon pull
```

`--reset-mcu` issues an ESP-style DTR/RTS reset on the PM32F407 EVB so the
probe firmware re-runs its SWD line-reset / IDCODE / DP power-up. Use it
whenever the target was power-cycled mid-session and `IDCODE` suddenly
reads `0x00000000`.

### Standalone reset utility

```bash
python tools/reset_pm32f407.py --port COM4 --verify
# resets PM32F407, then opens probe and reads IDCODE — should print 0x0BB11477
```

### What the demo actually does

`tools/demo_direct_drive.py` is open-loop trapezoidal 6-step BLDC commutation
driven entirely from this side of SWD:

```
halt(target)                            # Cortex-M0 C_HALT via DHCSR
TPPS |= TPHMS | TPOE                    # ATU keeps PWM alive while halted +
                                        # enable master output (TRWPT-protected)
loop 6 steps × N cycles:
    write CR0A/B, CR1A/B, CR2A/B        # phase duty registers, complementary
```

It deliberately bypasses the vendor's closed-source `MotorAlign` /
`MotorRamp` / FOC observer chain. That chain has a startup race on KXB +
B1044 (ALIGN ↔ STOP loop, signed-s16 OCP-threshold pitfall, ParameterInit
restoring patches every cycle) that couldn't be unwound without rebuilding
`PM30225_LIB_V1P4.lib`. Direct ATU drive runs around the whole problem:
the ATU peripheral was already initialised by the vendor's boot code, we
just take over the duty registers.

See PR #14 commit message for the byte-lane bug that had been silently
corrupting every Q_Example flash before this approach could work.

## License

MIT — see `LICENSE`.
