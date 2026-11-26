# Automatic Mosaic Remover

I use this desktop app to reduce a block mosaic with a manually selected frame interval, region, and grid. The current version estimates a single frame at a time by combining block fidelity with total variation. Automatic detection, tracking, and reconstruction from nearby frames are planned in the [Detailed Design](doc/detailed_design.md). A permanently hidden region has insufficient evidence to recover its original fine detail.

## Run on Windows

I include a portable Windows x64 build in [Windows x64](dist/windows-x64/). Download and extract the entire repository, then open `dist/windows-x64/mosaic-remover.exe`. Keep the bundled DLLs next to the executable.

## Build and test

On Windows x64 I build from the repository root in w64devkit. The Makefile uses the bundled MinGW import libraries, headers, and Dear ImGui source. It copies the runtime DLLs next to the newly built executable. Use a C++20-capable w64devkit toolchain. I verified the default build with w64devkit 2.11.0 under Wine and used the portable package to export H.264 video. Windows 11 26H2 hardware validation remains open.

```sh
make
./bin/mosaic-remover.exe
```

On Linux I use GCC with C++20 support, GNU Make, pkg-config, and the system SDL2 and FFmpeg 7 development libraries. On Debian 13 the development packages are `libsdl2-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`. I test with GCC 14.2, SDL 2.32.4, FFmpeg 7.1.5, and the bundled Dear ImGui 1.90.1 source.

```sh
make
./bin/mosaic-remover
```

I run `make test` for the numerical and boundary checks. It only needs a C++20 compiler and Make. I run `make test-media` for the video workflow checks. It also uses Python 3 and `ffmpeg` and `ffprobe` executables with libx264 available for fixture generation.

## Rebuild the Windows package

I build the bundled libraries on Linux with x86-64 MinGW-w64 GCC and binutils, GNU Make, pkg-config, NASM, and the standard archive tools. [Windows Dependency Sources](vendor/sources/) records the inputs, compiler versions, and checksums. The script verifies and builds from the local archives.

```sh
JOBS=4 ./tools/build-windows-deps.sh
make -j4 OS=Windows_NT CXX=x86_64-w64-mingw32-g++ BUILD_DIR=bin/windows-x64 LDFLAGS=-s package-windows
```

On Windows I can rebuild the executable with `make package-windows` from w64devkit. When updating library versions, I also update the source archives and checksum manifest, rebuild the package, remove any obsolete DLLs, and test H.264 and FFV1 export before committing the corresponding files.

## License

I license the application under GPL-2.0-or-later. The Windows package links to the GPL configuration of FFmpeg with libx264 support. [License Text](LICENSE) and [Third-Party Notices](dist/windows-x64/licenses/) document the terms. The application source, corresponding dependency source archives, and build instructions are available with the binaries in this repository.

## Edit and export

I drop a local video onto the window or enter its path and open it. The app indexes the frame count in the background before enabling the scrubber. I choose the first and last frames inclusively, then drag the region over the video or type its exact bounds. I enter the block width, height, and X and Y offsets in input pixels. The bottom and right ROI edges are exclusive.

I select **Preview restoration** to process the current frame and use **Show restored** to compare it with the source. I enter an output path and export to H.264 MP4 or lossless FFV1 MKV. MP4 requires the libx264 encoder and even video dimensions. The lossless output preserves unchanged RGB pixels in the processing raster. I can copy compatible audio and cancel an export while it is running. I can scrub backward immediately after an index completes, but this first version decodes forward from the beginning to find an earlier frame.
