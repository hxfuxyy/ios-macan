#!/bin/bash
set -e
VERSION=${MESA_VERSION:-26.3.0-rc1}
case $VERSION in
26.2.5) SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 ;;
26.3.0-rc1) SHA256=63327d58a51d189c1d1600746c505609bba5c495123b4e51de4e0c038b745542 ;;
*) echo "mk-mesa.sh: no checksum known for Mesa $VERSION" >&2; exit 1 ;;
esac
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
PATCHDIR=$ROOT/tools/mesa
[ -d "$ROOT/tools/mesa/v$VERSION" ] && PATCHDIR=$ROOT/tools/mesa/v$VERSION
source "$ROOT/tools/xnu-env.sh"
B=$ROOT/xnu-deps/build/mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
OUT=out
[ -z "$MESA_VERSION" ] || OUT=out-$VERSION
mkdir -p "$B/$OUT"
cd "$B"

PATCHES=$(cat "$PATCHDIR"/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.redwood-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-$VERSION-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$PATCHDIR"/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.redwood-patches
fi
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/drm-shim" mesa-$VERSION/src/redwood-drm
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging meson
fi
export PATH=$B/venv/bin:$PATH

cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-$VERSION-${DRIVERS//,/-}
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
	-Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log

llvm-strip -x -o $OUT/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /redwood/lib/libOSMesa.8.dylib $OUT/libOSMesa.8.dylib
nm -gU $OUT/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > $OUT/exported-gl.txt
cp mesa-$VERSION/src/mesa/glapi/glapi/registry/gl.xml $OUT/gl.xml
cat "$PATCHDIR"/*.patch "$ROOT"/tools/mesa/drm-shim/* | sha256sum > $OUT/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/$OUT/libOSMesa.8.dylib, $(stat -c %s $OUT/libOSMesa.8.dylib) bytes," \
	"$(wc -l < $OUT/exported-gl.txt) gl* functions"
