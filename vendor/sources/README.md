# Windows Dependency Sources

I built the bundled Windows x64 libraries from the verified Debian 13 source archives listed in [SHA256SUMS](SHA256SUMS). I build the original source archives from the Debian source packages directly. I link x264 into the FFmpeg DLLs and use the FFmpeg GPL version 2 or later configuration.

| Component | Source |
| --- | --- |
| FFmpeg 7.1.5 | [Debian FFmpeg Source Package](https://tracker.debian.org/pkg/ffmpeg) |
| SDL 2.32.4 | [Debian SDL2 Source Package](https://tracker.debian.org/pkg/libsdl2) |
| x264 Core 164, Commit 31e19f9 | [Debian x264 Source Package](https://tracker.debian.org/pkg/x264) |
| MinGW-w64 12.0.0 Winpthreads | [Debian MinGW-w64 Source Package](https://tracker.debian.org/pkg/mingw-w64) |
| Dear ImGui 1.90.1 | [Debian Dear ImGui Source Package](https://tracker.debian.org/pkg/imgui) |

I keep the Dear ImGui translation units, backend sources, headers, and license in [The ImGui Directory](../imgui/) in the same form used to build the application. I keep the Windows library configuration and installation commands in [The Build Script](../../tools/build-windows-deps.sh).

I used the Debian 13 MinGW-w64 12.0.0 headers and runtime, GCC 14.2.0, binutils 2.44, and NASM 2.16.03. The [Runtime Notices](../../dist/windows-x64/licenses/) include the MinGW-w64 terms and the GCC Runtime Library Exception.
