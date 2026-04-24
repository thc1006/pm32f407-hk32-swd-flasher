"""Halted-target direct ATU drive — open-loop 6-step BLDC commutation.

Proven 2026-04-25 on PM30225V (HK32F030 silicon) + B1044 hollow-cup motor +
1:125 gearbox, running PM30225Q_0405B_2R_Example firmware flashed through
the SWD flasher. The vendor FSM deadlocks in ALIGN/STOP on KXB EVB (fights
ADC bias init + an open-loop startup incompatibility with B1044's low-R
winding), so we go around it: halt the target CPU, set the ATU's "keep
running PWM while halted" bit (TPHMS), enable master output (TPOE), and
drive the three CRxA/B duty registers directly from this script over SWD.

Prerequisites:
  1. PM32F407 running the probe firmware from feat/interactive-swd-probe
     (tools/probe.py client works against it).
  2. Target PM30225V freshly power-cycled with Q_Example flashed. If you just
     flashed via the programmer firmware, a soft reset via probe
         p.w(0xE000ED0C, 0x05FA0004)
     gets Q_Example running.
  3. Motor wired through KXB P1 connector, 9–12V VIN.
  4. KXB P4 Pin 5 (PA11 = VSP ADC on Q_Example) pulled to VDD or a pot —
     otherwise the vendor FSM never leaves STANDBY. (Not strictly required
     for this script because we halt before the FSM touches anything, but
     power-up FSM still runs briefly and tidier if VSP is meaningful.)

Safety:
  - Halted CPU means the vendor's ALM_OCP / fault-retry logic is NOT running.
    If your motor mechanically stalls, current goes up to whatever the wiring
    allows; the PM30225V chip's own hardware OCP (~7 A per datasheet) is the
    last line of defence. Keep runs short.
  - The final cleanup always neutralises duties (50% on all three phases) and
    clears TPOE. If the script is killed mid-run, re-run the --stop invocation
    or power-cycle the target.
  - Arduino Uno VIN pass-through is rated ~1 A by its M7 diode. Above that the
    diode heats fast; prefer a direct VIN connection to KXB for sustained use.

Usage examples:
    python tools/demo_direct_drive.py --port COM4           # defaults: fwd 9 s @ 85/15 / 30 ms
    python tools/demo_direct_drive.py --reverse --seconds 18
    python tools/demo_direct_drive.py --duty 95 --step-ms 40 --seconds 24
    python tools/demo_direct_drive.py --stop                # neutralise + TPOE off only
    python tools/demo_direct_drive.py --reset-mcu           # reset PM32F407 before driving;
                                                            # use this if IDCODE reads 0 because
                                                            # the target was power-cycled mid-session
"""
from __future__ import annotations

import argparse
import sys
import time

sys.path.insert(0, "tools")
from probe import Probe  # noqa: E402
from reset_pm32f407 import reset_pm32f407  # noqa: E402

# PM30225V ATU (MMIO). Offsets match pm30225q_0405b.h ATU_TypeDef — same
# layout as the V and MY-i variants we audited.
ATU_BASE = 0x40004400
TPR   = ATU_BASE + 0x00   # PWM period (already set by vendor init to 1500)
CR0A  = ATU_BASE + 0x08   # phase-U high-side duty
CR1A  = ATU_BASE + 0x0C   # phase-V high-side duty
CR2A  = ATU_BASE + 0x10   # phase-W high-side duty
CR0B  = ATU_BASE + 0x20   # phase-U low-side duty (complementary)
CR1B  = ATU_BASE + 0x24   # phase-V low-side duty
CR2B  = ATU_BASE + 0x28   # phase-W low-side duty
TPPS  = ATU_BASE + 0x80   # Timer Protect / Pin Status — bit 12 TPHMS, bit 14 TPOE
TRWPT = ATU_BASE + 0x94   # Timer Register Write Protect — unlock 0xA5A5, lock 0xA


def enable_pwm_while_halted(p: Probe) -> int:
    """Set TPPS.TPHMS and TPPS.TPOE through TRWPT unlock. Returns TPPS readback."""
    p.halt()
    p.w(TRWPT, 0xA5A5)
    current = p.r(TPPS)
    p.w(TPPS, current | (1 << 12) | (1 << 14))
    p.w(TRWPT, 0x000A)
    time.sleep(0.01)
    return p.r(TPPS)


def disable_pwm(p: Probe) -> None:
    """Neutralise all duties and clear TPOE. Motor stops coasting."""
    mid = (p.r(TPR) & 0xFFFF) // 2
    for r in (CR0A, CR0B, CR1A, CR1B, CR2A, CR2B):
        p.w(r, mid)
    p.w(TRWPT, 0xA5A5)
    p.w(TPPS, p.r(TPPS) & ~(1 << 14))
    p.w(TRWPT, 0x000A)


def six_step_table(tpr: int, duty_pct: int, reverse: bool):
    """Classic trapezoidal 6-step commutation table. duty_pct = high-side %."""
    hi = int(tpr * duty_pct / 100)
    lo = int(tpr * (100 - duty_pct) / 100)
    mid = tpr // 2
    steps = [
        (hi, lo, mid),
        (hi, mid, lo),
        (mid, hi, lo),
        (lo, hi, mid),
        (lo, mid, hi),
        (mid, lo, hi),
    ]
    if reverse:
        steps = list(reversed(steps))
    return steps


def run_commutation(p: Probe, steps, step_s: float, seconds: float) -> None:
    """Cycle through the 6-step table until seconds elapses."""
    deadline = time.monotonic() + seconds
    cycle = 0
    while time.monotonic() < deadline:
        for (u, v, w) in steps:
            if time.monotonic() >= deadline:
                break
            p.w(CR0A, u); p.w(CR0B, u)
            p.w(CR1A, v); p.w(CR1B, v)
            p.w(CR2A, w); p.w(CR2B, w)
            time.sleep(step_s)
        cycle += 1
        if cycle % 10 == 0:
            elapsed = seconds - (deadline - time.monotonic())
            print(f"  cycle {cycle:3d}  t={elapsed:5.1f}s")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", default=115200, type=int)
    ap.add_argument("--duty", default=85, type=int,
                    help="high-side duty %%. 85 = gentle, 95 = strong, 98 = max. (default 85)")
    ap.add_argument("--step-ms", default=30, type=int,
                    help="per-step dwell in ms. 30 = stable, 40 = more load torque. (default 30)")
    ap.add_argument("--seconds", default=9.0, type=float,
                    help="total drive duration in seconds (default 9)")
    ap.add_argument("--reverse", action="store_true",
                    help="reverse commutation order (flips rotation direction)")
    ap.add_argument("--stop", action="store_true",
                    help="neutralise duties + clear TPOE, then exit (safety reset)")
    ap.add_argument("--reset-mcu", action="store_true",
                    help="reset PM32F407 via DTR/RTS before connecting (use when "
                         "IDCODE reads 0 because the target was power-cycled mid-session)")
    args = ap.parse_args()

    if args.reset_mcu:
        reset_pm32f407(args.port, args.baud)
        print(f"PM32F407 reset on {args.port}")

    p = Probe(args.port, args.baud)
    idcode = p.idcode()
    if idcode != 0x0BB11477:
        print(f"WARNING: IDCODE = {idcode:#010x} (expected 0x0BB11477). "
              f"Run with --reset-mcu, or check 12V on KXB and SWD wires.")
        return 2
    print(f"IDCODE = {idcode:#010x}  OK")

    if args.stop:
        disable_pwm(p)
        print("stopped: TPOE=0, duties neutral")
        return 0

    try:
        tpps = enable_pwm_while_halted(p)
        if not (tpps & (1 << 14)):
            print(f"ERROR: TPOE failed to stick (TPPS={tpps:#x}). Is the target halted?")
            return 1
        tpr = p.r(TPR) & 0xFFFF
        print(f"TPPS={tpps:#x}  TPHMS=ON TPOE=ON  TPR={tpr}")

        steps = six_step_table(tpr, args.duty, args.reverse)
        direction = "REVERSE" if args.reverse else "FORWARD"
        print(f"\n-- {direction}  duty {args.duty}%/{100-args.duty}%  step {args.step_ms}ms  "
              f"duration {args.seconds:.1f}s --")
        for s in (3, 2, 1):
            print(f"  start in {s}..."); time.sleep(1)
        print("  GO\n")
        run_commutation(p, steps, args.step_ms / 1000.0, args.seconds)
    finally:
        disable_pwm(p)
    print("\nstopped cleanly. TPOE=0, duties neutral.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
