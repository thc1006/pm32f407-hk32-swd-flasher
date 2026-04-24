"""Reset PM32F407 EVB to run-from-Flash via CH340C DTR/RTS lines.

Solves a recurring confusion: the probe firmware initialises its SWD master
state once at boot (line reset, IDCODE, DP power-up). If the *target*
(PM30225V) gets power-cycled in the middle of a session — e.g. you swapped
the 12V adapter — the master never re-syncs, and every subsequent
`Probe.idcode()` returns 0x00000000 even though both chips are healthy.
Resetting the master via the EVB's auto-ISP DTR/RTS lines forces the probe
firmware to re-run its init sequence and the SWD link comes back.

Why this works without J-Link: PM32F407 EVB wires CH340C's DTR to BOOT0
(through an inverter) and RTS to NRST (through an inverter). For a plain
run-from-Flash reset we leave DTR low (BOOT0=0) and pulse RTS to assert
then release NRST. This is the well-known "ESP-style" auto-reset sequence
already proven on this board (see memory: pm32f407_autoisp_polarity.md).

Usage as library:
    from reset_pm32f407 import reset_pm32f407
    reset_pm32f407("COM4")              # quick reset, defaults are sane

Usage as CLI:
    python tools/reset_pm32f407.py --port COM4
    python tools/reset_pm32f407.py --port COM4 --verify     # also IDCODE-check after
"""
from __future__ import annotations

import argparse
import sys
import time

import serial


def reset_pm32f407(port: str, baud: int = 115200,
                   nrst_pulse_s: float = 0.05,
                   boot_s: float = 0.5) -> None:
    """ESP-style run-from-Flash reset.

    Parameters
    ----------
    port : str          serial port (e.g. "COM4" on Windows, "/dev/ttyUSB0" on Linux)
    baud : int          baud rate, only matters once probe firmware starts; default 115200
    nrst_pulse_s : float how long NRST stays asserted (default 50 ms — well above
                         STM32F4 minimum 20 µs, gives capacitor + inverter time to settle)
    boot_s : float       how long to wait after NRST release for probe firmware to
                         finish init (default 500 ms; SWD line reset + IDCODE +
                         CTRL/STAT power-up + initial CSW write fits comfortably).
    """
    s = serial.Serial(port, baud, timeout=0.5)
    try:
        # DTR LOW -> BOOT0 LOW (after CH340C wiring inverter) = run from Flash
        s.setDTR(False)
        # RTS HIGH -> NRST asserted (after inverter)
        s.setRTS(True)
        time.sleep(nrst_pulse_s)
        # RTS LOW -> NRST released, CPU starts running probe firmware
        s.setRTS(False)
        time.sleep(boot_s)
        s.reset_input_buffer()
    finally:
        s.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", default=115200, type=int)
    ap.add_argument("--verify", action="store_true",
                    help="after reset, open probe and read PM30225V IDCODE to confirm SWD healthy")
    args = ap.parse_args()

    reset_pm32f407(args.port, args.baud)
    print(f"reset issued on {args.port} (DTR=0, RTS pulse high then low)")

    if args.verify:
        sys.path.insert(0, "tools")
        from probe import Probe
        p = Probe(args.port, args.baud)
        idcode = p.idcode()
        ok = idcode == 0x0BB11477
        print(f"IDCODE = {idcode:#010x}  {'OK (PM30225V healthy)' if ok else 'FAIL (check 12V on KXB + SWD wires)'}")
        return 0 if ok else 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
