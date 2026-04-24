# Known Issues

Status as of 2026-04-25. The original tracker entries from the post-import code review (C1–H5) have all been resolved by merged PRs. Two newer critical bugs were found later (byte-lane silent corruption and CSW context bits) and are also fixed. Remaining items are Medium priority polish + a vendor-patching gotcha worth knowing about.

## ✅ Resolved (PRs merged on main)

| ID | Issue | Fix PR |
|---|---|---|
| **C1** | Target CPU not halted before Flash operations — risked SWD wedging if CPU fetched from a page being erased | [#1](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/1) `fix/halt-target-before-flash` |
| **C2** | `g_result[]` slots collide (13/14 written twice) | [#2](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/2) `fix/gresult-index-collision` |
| **C3** | AP memory read used double-DRW instead of canonical post + DP RDBUFF | [#3](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/3) `fix/use-rdbuff-for-ap-reads` |
| **H1** | No retry on WAIT ACK | [#4](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/4) `fix/wait-ack-retry` |
| **H2** | STICKYERR never cleared between operations | [#10](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/10) `fix/sticky-err-clear` |
| **H3** | FLASH_SR error bits not inspected | [#6](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/6) `fix/sr-error-check` |
| **H4** | No target reset after programming | [#7](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/7) `fix/target-reset-after-program` |
| **H5** | Only single halfword programmed (toy demo) | [#8](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/8) `feat/flash-loop-binary-embed` |
| **C4** ★ | **Halfword byte-lane bug**: every Flash word's high halfword silently written as 0; verify loop returned clean. Reset vector landed as `0x000000C9` instead of `0x080000C9` so target hard-faulted on boot. | [#14](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/14) `fix/flash-program-hw-byte-lane-csw` |
| **C5** ★ | **CSW context bits dropped**: writing CSW = `0x01`/`0x02` (size only) leaves MasterType/HPROT/DeviceEn unset; HK32F030's AHB-AP silently drops the resulting memory transfers. | [#14](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/14) (same PR — both bugs found together) |
| **L1** | Bug-tracking file used `0x400` as Flash page size for HK32F030 — actual silicon uses 0x200 (512 bytes). PGERR at byte 512 of any blob > 1 page was the smoking gun. | [#12](https://github.com/thc1006/pm32f407-hk32-swd-flasher/pull/12) `fix/target-hk32-page-size` |

C4 + C5 were the more severe of the lot: they made every Flash write *appear* to succeed (verify loop reads back the same lane-masked zeros and reports no mismatch), so without an external probe to read the target Flash directly, you could spend hours wondering why a freshly-flashed target won't boot.

## ⚠ Vendor-firmware patching gotcha (not a flasher bug, but bites flasher users)

### V1. `nOCPLevel` is `s16` — `0xFFFF` does not disable OCP

Vendor's `mS_Para.nOCPLevel` field is declared `int16_t`. Patching it to `0xFFFF` over SWD intending "max threshold = OCP disabled" sets it to **−1** after sign extension. The check `|nIa| > nOCPLevel` then reads as `|nIa| > −1`, which is always true → OCP fires every ISR tick → `eSYSPROTECT` latches to `E_OCP` and `TPOE` clears.

Use `0x7FFF` (s16 max = 32767) when disabling. Always grep the vendor source for the field's declared type before deciding the "infinity" value.

This is documented separately in the project memory as `swd_signed_s16_threshold_pitfall.md`.

## Medium (still open, no PR)

### M1. `FLASH_SR.EOP` is never explicitly cleared

EOP is sticky — per the STM32F0 reference manual you clear it by writing 1 to SR bit 5 before starting the next operation. We rely on polling BSY only, so EOP stays set and could mask a later genuine EOP check. Hasn't bitten in practice because every operation reads SR fresh and looks at PGERR/WRPERR/EOP at the same point; but a stricter implementation should clear it.

### M2. No verify-after-erase

We trust the erase STRT without reading the page back to confirm it's all `0xFF`. The verify loop after programming would catch a botched erase indirectly (programmed value doesn't match), but a dedicated erase-verify would localise the failure mode.

### M3. `swd_delay()` is hardcoded to 8 NOPs at 16 MHz HSI

Changing optimisation level or system clock silently changes SWD timing. Should be a configurable constant or derived from CPU clock. Working around it currently means "don't change the build flags".

### M4. Magic addresses still inline in `main.c`

Most of the SWD bit-bang and FPEC orchestration uses `0x40022010`-style literals inline. Partly migrated to `firmware/target.h` (FLASH controller + DHCSR + AIRCR are macroed) but ATU / GPIO / RCC remain inline. Refactor candidate.

## Style

`main.c` mixes `/* */` and `//` comments and is now ~700 lines mixing SWD protocol code and orchestration. Split candidates: `firmware/swd.{c,h}` for the protocol layer, `firmware/flash_prog.{c,h}` for the Flash controller routines. PR: `refactor/module-split` (not yet created).
