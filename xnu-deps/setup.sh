#!/bin/bash
set -e -o pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$HERE/../tools/xnu-env.sh"
cd "$XNUDEPS"
JOBS=${JOBS:-$(nproc)}
CCTOOLS=$THEOS/toolchain/linux/iphone/bin
IOS_SDK=$THEOS/sdks/iPhoneOS16.5.sdk
MACOS_SDK_URL=https://github.com/joseluisq/macosx-sdks/releases/download/13.3/MacOSX13.3.sdk.tar.xz

log() { echo "setup: $*"; }
die() { echo "setup: $*" >&2; exit 1; }

fetch() {
	[ -d "$1" ] && git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1 &&
		[ "$(git -C "$1" rev-parse --show-toplevel)" = "$(cd "$1" && pwd -P)" ] && return 0
	log "fetching $1"
	rm -rf "$1"
	git init -q "$1"
	git -C "$1" remote add origin "$2"
	git -C "$1" fetch -q --depth 1 origin "$3" || die "cannot fetch $3 from $2"
	git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}

link() {
	[ "$(readlink "$2" 2>/dev/null)" = "$1" ] || ln -sfn "$1" "$2"
}

update() {
	cat > "$1.new"
	if cmp -s "$1.new" "$1"; then rm "$1.new"; else mv "$1.new" "$1"; fi
}

stage1() {
	local missing= tool
	for tool in git curl tar xz make cc clang clang++ flex bison m4 perl python3 autoreconf automake; do
		command -v $tool >/dev/null || missing="$missing $tool"
	done
	[ -z "$missing" ] || die "not installed:$missing"
	[ -x "$CCTOOLS/ld" ] || die "no cctools in $CCTOOLS: install theos with its Linux toolchain (or set THEOS)"
	[ -d "$IOS_SDK/usr/include" ] || die "no $IOS_SDK (the SDKs of github.com/theos/sdks go to \$THEOS/sdks)"

	fetch AvailabilityVersions https://github.com/apple-oss-distributions/AvailabilityVersions.git \
		5d9baee345b3bc72b26c71adc65f94f2d5e9ced2
	fetch libdispatch https://github.com/apple-oss-distributions/libdispatch.git \
		a641558f447659d90ad002ef0ee6f0212273fbfc
	fetch libpthread https://github.com/apple-oss-distributions/libpthread.git \
		a6c70ca0e544347a0377babf748f831577ec24f9
	fetch mm_bootstrap_cmds https://github.com/markmentovai/bootstrap_cmds \
		f337e600741638896fb6056ce87c6479f74b89a3
	fetch tcsh https://github.com/tcsh-org/tcsh.git b7db07931546178c4cb2542b5fc850c483628ed5
	fetch unifdef https://github.com/fanf2/unifdef.git 08d12cd3059fe072929ea001ad9833f5487fd355

	if [ ! -d MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers/DriverKit ]; then
		log "downloading the macOS 13.3 SDK for its DriverKit headers"
		curl -L --fail --retry 3 -s -o macos-sdk.tar.xz "$MACOS_SDK_URL" || die "download failed: $MACOS_SDK_URL"
		tar -xJf macos-sdk.tar.xz --wildcards '*MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework*' ||
			die "cannot unpack the macOS SDK"
		rm -f macos-sdk.tar.xz
	fi

	mkdir -p host/bin
	if [ ! -x host/bin/migcom ] || [ ! -x host/bin/mig ]; then
		log "building mig"
		(cd mm_bootstrap_cmds && autoreconf --install && sh configure --prefix="$XNUDEPS/host" &&
			make -j$JOBS && make install) > mig-build.log 2>&1 || die "mig did not build, see xnu-deps/mig-build.log"
	fi
	if [ ! -x host/bin/unifdef ]; then
		log "building unifdef"
		[ -f unifdef/version.h ] || printf '"@(#) $Version: unifdef-2.12 $\\n"\n"@(#) $Date: 2020-11-06 11:22:51 +0000 $\\n"\n' > unifdef/version.h
		cc -std=gnu11 -O2 -w -o host/bin/unifdef unifdef/unifdef.c
	fi
	if [ ! -x host/bin/tcsh ]; then
		log "building tcsh"
		(cd tcsh && ./configure --prefix="$XNUDEPS/host" && make -j$JOBS) > tcsh-build.log 2>&1 ||
			die "tcsh did not build, see xnu-deps/tcsh-build.log"
		cp tcsh/tcsh host/bin/tcsh
	fi
	link tcsh host/bin/csh
	if [ ! -x host/bin/unzbm ]; then
		cc -O2 -w -o host/bin/unzbm build/lzbitmap/unzbm.c build/lzbitmap/libzbitmap.c
	fi
	for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
		link "$CCTOOLS/$tool" host/bin/$tool
	done
	link "$CCTOOLS/ld" host/bin/ld64
	link xnu-clang host/bin/xnu-clang++

	mkdir -p sdk/Platforms/iPhoneOS.platform
	if [ ! -d "$XNU_SDK_PATH/usr/include" ]; then
		log "making the SDK: links to $IOS_SDK"
		rm -rf "$XNU_SDK_PATH"
		cp -rs "$(cd "$IOS_SDK" && pwd)" "$XNU_SDK_PATH"
	fi
	local sdk_local=$XNU_SDK_PATH/usr/local
	mkdir -p $sdk_local/include/kernel/os $sdk_local/include/os $sdk_local/lib/kernel $sdk_local/libexec
	update $sdk_local/libexec/availability.pl < AvailabilityVersions/availability.pl
	[ -x $sdk_local/libexec/availability.pl ] || chmod +x $sdk_local/libexec/availability.pl
	{ host/bin/unifdef -DOS_FIREHOSE_SPI=1 -DKERNEL=1 libdispatch/os/firehose_buffer_private.h || [ $? -eq 1 ]; } |
		update $sdk_local/include/kernel/os/firehose_buffer_private.h
	update $sdk_local/include/kernel/os/firehose_types.defs < libdispatch/src/firehose/firehose_types.defs
	log "stage 1 done"
}

stage2() {
	[ -f "$XNU_SRC/makedefs/MakeInc.def" ] || die "no kernel tree in $XNU_SRC (kernel/apply.sh makes it)"
	[ -x host/bin/migcom ] && [ -d "$XNU_SDK_PATH/usr/local/lib/kernel" ] || die "stage 1 has not been done"
	local lib=$XNU_SDK_PATH/usr/local/lib/kernel hdrs=$XNU_SRC/BUILD/obj/EXPORT_HDRS file
	mkdir -p hostinc/mach-o
	for file in loader.h nlist.h fat.h; do
		[ -f hostinc/mach-o/$file ] || cp "$XNU_SRC/EXTERNAL_HEADERS/mach-o/$file" hostinc/mach-o/
	done
	if [ -n "$REBUILD_LIBS" ]; then rm -f $lib/libfirehose_kernel.a $lib/libpthread_kext.a; fi
	if [ -f $lib/libfirehose_kernel.a ] && [ -f $lib/libpthread_kext.a ]; then
		log "stage 2: nothing to do"
		return 0
	fi
	log "exporting the kernel's headers (xnu-build.sh exporthdrs)"
	"$DARWIN_REDWOOD/tools/xnu-build.sh" -j$JOBS exporthdrs > exporthdrs.log 2>&1 && [ -d "$hdrs/osfmk" ] ||
		die "the kernel headers were not exported, see xnu-deps/exporthdrs.log"
	if [ ! -f $lib/libfirehose_kernel.a ]; then
		log "building libfirehose_kernel.a"
		mkdir -p build/firehose
		xnu-clang -arch arm64 -c -Os -mkernel -nostdinc -fno-builtin \
			-DKERNEL=1 -DKERNEL_PRIVATE=1 -DXNU_KERNEL_PRIVATE=0 -DPRIVATE=1 -D__DARWIN_64_BIT_INO_T=1 \
			-DDISPATCH_USE_DTRACE=0 -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 \
			-DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 -DOS_FIREHOSE_SPI=1 -DARM64_BOARD_CONFIG_BCM2837 \
			-Ilibdispatch -Ilibdispatch/src -Ilibdispatch/src/firehose \
			-I$hdrs/osfmk -I$hdrs/bsd -I$hdrs/libkern -I$hdrs/pexpert -I$hdrs/iokit -I$hdrs/security \
			-I$XNU_SRC/libkern/firehose -I$hdrs/libkern/os -I$XNU_SRC/EXTERNAL_HEADERS \
			-I$XNU_SDK_PATH/usr/local/include/kernel \
			-o build/firehose/firehose_buffer.o libdispatch/src/firehose/firehose_buffer.c
		libtool -static -o $lib/libfirehose_kernel.a build/firehose/firehose_buffer.o
	fi
	if [ ! -f $lib/libpthread_kext.a ]; then
		log "building libpthread_kext.a"
		build/pthread/build.sh
	fi
	log "stage 2 done; now: tools/xnu-build.sh -j$JOBS -k"
}

case "$1" in
--stage1) stage1 ;;
--stage2) stage2 ;;
--rebuild-libs) REBUILD_LIBS=1 stage2 ;;
"")
	stage1
	if [ -f "$XNU_SRC/makedefs/MakeInc.def" ]; then
		stage2
	else
		log "no kernel tree yet: run kernel/apply.sh, then this script again"
	fi ;;
*) sed -n '2,12p' "$HERE/setup.sh"; exit 1 ;;
esac
