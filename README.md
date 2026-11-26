# Automatic Mosaic Remover

I use this desktop app to reduce a block mosaic with a manually selected frame interval, region, and grid. The current version estimates a single frame at a time by combining block fidelity with total variation. Automatic detection, tracking, and reconstruction from nearby frames are planned in the [Detailed Design](doc/detailed_design.md). A permanently hidden region has insufficient evidence to recover its original fine detail.

## Build and run

I tested the Linux build with GCC 14.2, SDL 2.32.4, FFmpeg 7.1.5, and the Dear ImGui 1.90.1 source tree. On Linux, install GCC with C++20 support, GNU Make, pkg-config, and the development packages for SDL2 and FFmpeg, including libavformat, libavcodec, libavutil, and libswscale. On Debian 13 the development packages are `libsdl2-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev`. Set `IMGUI_DIR` to the extracted Dear ImGui 1.90.1 release.

```sh
make IMGUI_DIR=/path/to/imgui-1.90.1
./bin/mosaic-remover
```

For w64devkit, I use the same Makefile with a directory containing the x86-64 MinGW-compatible SDL2 and FFmpeg headers and import libraries. The default layout is `DEP_PREFIX/include` and `DEP_PREFIX/lib`. I place the required DLLs beside the executable or on `PATH`. Windows build and runtime validation remain open.

```sh
make DEP_PREFIX=C:/path/to/deps IMGUI_DIR=C:/path/to/imgui-1.90.1
./bin/mosaic-remover.exe
```

I run `make test` for the numerical and boundary checks. This target only needs a C++20 compiler and Make. I run `make test-media` for the video workflow checks. It also uses Python 3, the FFmpeg development libraries, and the `ffmpeg` and `ffprobe` executables with libx264 available.

## Edit and export

I drop a local video onto the window or enter its path and open it. The app indexes the frame count in the background before enabling the scrubber. I choose the first and last frames inclusively, then drag the region over the video or type its exact bounds. I enter the block width, height, and X and Y offsets in input pixels. The bottom and right ROI edges are exclusive.

I select **Preview restoration** to process the current frame and use **Show restored** to compare it with the source. I enter an output path and export to H.264 MP4 or lossless FFV1 MKV. MP4 requires the libx264 encoder and even video dimensions. The lossless output preserves unchanged RGB pixels in the processing raster. I can copy compatible audio and cancel an export while it is running. I can scrub backward immediately after an index completes, but this first version decodes forward from the beginning to find an earlier frame.
