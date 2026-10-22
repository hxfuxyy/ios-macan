#!/bin/bash
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../tools/xnu-env.sh
STOREFRONT=${STOREFRONT-1}
GL=${GL-1}

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
DYN="xnu-clang -arch arm64 -isysroot $SDK -O1"
INJECT=${INJECT-"/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libredwood-sb.dylib SpringBoard
/redwood/lib/libredwood-media.dylib mediaserverd
/redwood/lib/libredwood-app.dylib Sileo
/redwood/lib/libredwood-tweaks.dylib Preferences"}
[ -n "$STOREFRONT" ] && INJECT="$INJECT
/redwood/lib/libredwood-store.dylib appstored AppStore
/redwood/lib/libredwood-fairplay.dylib fairplayd.H2"
mkdir -p obj

if ! command -v ld64 >/dev/null; then
	echo "build.sh: no ld64: set up the cross toolchain first (../xnu-deps/setup.sh, theos)" >&2
	exit 1
fi
$CC $LD -o hello src/hello.c
$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
$CC -c libmini/start.S -o obj/start.o
for prog in init child stage0 var_store; do
	$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
	cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
done

rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp com.redwood.varstore.plist root/LaunchDaemons/
echo "Darwin on redwood: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done
if [ -d $SDK/usr/lib ]; then
	if :; then
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood.dylib src/inject.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood-sb.dylib src/inject_sb.c src/inject_island.m -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libredwood-tweaks.dylib src/inject_tweaks.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-app.dylib -lobjc \
			-o obj/libredwood-app.dylib src/inject_app.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -lobjc \
			-o obj/libredwood-media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-store.dylib -framework CoreFoundation -lobjc \
			-lc++abi -o obj/libredwood-store.dylib src/inject_store.c
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-fairplay.dylib -framework CoreFoundation \
			-framework IOKit -framework Security -lMobileGestalt \
			-o obj/libredwood-fairplay.dylib src/inject_fairplay.c
		$DYN -bundle -framework CoreFoundation -o obj/RedwoodAudio src/audio_plugin.c
		if [ -n "$GL" ]; then
			M=../xnu-deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../tools/mesa/*.patch ../tools/mesa/drm-shim/* | sha256sum)" ] ||
				../tools/mk-mesa.sh
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			cp $M/libOSMesa.8.dylib obj/
		fi
	fi
	cp obj/hello_dyn obj/svc_test obj/surf_test obj/mg_test root/bin/
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	for f in $EXTRA; do cp "$f" root/bin/; done
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libredwood.dylib obj/libredwood-sb.dylib root/lib/
	[ -f obj/libredwood-media.dylib ] && cp obj/libredwood-media.dylib root/lib/
	[ -f obj/libredwood-store.dylib ] && cp obj/libredwood-store.dylib root/lib/
	[ -f obj/libredwood-fairplay.dylib ] && cp obj/libredwood-fairplay.dylib root/lib/
	[ -f obj/libredwood-app.dylib ] && cp obj/libredwood-app.dylib root/lib/
	[ -f obj/libredwood-tweaks.dylib ] && cp obj/libredwood-tweaks.dylib root/lib/
	if [ -f obj/RedwoodAudio ]; then
		mkdir -p root/lib/RedwoodAudio.driver
		cp obj/RedwoodAudio root/lib/RedwoodAudio.driver/
		cp src/audio_plugin.plist root/lib/RedwoodAudio.driver/Info.plist
	fi
	if [ -n "$GL" ]; then
		ln -sf /usr/local/redwood/libOSMesa.8.dylib root/lib/libOSMesa.8.dylib
		[ "$GL" = test ] || cp obj/libredwood-gl.dylib root/lib/
	fi
	echo "$INJECT" | while read -r library names; do
		case "$library" in
		"") ;;
		/redwood/lib/*) if [ -f "root/lib/${library#/redwood/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	: > root/etc/var.skel.redwood
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel.redwood
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel.redwood
	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel.redwood
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel.redwood
		done
	fi
	cp ${RC:-rc.launchd} root/etc/rc
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	rm -f root/etc/quiet
	[ -n "$QUIET" ] && : > root/etc/quiet
	cp com.redwood.demo.plist com.redwood.wifi.plist root/LaunchDaemons/
fi
secrets=
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] && secrets=" /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi
mkdir -p root/empty root/xpc root/hal
[ -d root/lib/RedwoodAudio.driver ] && ln -s /redwood/lib/RedwoodAudio.driver root/hal/RedwoodAudio.driver
echo "build.sh: programs in obj/, the root in root/ (images: ../tools/assemble.py)"
