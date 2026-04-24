"""Interactive SWD probe client.

Talks to the PM32F407 probe firmware over its onboard CH340C USB-UART
(typically COMx on Windows, /dev/ttyUSBx on Linux). Lets Python do
arbitrary target-side (PM30225V) memory reads/writes via SWD.

Examples:
    p = Probe('COM3')
    print(f"eFSM = {p.r(0x200000F0):#010x}")
    p.w(0x20000028, 0x00000FFF)           # force VSP command to max
    print(f"DHCSR = {p.halt():#010x}")    # halt + read DHCSR
    p.go()                                 # resume target
"""
import argparse
import sys
import time
import serial


class Probe:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 1.0):
        self.s = serial.Serial(port, baud, timeout=timeout)
        # Drain any boot banner
        time.sleep(0.1)
        self.s.reset_input_buffer()

    # Markers that identify a real response (not just the echoed command).
    _TERMINATORS = ("ack=", "dhcsr=", "idcode=", "probe v1")

    def _cmd(self, line: str) -> str:
        # Discard any stale data from previous commands.
        self.s.reset_input_buffer()
        self.s.write((line + "\n").encode("ascii"))
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            raw = self.s.readline().decode("ascii", errors="replace").strip()
            if not raw:
                continue
            # Only accept lines that contain a known response terminator.
            # Echoes from the firmware's uart_readline don't have these.
            if any(t in raw for t in self._TERMINATORS):
                return raw
        raise RuntimeError(f"timeout waiting for response to {line!r}")

    def r(self, addr: int) -> int:
        """Read 32-bit word at target address via SWD."""
        resp = self._cmd(f"R {addr:08x}")
        # Expected: "R <addr> <val> ack=NNN"
        parts = resp.split()
        if len(parts) < 3 or parts[0] != "R":
            raise RuntimeError(f"bad read response: {resp!r}")
        return int(parts[2], 16)

    def w(self, addr: int, val: int) -> str:
        """Write 32-bit word via SWD. Returns the raw ack string for logging."""
        resp = self._cmd(f"W {addr:08x} {val:08x}")
        parts = resp.split()
        if len(parts) < 4 or parts[0] != "W":
            raise RuntimeError(f"bad write response: {resp!r}")
        return parts[-1]  # "ack=NNN"

    def halt(self) -> int:
        resp = self._cmd("H")
        # "H dhcsr=XXXXXXXX"
        for p in resp.split():
            if p.startswith("dhcsr="):
                return int(p[6:], 16)
        raise RuntimeError(f"halt: no dhcsr in {resp!r}")

    def go(self) -> int:
        resp = self._cmd("G")
        for p in resp.split():
            if p.startswith("dhcsr="):
                return int(p[6:], 16)
        raise RuntimeError(f"go: no dhcsr in {resp!r}")

    def idcode(self) -> int:
        resp = self._cmd("I")
        for p in resp.split():
            if p.startswith("idcode="):
                return int(p[7:], 16)
        raise RuntimeError(f"idcode: no idcode in {resp!r}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Sanity check for the PM32F407 SWD probe firmware. Opens the serial "
            "port, reads the target's IDCODE, and prints it. PM30225V (HK32F030 "
            "silicon) reports 0x0BB11477. For richer usage, import Probe and "
            "script against p.r(), p.w(), p.halt(), p.go()."
        )
    )
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", default=115200, type=int)
    args = ap.parse_args()

    p = Probe(args.port, args.baud)
    idcode = p.idcode()
    print(f"IDCODE = {idcode:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
