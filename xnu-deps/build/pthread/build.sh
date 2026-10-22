#!/bin/bash
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
source ../../../tools/xnu-env.sh
X=$XNU_SRC; E=$X/BUILD/obj/EXPORT_HDRS; P=$XNUDEPS/libpthread
[ -d $E/osfmk ] || { echo "pthread kext: no exported kernel headers in $E (run: xnu-build.sh exporthdrs)" >&2; exit 1; }
mkdir -p $XNU_SDK_PATH/usr/local/lib/kernel
for f in kern_init kern_support kern_synch; do
  xnu-clang -arch arm64 -c -Os -mkernel -nostdinc -fno-builtin -fno-common -Wno-int-conversion \
    -DKERNEL=1 -DKERNEL_PRIVATE=1 -DPRIVATE=1 -D__PTHREAD_EXPOSE_INTERNALS__=1 -Dpthread_kern=pthread_kext_kern -Dcurrent_uthread=pthread_kext_current_uthread -D__DARWIN_64_BIT_INO_T=1 -DARM64_BOARD_CONFIG_BCM2837 \
    -Iinc -I$P -I$E/osfmk -I$E/bsd -I$E/libkern -I$E/pexpert -I$E/iokit -I$E/security -I$X/EXTERNAL_HEADERS \
    -I$P/include -I$P/private -o pthread_$f.o $P/kern/$f.c
done
libtool -static -o $XNU_SDK_PATH/usr/local/lib/kernel/libpthread_kext.a pthread_kern_init.o pthread_kern_support.o pthread_kern_synch.o
ls -la $XNU_SDK_PATH/usr/local/lib/kernel/libpthread_kext.a
