#!/usr/bin/env bash
# Regenerate firmware/target_blob.c from a .hex or .bin target image.
#
# Usage:
#   tools/gen_target_blob.sh path/to/target_firmware.hex
#   tools/gen_target_blob.sh path/to/target_firmware.bin
#
# The generated target_blob.c contains a uint8_t TARGET_BLOB[] + uint32_t
# TARGET_BLOB_BYTES with the image laid out little-endian (native Flash
# storage order). Rebuild fw.bin afterwards via `python tools/build.py`.
#
# Typically you do NOT want to commit a vendor-sourced blob — add your
# generated target_blob.c to .gitignore locally or stash it before pushing.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <target.hex | target.bin>" >&2
  exit 1
fi

SRC="$1"
if [[ ! -f "$SRC" ]]; then
  echo "error: $SRC not found" >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/firmware/target_blob.c"
TMP_BIN="$(mktemp --suffix=.bin)"
trap "rm -f $TMP_BIN" EXIT

# Pick objcopy. Prefer Arm toolchain from the standard WSL location used by
# tools/build.py; fall back to system objcopy if already on PATH.
OBJCOPY=""
for cand in \
  "$HOME/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-objcopy" \
  "$(command -v arm-none-eabi-objcopy || true)"; do
  if [[ -n "$cand" && -x "$cand" ]]; then OBJCOPY="$cand"; break; fi
done

case "$SRC" in
  *.hex|*.HEX)
    if [[ -z "$OBJCOPY" ]]; then
      echo "error: arm-none-eabi-objcopy not found — needed for .hex input" >&2
      exit 1
    fi
    "$OBJCOPY" -I ihex -O binary "$SRC" "$TMP_BIN"
    ;;
  *.bin|*.BIN)
    cp "$SRC" "$TMP_BIN"
    ;;
  *)
    echo "error: unrecognised extension on $SRC (expected .hex or .bin)" >&2
    exit 1
    ;;
esac

BYTES=$(stat -c%s "$TMP_BIN")
BASENAME=$(basename "$SRC")

{
  echo "/* Auto-generated from ${BASENAME} (${BYTES} bytes)."
  echo " * Generator: tools/gen_target_blob.sh"
  echo " * Do not commit if the source image has restrictive licensing. */"
  echo '#include "target_blob.h"'
  echo ""
  xxd -i < "$TMP_BIN" | head -c 0   # prime xxd invocation noop
  # xxd -i on stdin emits just the bytes; wrap them ourselves so we control
  # the symbol names regardless of input filename.
  echo "const uint8_t TARGET_BLOB[] = {"
  xxd -i < "$TMP_BIN"
  echo "};"
  echo "const uint32_t TARGET_BLOB_BYTES = ${BYTES};"
} > "$OUT"

echo "wrote $OUT ($BYTES bytes blob)"
echo "rebuild with: python tools/build.py"
