#!/usr/bin/env python3
"""Build PM32F407 SWD flasher firmware.

Requires:
  - WSL Ubuntu (if building on Windows), with arm-none-eabi-gcc 14.2+
    Install via tarball (no sudo needed):
      wget https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi.tar.xz
      tar -xJf arm-gnu-toolchain-*.tar.xz -C ~/toolchain/

Outputs firmware/build/fw.bin (flashable via J-Link or stm32flash UART).
"""
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FW_DIR = REPO_ROOT / "firmware"
BUILD_DIR = FW_DIR / "build"
BUILD_DIR.mkdir(exist_ok=True)

TOOLCHAIN = "~/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin"
GCC = f"{TOOLCHAIN}/arm-none-eabi-gcc"
OBJCOPY = f"{TOOLCHAIN}/arm-none-eabi-objcopy"
SIZE = f"{TOOLCHAIN}/arm-none-eabi-size"

SOURCES = ["startup.c", "main.c", "target_blob.c"]
CFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
    "-Os", "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra", "-std=c11",
]
LDFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft",
    "-nostartfiles", "-T", "linker.ld",
    "-Wl,--gc-sections", "-Wl,-Map,build/fw.map",
    "-specs=nosys.specs",
]


def to_wsl_path(p: str) -> str:
    p = str(p).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        return f"/mnt/{p[0].lower()}{p[2:]}"
    return p


def run(cmd_str: str) -> subprocess.CompletedProcess:
    if sys.platform.startswith("win"):
        return subprocess.run(
            ["wsl", "-d", "Ubuntu", "-e", "bash", "-c", cmd_str],
            capture_output=True, text=True, timeout=120,
        )
    return subprocess.run(cmd_str, shell=True, capture_output=True, text=True, timeout=120)


def main() -> int:
    src_dir_wsl = to_wsl_path(FW_DIR) if sys.platform.startswith("win") else str(FW_DIR)

    objects = []
    for src in SOURCES:
        obj = f"build/{src.replace('.c', '.o')}"
        objects.append(obj)
        cmd = f"cd {src_dir_wsl} && {GCC} {' '.join(CFLAGS)} -c {src} -o {obj}"
        print(f"[CC] {src}")
        r = run(cmd)
        if r.returncode != 0:
            print(f"[ERROR] compile failed:\n{r.stderr}")
            return 1

    elf = "build/fw.elf"
    cmd = f"cd {src_dir_wsl} && {GCC} {' '.join(LDFLAGS)} {' '.join(objects)} -o {elf}"
    print("[LD] fw.elf")
    r = run(cmd)
    if r.returncode != 0:
        print(f"[ERROR] link failed:\n{r.stderr}")
        return 1

    binf = "build/fw.bin"
    cmd = f"cd {src_dir_wsl} && {OBJCOPY} -O binary {elf} {binf}"
    print("[OBJCOPY] fw.bin")
    r = run(cmd)
    if r.returncode != 0:
        print(f"[ERROR] objcopy failed:\n{r.stderr}")
        return 1

    cmd = f"cd {src_dir_wsl} && {SIZE} {elf}"
    r = run(cmd)
    print(r.stdout)

    bin_host = BUILD_DIR / "fw.bin"
    if bin_host.exists():
        size = bin_host.stat().st_size
        print(f"[OK] firmware/build/fw.bin ({size} bytes)")
        print(f"     Flash via J-Link: JLink.exe loadbin {bin_host} 0x08000000")
        print(f"     Flash via UART:   stm32flash -f F4 -P COMx -w {bin_host}")
    else:
        print(f"[ERROR] {bin_host} not found after build")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
