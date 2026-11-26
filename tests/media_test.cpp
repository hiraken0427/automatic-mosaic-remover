#include "infrastructure/ffmpeg_media.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    using namespace mosaic;
    if (argc != 5)
        return 2;
    try {
        FFmpegMedia media;
        const std::filesystem::path input = argv[1], output = argv[2];
        const auto index = index_video(media, input, {}, [](auto) {});
        if (index.frames != 8 || !index.info.has_audio)
            throw std::runtime_error("Unexpected fixture frame count or audio.");
        Preview preview;
        for (const auto ordinal : {0, 7, 2, 3, 3}) {
            const auto frame = preview.frame(media, input, ordinal, {});
            if (frame.ordinal != ordinal || frame.raster.pixels.empty())
                throw std::runtime_error("Preview returned the wrong frame.");
        }
        Edit edit{2, 6, {5, 4, 17, 16}, {4, 3, -3, 1}};
        if (std::string_view(argv[4]) == "detected") {
            auto candidates = preview.candidates(media, input, 3, edit.roi, {});
            if (candidates.empty())
                throw std::runtime_error("No mosaic candidate found in the lossless fixture.");
            const auto& candidate = candidates.front();
            const auto grid = candidate.grid;
            if (grid.width != 4 || grid.height != 3 || grid.phase_x != 1 || grid.phase_y != 1)
                throw std::runtime_error("Unexpected grid in the lossless fixture.");
            edit.grid = grid;
            edit.mask = FrameMask{3, candidate.regions};
        }
        const auto profile = std::string_view(argv[3]) == "lossless" ? Profile::lossless : Profile::mp4;
        const bool cancel = std::string_view(argv[4]) == "cancel";
        std::stop_source stop;
        std::int64_t progress = 0;
        try {
            export_video(media, input, index, edit, {output, profile, true, cancel},
                         stop.get_token(), [&](auto written) {
                progress = written;
                if (cancel)
                    stop.request_stop();
            });
        } catch (const std::runtime_error&) {
            if (cancel && stop.stop_requested() && progress == 1)
                return 0;
            throw;
        }
        if (cancel || progress != index.frames)
            throw std::runtime_error("Unexpected export completion.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
