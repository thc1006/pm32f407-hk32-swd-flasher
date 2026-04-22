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
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", default=115200, type=int)
    ap.add_argument("cmd", nargs="?", default="snapshot")
    args = ap.parse_args()

    p = Probe(args.port, args.baud)

    if args.cmd == "force-align-with-tpoe":
        # Combined: neutralize VSP_Control, force ALIGN, and manually set TPOE
        # so PWM output definitely reaches the MOSFETs even if REG_PWM_Out_Enable
        # inside the case handler is a no-op.
        w = p.r(0x20000024)
        p.w(0x20000024, w & 0xFFFF0000)     # unStopLevel = 0
        p.w(0x40004494, 0x0000A5A5)
        tpps_new = p.r(0x40004480) | (1 << 14)
        p.w(0x40004480, tpps_new)
        p.w(0x40004494, 0x0000000A)
        p.w(0x200000F0, 7)                  # eFSM = ALIGN
        print("forced ALIGN + TPOE; watching duty response for 6s...")
        t0 = time.monotonic()
        while time.monotonic() - t0 < 6.0:
            cr0 = p.r(0x40004408) & 0xFFFF
            cr1 = p.r(0x4000440C) & 0xFFFF
            cr2 = p.r(0x40004410) & 0xFFFF
            fsm = p.r(0x200000F0) & 0xFF
            tpps = p.r(0x40004480)
            tpoe = (tpps >> 14) & 1
            print(f"  t={time.monotonic()-t0:5.2f}s  eFSM={fsm}  TPOE={tpoe}  duties=({cr0} {cr1} {cr2})")
            time.sleep(0.3)
        return 0

    if args.cmd == "force-tpoe":
        # Directly set ATU.TPPS bit 14 (TPOE = master PWM output enable)
        # via SWD, replicating what REG_PWM_Out_Enable does:
        #   ATU->TRWPT = 0xA5A5    (unlock protected regs)
        #   ATU->TPPS  |= (1<<14)
        #   ATU->TRWPT = 0xA       (relock)
        tpps_before = p.r(0x40004480)
        tpoe_before = (tpps_before >> 14) & 1
        print(f"before: TPPS={tpps_before:#010x}  TPOE={tpoe_before}")
        p.w(0x40004494, 0x0000A5A5)    # TRWPT unlock
        # read-modify-write TPPS
        tpps_rmw = p.r(0x40004480) | (1 << 14)
        p.w(0x40004480, tpps_rmw)
        p.w(0x40004494, 0x0000000A)    # TRWPT relock
        tpps_after = p.r(0x40004480)
        tpoe_after = (tpps_after >> 14) & 1
        print(f"after:  TPPS={tpps_after:#010x}  TPOE={tpoe_after}")
        if tpoe_after == 0:
            print("!! TPOE write did not stick — protection still blocking or wrong reg")
        else:
            print("PWM output enabled — motor should twitch/spin if FOC is commanding duty != 50%")
        # Dump duties
        cr0a = p.r(0x40004408) & 0xFFFF
        cr1a = p.r(0x4000440C) & 0xFFFF
        cr2a = p.r(0x40004410) & 0xFFFF
        tpr = p.r(0x40004400) & 0xFFFF
        print(f"TPR={tpr}  CR0A={cr0a} ({100*cr0a/tpr:.1f}%)  CR1A={cr1a} ({100*cr1a/tpr:.1f}%)  CR2A={cr2a} ({100*cr2a/tpr:.1f}%)")
        return 0

    if args.cmd == "force-align":
        # Keep VSP_Control from slamming us back to SOFTSTOP.
        w = p.r(0x20000024)
        p.w(0x20000024, w & 0xFFFF0000)    # unStopLevel = 0
        # eFSM = E_FSM_SYS_ALIGN (7): handler calls MotorAlign + REG_PWM_Out_Enable
        p.w(0x200000F0, 7)
        print("forced eFSM=ALIGN; watching for PWM enable + motor twitch...")
        t0 = time.monotonic()
        last_tpoe = -1
        last_fsm = -1
        while time.monotonic() - t0 < 8.0:
            fsm = p.r(0x200000F0) & 0xFF
            tpps = p.r(0x40004480)
            tpoe = (tpps >> 14) & 1
            cr0a = p.r(0x40004408) & 0xFFFF
            cr1a = p.r(0x4000440C) & 0xFFFF
            cr2a = p.r(0x40004410) & 0xFFFF
            if tpoe != last_tpoe or fsm != last_fsm:
                print(f"  t={time.monotonic()-t0:5.2f}s  eFSM={fsm}  TPOE={tpoe}  CR0A={cr0a} CR1A={cr1a} CR2A={cr2a}")
                last_tpoe = tpoe
                last_fsm = fsm
            time.sleep(0.05)
        return 0

    if args.cmd == "force-start":
        # Motor demo's VSP_Control polls at 100 Hz:
        #   if VspCommand > unStartLevel → STANDBY → DET
        #   if VspCommand < unStopLevel  → (not STANDBY/INIT/BRAKE) → SOFTSTOP
        # With unVspCommand stuck at 0 (VSP ADC reads 0), we fall into the
        # second branch → any forced eFSM gets slammed back to SOFTSTOP within
        # 10 ms. Defeat that by zeroing unStopLevel first, then forcing eFSM.
        w = p.r(0x20000024)
        factor = w & 0xFFFF0000  # preserve unFactor in high half
        p.w(0x20000024, factor)  # unStopLevel = 0
        print(f"unStopLevel zeroed; word @ 0x20000024 now = {p.r(0x20000024):#010x}")
        # Force eFSM = E_FSM_SYS_DET (5)
        p.w(0x200000F0, 5)
        print(f"eFSM forced to DET; word @ 0x200000F0 now = {p.r(0x200000F0):#010x}")
        # Watch for 5 seconds — sample state machine and PWM enable bit.
        print("watching state... (5 sec)")
        t0 = time.monotonic()
        last_fsm = -1
        last_tpps = -1
        while time.monotonic() - t0 < 5.0:
            fsm = p.r(0x200000F0) & 0xFF
            tpps = p.r(0x40004480)
            if fsm != last_fsm or (tpps & (1 << 14)) != (last_tpps & (1 << 14)):
                moe = "ON" if (tpps & (1 << 14)) else "off"
                cr0a = p.r(0x40004408) & 0xFFFF
                cnt = p.r(0x40004404) & 0xFFFF
                print(f"  t={time.monotonic()-t0:5.2f}s  eFSM={fsm}  TPPS={tpps:#010x} PWM={moe}  CR0A={cr0a}  CNT={cnt}")
                last_fsm = fsm
                last_tpps = tpps
            time.sleep(0.05)
        return 0

    if args.cmd == "snapshot":
        print(f"IDCODE            = {p.idcode():#010x}")
        print(f"eFSM              = {p.r(0x200000F0):#010x}   (1=INIT 2=STANDBY 3=STOP 4=RUN 5=DET 6=RAMP 7=ALIGN)")
        w = p.r(0x200000F8)
        print(f"eSYSPROTECT packed = {w:#010x}")
        w = p.r(0x20000028)
        vsp_cmd = w & 0xFFFF
        print(f"gS_VSP.unVspCommand = {vsp_cmd:#06x} = {vsp_cmd}  (threshold unStartLevel=568)")
        w = p.r(0x200000EC)
        vsp_raw = w & 0xFFFF
        is_raw = (w >> 16) & 0xFFFF
        print(f"u16ADCValue[3] VSP_raw = {vsp_raw}   u16ADCValue[4] IS_raw = {is_raw}")
        w = p.r(0x200000E8)
        iv_raw = w & 0xFFFF
        vbus_raw = (w >> 16) & 0xFFFF
        print(f"u16ADCValue[1] IV_raw  = {iv_raw}   u16ADCValue[2] VBUS_raw = {vbus_raw}")
        print(f"ATU.TPPS          = {p.r(0x40004480):#010x}   (bit 14 TPOE = master PWM enable)")
        print(f"ATU.CNT           = {p.r(0x40004404):#010x}")
        print(f"ATU.CR0A          = {p.r(0x40004408):#010x}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
