#include "application/video.hpp"

#include <stdexcept>

namespace mosaic {

void check_cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Cancelled.");
}

VideoIndex index_video(const Media& media, const std::filesystem::path& source,
                       std::stop_token stop, const Progress& progress) {
    check_cancelled(stop);
    auto reader = media.open(source);
    VideoIndex index{reader->info(), 0};
    while (reader->next(false)) {
        check_cancelled(stop);
        progress(++index.frames);
    }
    if (!index.frames)
        throw std::runtime_error("The video has no readable frames.");
    return index;
}

Frame Preview::frame(const Media& media, const std::filesystem::path& source,
                     std::int64_t ordinal, std::stop_token stop) {
    if (ordinal < 0)
        throw std::invalid_argument("Select a valid frame.");
    if (!reader_ || source != source_ || !frame_ || ordinal < frame_->ordinal ||
        (ordinal == frame_->ordinal && frame_->raster.pixels.empty())) {
        reader_ = media.open(source);
        source_ = source;
        frame_.reset();
    }
    while (!frame_ || frame_->ordinal < ordinal) {
        check_cancelled(stop);
        frame_ = reader_->next(ordinal == (frame_ ? frame_->ordinal + 1 : 0));
        if (!frame_)
            throw std::runtime_error("The selected frame is unavailable.");
    }
    return *frame_;
}

void export_video(const Media& media, const std::filesystem::path& source,
                  const VideoIndex& index, const Edit& edit, const ExportOptions& options,
                  std::stop_token stop, const Progress& progress) {
    validate(edit, {index.info.width, index.info.height, 0, {}});
    if (edit.end > index.frames)
        throw std::invalid_argument("The selected range extends past the end of the video.");

    auto reader = media.open(source);
    auto current = reader->next();
    if (!current)
        throw std::runtime_error("The source video has no readable frames.");
    auto writer = media.create(source, options);
    std::int64_t written = 0;
    while (current) {
        check_cancelled(stop);
        auto next = reader->next();
        if (next)
            current->duration = next->pts - current->pts;
        restore(current->raster, current->ordinal, edit, 200, stop);
        check_cancelled(stop);
        writer->write(*current, stop);
        progress(++written);
        current = std::move(next);
    }
    if (written != index.frames)
        throw std::runtime_error("The source frame count changed. Reopen the video.");
    writer->finish(stop);
}

}
