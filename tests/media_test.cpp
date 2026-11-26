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
        const std::string_view mode = argv[4];
        if (index.frames != 8 || (!index.info.has_audio && mode != "tracked"))
            throw std::runtime_error("Unexpected fixture frame count or audio.");
        Preview preview;
        for (const auto ordinal : {0, 7, 2, 3, 3}) {
            const auto frame = preview.frame(media, input, ordinal, {});
            if (frame.ordinal != ordinal || frame.raster.pixels.empty())
                throw std::runtime_error("Preview returned the wrong frame.");
        }
        Edit edit{2, 6, {5, 4, 17, 16}, {4, 3, -3, 1}};
        std::optional<Track> track;
        if (mode == "tracked") {
            edit = {1, 7, {0, 0, 142, index.info.height}, {8, 12, 4, 11}};
            auto candidates = preview.candidates(media, input, 3, edit.roi, {});
            if (candidates.empty())
                throw std::runtime_error("No tracking seed found.");
            const auto& seed = candidates.front();
            if (seed.grid.width != 8 || seed.grid.height != 12)
                throw std::runtime_error("Unexpected tracking seed grid.");
            edit.grid = seed.grid;
            edit.mask = FrameMask{3, seed.regions};
            track = track_video(media, input, index, edit, {}, [](auto) {});
            if (track->size() != 5 || track->begin()->first != 1 || track->rbegin()->first != 5)
                throw std::runtime_error("Tracking did not follow both directions or stop at the scene cut.");
            auto frame = preview.frame(media, input, 2, {});
            const auto before = frame.raster.pixels;
            restore_frame(frame, edit, &*track, 200, {});
            if (before == frame.raster.pixels)
                throw std::runtime_error("The tracked frame was not applied to the preview.");
        }
        if (mode == "detected") {
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
        const bool cancel = mode == "cancel";
        std::stop_source stop;
        std::int64_t progress = 0;
        try {
            export_video(media, input, index, edit, {output, profile, true, cancel},
                         stop.get_token(), [&](auto written) {
                progress = written;
                if (cancel)
                    stop.request_stop();
            }, track ? &*track : nullptr);
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
