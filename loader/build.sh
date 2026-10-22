#!/bin/bash
set -e
cd "$(dirname "$0")"
case "${XNU_PLATFORM:-auto}" in
  auto) PLATFORM_FLAGS=; OUTPUT=loader ;;
  macan) PLATFORM_FLAGS=-DXNU_PLATFORM_MACAN=1; OUTPUT=loader-macan ;;
  *) echo "Unknown XNU_PLATFORM (use auto or macan)" >&2; exit 2 ;;
esac
EXTRA_FLAGS=()
if [ "${XNU_PLATFORM:-auto}" = macan ] && [ -n "${MACAN_KERNEL_HEADER:-}" ]; then
  EXTRA_FLAGS=(-include "$MACAN_KERNEL_HEADER")
fi
clang $PLATFORM_FLAGS "${EXTRA_FLAGS[@]}" --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -c loader.c -o "$OUTPUT.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:"$OUTPUT.efi" "$OUTPUT.obj"
ls -la "$OUTPUT.efi"
