# HK32F030 / PM30225V SWD quirks we discovered

These are the three things that took the longest to figure out while bringing up the bit-bang flasher. None of them are in the HK32 or STM32F030 reference manuals; all were discovered empirically by running a Python-driven J-Link GPIO-poke prototype first (`scripts/swd_bitbang_via_jlink.py` in the dev scratchpad), then confirmed on the native C firmware.

## 1. Writes are shifted by one bit

What looks like a stock ADIv5 write transaction — 8 request + 3 ACK + 1 TRN + 32 DATA + 1 PARITY — actually places the host's transmitted data one bit ahead of what the target's internal shift register expects. Concretely:

```
target.DATA_register[k] = wire_bit[k + 1]   for k ∈ [0, 30]
target.DATA_register[31] = parity_bit_slot  (what the host sent where the spec says "parity")
```

The host's bit 0 is consumed as some kind of implicit extra turnaround and discarded.

**Empirical proof:** sending `0x28000000` (bits 29, 27 set) to CTRL/STAT resulted in `0x14000000` (bits 28, 26) landing in the target register — one bit shift to the right in MSB-first display = one-bit skip in LSB-first transmission.

**How we handle it:**

```c
uint32_t wire_data  = (data & 0x7FFFFFFF) << 1;  // bit 31 dropped to avoid overflow
int      parity_bit = (data >> 31) & 1;          // bit 31 goes into the "parity" slot
```

This hits all 32 bits correctly, including bit 31.

## 2. Parity bit must be transmitted on its own clock

The target does a parity check on the 32 bits it received (`target.DATA`), comparing against whatever is on SWDIO one clock after the DATA phase ends. If we don't drive that clock, SWDIO is floating / pulled low by our next idle cycle, and the check fails for any data value with odd parity.

Symptom: transactions with odd-parity data (e.g. CSW=`0x00000002`, which has one bit set) pass the ACK check (ACK=OK) but silently set `WDATAERR` in CTRL/STAT. Every subsequent AP access then returns FAULT and the entire debug session collapses.

**Fix:** after transmitting 32 DATA bits + the `parity_bit` slot (which lands in `target.DATA[31]`), transmit one more clock driven to `parity32(data)` — the real parity value the target is looking for.

```c
swd_write_bits_lsb(wire_data, 32);
swd_write_bit(parity_bit);       // target.DATA[31]
swd_write_bit(parity32(data));   // <- the clock we were missing
```

This is the single most expensive bug of the bringup.

## 3. Flash programming requires halfword AP access

The AP CSW register's `Size` field default for AHB-AP is word (32-bit). That works fine for SRAM or peripheral registers. But STM32F0 / HK32F030 Flash controller requires 16-bit half-word writes — a 32-bit write silently becomes a no-op (BSY clears, EOP fires, SR looks clean, and the Flash stays at `0xFFFFFFFF`).

**Fix:** around the programming phase, switch CSW:

```c
swd_write_ap(0x0, 0x00000001);  // CSW.Size = 001 (halfword)
// ... program all halfwords ...
swd_write_ap(0x0, 0x00000002);  // restore to word for readback
```

## Things that were red herrings

- **"HK32 has inverted ACK (OK=0x4 instead of 0x1)"** — the PM10225 user manual §22.4.2 suggested this, and one of our early prototypes saw `ACK=0x4` consistently. It was actually standard `OK=0x1` being misread due to the one-bit shift above. Once we had correct clocking, ACK values matched the ARM spec exactly.
- **"Maybe the chip is RDP1-locked"** — when memory reads were returning `0xFFFFFFFF` we assumed vendor had set read protection. It was the odd-parity cascade above. Actual RDP status reads cleanly as RDP0 once parity is fixed.

## Empirical clock timing

- SWD clock period on PM32F407 at 16 MHz HSI with `swd_delay()` = 8 NOPs: ~1 μs ≈ 1 MHz.
- Target accepts anything up to tens of MHz (spec-wise ≤25 MHz for this SW-DP); 1 MHz is comfortable.
- Running PM32F407 at PLL-boosted 168 MHz would give us ~10 MHz SWD if we wanted to speed up programming. Haven't needed it.
