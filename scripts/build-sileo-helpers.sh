#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

out=${1:-artifacts/ace6t/sileo}
sdk=${SDK:-${THEOS:-$PWD/work/theos}/sdks/iPhoneOS16.5.sdk}
ldid=${LDID:-${THEOS:-$PWD/work/theos}/toolchain/linux/iphone/bin/ldid}
export PATH=$PWD/xnu-deps/host/bin:$PATH

command -v xnu-clang >/dev/null || { echo "build-sileo-helpers: no xnu-clang in xnu-deps/host/bin" >&2; exit 1; }
[[ -d $sdk ]] || { echo "build-sileo-helpers: no iOS SDK at $sdk (SDK=...)" >&2; exit 1; }
[[ -x $ldid ]] || { echo "build-sileo-helpers: no ldid at $ldid (LDID=...)" >&2; exit 1; }
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

static=(xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra)
"${static[@]}" -Iuserland/libmini -c userland/libmini/mini.c -o "$tmp/mini.o"
"${static[@]}" -c userland/libmini/start.S -o "$tmp/start.o"
"${static[@]}" -Iuserland/libmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
    -o "$out/bootstrap-copy" sileo/bootstrap_copy.c "$tmp/mini.o" "$tmp/start.o"

xnu-clang -arch arm64 -isysroot "$sdk" -O1 -o "$out/persona-init" sileo/persona_init.c
"$ldid" -Cadhoc -Ssileo/persona_init.entitlements "$out/persona-init"
"$ldid" -e "$out/persona-init" | grep -q persona-mgmt || { echo "build-sileo-helpers: persona-init lost its entitlement" >&2; exit 1; }

xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fobjc-arc -framework Foundation -o "$out/sileo-session" sileo/sileo_session.m
xnu-clang -arch arm64 -isysroot "$sdk" -O1 -fobjc-arc -framework Foundation -o "$out/open-sileo" sileo/open_sileo.m
ls -l "$out/bootstrap-copy" "$out/persona-init" "$out/sileo-session" "$out/open-sileo"
