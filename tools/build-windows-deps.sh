#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sources=$root/vendor/sources
target=${CROSS_PREFIX:-x86_64-w64-mingw32-}
jobs=${JOBS:-4}
work=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        printf 'Dependency build failed. Build directory: %s\n' "$work" >&2
    else
        rm -rf -- "$work"
    fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
stage=$work/stage
sdk=$root/vendor/windows-x64
dist=$root/dist/windows-x64

cd "$sources"
sha256sum -c SHA256SUMS
cd "$work"
tar -xf "$sources/ffmpeg_7.1.5.orig.tar.xz"
tar -xf "$sources/libsdl2_2.32.4+dfsg.orig.tar.gz"
tar -xf "$sources/x264_0.164.3108+git31e19f9.orig.tar.gz"
tar -xf "$sources/mingw-w64_12.0.0.orig.tar.bz2"
export PKG_CONFIG_LIBDIR="$stage/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
export LC_ALL=C

cd "$work/x264-0.164.3108+git31e19f9"
./configure --host=x86_64-w64-mingw32 --cross-prefix="$target" \
    --prefix="$stage" --enable-static --disable-cli --disable-opencl
make -j"$jobs"
make install

cd "$work/ffmpeg-7.1.5"
./configure --target-os=mingw32 --arch=x86_64 --cross-prefix="$target" \
    --prefix="$stage" --enable-cross-compile --enable-shared --disable-static \
    --enable-gpl --enable-libx264 --pkg-config=pkg-config --pkg-config-flags=--static \
    --disable-autodetect --enable-w32threads --disable-pthreads --disable-network \
    --disable-programs --disable-doc --disable-debug --disable-avdevice \
    --disable-avfilter --disable-postproc --disable-encoders --enable-encoder=libx264,ffv1 \
    --disable-muxers --enable-muxer=mp4,matroska --extra-ldflags=-static-libgcc
make -j"$jobs"
make install

cd "$work/SDL2-2.32.4"
CC="${target}gcc" WINDRES="${target}windres" LDFLAGS=-static-libgcc \
    ./configure --build="$(./build-scripts/config.guess)" --host=x86_64-w64-mingw32 \
    --prefix="$stage" --enable-shared --disable-static --disable-sdl2main
make -j"$jobs"
make install

cd "$work/mingw-w64-v12.0.0/mingw-w64-libraries/winpthreads"
CC="${target}gcc" LDFLAGS=-static-libgcc \
    ./configure --host=x86_64-w64-mingw32 --prefix="$stage" --enable-shared --disable-static
make -j"$jobs"
make install

mkdir -p "$sdk/include" "$sdk/lib" "$dist/licenses"
for library in SDL2 libavcodec libavformat libavutil libswresample libswscale; do
    cp -R "$stage/include/$library" "$sdk/include/"
done
for library in SDL2 avcodec avformat avutil swresample swscale; do
    cp "$stage/lib/lib$library.dll.a" "$sdk/lib/"
done
for dll in "$stage/bin/"*.dll; do
    "${target}strip" --strip-unneeded "$dll"
    cp "$dll" "$dist/"
done
cp "$work/SDL2-2.32.4/LICENSE.txt" "$dist/licenses/SDL2.txt"
cp "$work/ffmpeg-7.1.5/COPYING.GPLv2" "$dist/licenses/FFmpeg.txt"
cp "$work/x264-0.164.3108+git31e19f9/COPYING" "$dist/licenses/x264.txt"
cp "$work/mingw-w64-v12.0.0/mingw-w64-libraries/winpthreads/COPYING" "$dist/licenses/Winpthreads.txt"
