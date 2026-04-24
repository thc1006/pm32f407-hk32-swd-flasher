# HK32F030 / PM30225V SWD quirks we discovered

These are the things that took the longest to figure out while bringing up the bit-bang flasher. None are in the HK32 or STM32F030 reference manuals; all were discovered empirically — the first three by running a Python-driven J-Link GPIO-poke prototype (`scripts/swd_bitbang_via_jlink.py` in the dev scratchpad), then confirmed on the native C firmware; the last two by reading PM30225V Flash back through the probe firmware after every "successful" flash showed garbage vectors.

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

**Fix:** around the programming phase, switch CSW size:

```c
swd_write_ap(0x0, 0x03000041);  // CSW.Size = 001 (halfword) | preserve context bits — see quirk #5
// ... program all halfwords ...
swd_write_ap(0x0, 0x03000042);  // restore to word for readback
```

## 4. Halfword writes need byte-lane shift in DRW

Once CSW is in halfword mode, AHB-AP picks the active byte lane in DRW from `addr[1:0]`. For an address with bit 1 set (`(addr & 2) == 2`, i.e. odd halfword position within a word), the payload must sit in `DRW[31:16]` — not `[15:0]`. Otherwise the target receives `0x0000` on the active lane and the high halfword of every word silently programs to zero.

Because the verify loop reads back through the same CSW configuration and sees the same lane-masked zero, `expected == got` trivially passes on the zeroed lane. Verify reports clean. The target then refuses to boot because its reset vector has been mangled.

**Empirical proof:** Q_Example flash readback after a "successful" verify:

```
0x08000000 stored: 0x00000af0   (expected 0x20000af0 — initial SP)
0x08000004 stored: 0x000000c9   (expected 0x080000c9 — Reset)
0x08000008 stored: 0x00001335   (expected 0x08001335 — NMI)
0x0800000c stored: 0x00000fb9   (expected 0x08000fb9 — HardFault)
```

Every word's high halfword is exactly zero. Low halfword preserved.

**Fix:**

```c
uint32_t drw_val = (addr & 2u) ? ((uint32_t)hw << 16) : (uint32_t)hw;
swd_write_ap_drw(drw_val);
```

Discovered while debugging "why doesn't Q_Example boot after a clean flash" — see PR #14 for the fix.

## 5. CSW writes must preserve MasterType / HPROT / DeviceEn

The AHB-AP `CSW.Size` field is the well-known knob, but bits 24 (MasterType), 25 (HPROT master-like), and 6 (DeviceEn) gate whether the AP actually issues the memory transaction downstream. Some implementations (notably HK32F030's) silently drop transfers when these bits are clear: CSW write returns OK ACK, subsequent DRW posts to the AP, target memory never gets the burst.

This compounded with quirk #4 to make it look like the flasher was working when it wasn't.

**Fix:** every CSW write preserves these bits:

```c
#define CSW_WORD       0x03000042   // Size=Word     | MasterType | HPROT | DeviceEn
#define CSW_HALFWORD   0x03000041   // Size=Halfword | MasterType | HPROT | DeviceEn
```

Discovered in the same session as quirk #4 (PR #14).

## Things that were red herrings

- **"HK32 has inverted ACK (OK=0x4 instead of 0x1)"** — the PM10225 user manual §22.4.2 suggested this, and one of our early prototypes saw `ACK=0x4` consistently. It was actually standard `OK=0x1` being misread due to the one-bit shift above. Once we had correct clocking, ACK values matched the ARM spec exactly.
- **"Maybe the chip is RDP1-locked"** — when memory reads were returning `0xFFFFFFFF` we assumed vendor had set read protection. It was the odd-parity cascade above. Actual RDP status reads cleanly as RDP0 once parity is fixed.
- **"The Q variant can't be flashed to a V chip because IU/IV CH_BUFFER are swapped"** — claim from CLAUDE.md that delayed motor success by a session. The IU/IV swap (PGA1_OUTA vs OUTB assignment) only flips rotation direction; it does NOT prevent the motor from working. The variants that genuinely don't match KXB EVB are V_Sample / MY-i / MY-g / Q-Sample-in-main-pack — all four route VBUS or VSP ADC to floating pins (PA6/PA8/PA9). The standalone `PM30225Q_0405B_2R_Example` folder is the only variant whose ADC channels match KXB physical wiring (VBUS=PB5/AIN20, VSP=PA11/AIN14).

## Empirical clock timing

- SWD clock period on PM32F407 at 16 MHz HSI with `swd_delay()` = 8 NOPs: ~1 μs ≈ 1 MHz.
- Target accepts anything up to tens of MHz (spec-wise ≤25 MHz for this SW-DP); 1 MHz is comfortable.
- Running PM32F407 at PLL-boosted 168 MHz would give us ~10 MHz SWD if we wanted to speed up programming. Haven't needed it.
