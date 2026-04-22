# Known Issues

The initial drop of this repo (commit that imports `main.c` straight out of the dev scratchpad) has several problems identified in post-hoc code review. Each is tracked as a dedicated follow-up PR so the fixes can be reviewed and merged independently.

## Critical

### C1. Target CPU is not halted before Flash operations

The PM30225V's own Cortex-M0 keeps executing its pre-burned vendor firmware while we erase/program its Flash. If the CPU happens to fetch from a page being erased, it will hardfault and may scramble the SWD state — in the worst case, the chip becomes unresponsive until power-cycled. The empirical test on 2026-04-22 worked, but it was luck.

**Fix:** before the first FLASH_KEYR write, issue a memory write to DHCSR (`0xE000EDF0`) with value `0xA05F0003` (DBGKEY | C_HALT | C_DEBUGEN). Tracked in PR: `fix/halt-target-before-flash`.

### C2. `g_result[]` indices are written twice

`g_result[13]` gets assigned first to `flash_cr` (pre-unlock LOCK check) and later overwritten by `sr_erase` (post-erase status). Same pattern for `g_result[14]` (`flash_cr` post-unlock is clobbered by the final Flash read-back). The only reason everything "looks right" in the end is that the last write happens to land on the value we care about.

**Fix:** give each captured value its own slot (or expand the array). Tracked in PR: `fix/gresult-index-collision`.

### C3. AP memory read uses "double DRW" instead of DP RDBUFF

The helper does two consecutive AP DRW reads to grab a value. Each AP read posts another access; with the current CSW (AddrInc off) that's equivalent, but it's brittle. The ADIv5 canonical pattern is: one posted AP DRW read, then read DP RDBUFF to retrieve the result without triggering a new access.

**Fix:** switch `swd_ap_mem_read` to post + RDBUFF. Tracked in PR: `fix/use-rdbuff-for-ap-reads`.

## High priority

### H1. No retry on WAIT ACK

The spec says WAIT means "target busy, retry the same request". We currently treat WAIT the same as FAULT and bail. For a slow FPEC this will occasionally drop legitimate transactions.

**Fix:** wrap transactions in a retry loop that honours WAIT. PR: `fix/wait-ack-retry`.

### H2. STICKYERR is never cleared between operations

Once STICKYERR is set (e.g. from a single bad write), every subsequent AP transaction returns FAULT. We don't have a recovery path — the user has to power-cycle.

**Fix:** on FAULT, issue DP ABORT with STKERRCLR + STKCMPCLR + WDERRCLR + ORUNERRCLR, then retry. PR: `fix/sticky-err-clear`.

### H3. FLASH_SR error bits aren't inspected

After poll-for-BSY=0 we move on without checking PGERR (bit 2) / WRPERR (bit 4). If the target has write-protected sectors we'll report success falsely.

**Fix:** after BSY clears, check SR for errors and return a fail code. PR: `fix/sr-error-check`.

### H4. No target reset after programming

PM30225V's CPU continues running old cached code after Flash is updated — the new firmware doesn't take effect until reset. We should issue a soft-reset via AIRCR (`0xE000ED0C = 0x05FA0004`) at the end of the sequence.

**Fix:** add target reset as final step. PR: `fix/target-reset-after-program`.

### H5. Only a single halfword is programmed (not a real "programmer")

The demo writes `0x1234` at `0x08000000` and stops. A usable flasher embeds the target binary as a `const uint8_t[]` and loops halfword-programming across it with erase-first-as-needed page boundaries.

**Fix:** embed binary + loop + multi-page erase + final verify. PR: `feat/flash-loop-binary-embed`.

## Medium

### M1. FLASH_SR.EOP is never explicitly cleared

EOP is sticky — per the STM32F0 reference manual you clear it by writing 1 to SR bit 5 before starting the next operation. We rely on polling BSY only, so EOP stays set and could mask a later genuine EOP check.

### M2. No verify-after-erase

We trust the erase STRT without reading the page back to confirm it's all `0xFF`.

### M3. `swd_delay()` is hardcoded to 8 NOPs at 16 MHz HSI

Changing optimisation level or system clock silently changes SWD timing. Should be a configurable constant or derived from CPU clock.

### M4. Magic addresses throughout `main.c`

Should move all the `0x40022010` style literals into `firmware/target.h`. Partly done — only `gpio.h`/`target.h` are split out; the rest is inline.

## Style

`main.c` mixes `/* */` and `//` comments; needs a final refactor pass to split SWD protocol code out of the orchestration into `firmware/swd.{c,h}` and Flash operations into `firmware/flash_prog.{c,h}`. PR: `refactor/module-split`.
