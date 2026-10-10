#!/bin/sh
# Run from repository root. Old and new executables are explicit inputs.
set -eu
: "${BASELINE_QEMU:?path to a472543 qemu-system-arm}"
: "${NEW_QEMU:?path to V4 qemu-system-arm}"
out=${1:-evidence/qspi-v4-reproduce}
mkdir -p "$out"
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -ffreestanding -nostdlib -Os \
  -Wall -Wextra -Werror -Wl,--fatal-warnings \
  -Wl,-T,tests/functional/arm/guest-src/bk7258/diagnostic.ld \
  tests/functional/arm/guest-src/bk7258/qspi_pio.c -o "$out/qspi_pio.elf"
sha256sum "$out/qspi_pio.elf" "$BASELINE_QEMU" "$NEW_QEMU" > "$out/inputs.sha256"
set -- -M t5_board -display none -serial stdio -monitor none \
  -semihosting-config enable=on,target=native \
  -icount shift=0,align=off,sleep=off -kernel "$out/qspi_pio.elf" \
  -d guest_errors,unimp
set +e
timeout 15 "$BASELINE_QEMU" "$@" -D "$out/old-mmio.log" > "$out/old-uart.log" 2>&1
old_status=$?
set -e
printf '%s\n' "$old_status" > "$out/old-exit.txt"
test "$old_status" = 1
# The old binary has no QSPI buses. Only V4 can attach these explicit fixtures.
timeout 15 "$NEW_QEMU" "$@" -device w25q32,bus=qspi0,cs=0 \
  -device w25q32,bus=qspi1,cs=0 -trace enable=m25p80_select \
  -trace enable=m25p80_transfer -D "$out/new-trace.log" > "$out/new-uart.log" 2>&1
# Host timeout alone is never a pass; guest exit and native frame checks matter.
