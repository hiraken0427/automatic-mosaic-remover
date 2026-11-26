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

std::vector<Candidate> Preview::candidates(const Media& media, const std::filesystem::path& source,
                                           std::int64_t ordinal, Rect roi, std::stop_token stop) {
    return detect(frame(media, source, ordinal, stop).raster, roi, stop);
}

namespace {

std::unique_ptr<Reader> open_at(const Media& media, const std::filesystem::path& source,
                                std::int64_t ordinal, std::stop_token stop) {
    check_cancelled(stop);
    auto reader = media.open(source);
    for (std::int64_t i = 0; i < ordinal; ++i) {
        check_cancelled(stop);
        if (!reader->next(false))
            throw std::runtime_error("The source frame count changed. Reopen the video.");
    }
    return reader;
}

Frame read_frame(Reader& reader, std::int64_t ordinal, std::stop_token stop) {
    check_cancelled(stop);
    auto frame = reader.next();
    if (!frame || frame->ordinal != ordinal)
        throw std::runtime_error("The source frame count changed. Reopen the video.");
    return std::move(*frame);
}

}

Track track_video(const Media& media, const std::filesystem::path& source,
                  const VideoIndex& index, const Edit& edit, std::stop_token stop,
                  const Progress& progress) {
    validate(edit, {index.info.width, index.info.height, 0, {}});
    if (edit.end > index.frames || !edit.mask || edit.mask->ordinal < edit.first ||
        edit.mask->ordinal >= edit.end)
        throw std::invalid_argument("Include the accepted frame in the selected range.");

    TrackRegion initial{edit.grid, {}};
    for (const auto region : edit.mask->regions) {
        const auto clipped = intersect(region, edit.roi);
        if (!clipped.empty())
            initial.regions.push_back(clipped);
    }
    if (initial.regions.empty())
        throw std::invalid_argument("Include the accepted mask in the ROI.");

    const auto seed_ordinal = edit.mask->ordinal;
    auto reader = open_at(media, source, seed_ordinal, stop);
    const auto seed = read_frame(*reader, seed_ordinal, stop);
    if (seed.raster.width != index.info.width || seed.raster.height != index.info.height)
        throw std::runtime_error("The source dimensions changed. Reopen the video.");
    Track result;
    result.emplace(seed_ordinal, std::move(initial));
    progress(1);
    const auto advance = [&](const Frame& from, const Frame& to) {
        auto region = track_next(from.raster, to.raster, result.at(from.ordinal), stop);
        check_cancelled(stop);
        if (!region)
            return false;
        result.emplace(to.ordinal, std::move(*region));
        progress(static_cast<std::int64_t>(result.size()));
        return true;
    };

    if (seed_ordinal + 1 < edit.end) {
        std::optional<Frame> previous;
        for (auto ordinal = seed_ordinal + 1; ordinal < edit.end; ++ordinal) {
            auto frame = read_frame(*reader, ordinal, stop);
            if (!advance(previous ? *previous : seed, frame))
                break;
            previous = std::move(frame);
        }
    }
    reader.reset();

    // Decode backward-tracking batches in presentation order and reuse their buffers.
    constexpr std::size_t cache_bytes = 64 * 1024 * 1024;
    const auto batch_size = static_cast<std::int64_t>(
        std::clamp<std::size_t>(cache_bytes / std::max<std::size_t>(1, seed.raster.pixels.size()), 1, 128));
    std::optional<Frame> previous;
    for (auto end = seed_ordinal; end > edit.first;) {
        const auto first = std::max(edit.first, end - batch_size);
        auto batch_reader = open_at(media, source, first, stop);
        std::vector<Frame> batch;
        batch.reserve(static_cast<std::size_t>(end - first));
        for (auto ordinal = first; ordinal < end; ++ordinal)
            batch.push_back(read_frame(*batch_reader, ordinal, stop));

        for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
            if (!advance(previous ? *previous : seed, *it))
                return result;
            previous = std::move(*it);
        }
        end = first;
    }
    check_cancelled(stop);
    return result;
}

void restore_frame(Frame& frame, const Edit& edit, const Track* track,
                   int iterations, std::stop_token stop) {
    if (!track) {
        restore(frame.raster, frame.ordinal, edit, iterations, stop);
        return;
    }
    const auto found = track->find(frame.ordinal);
    if (found != track->end() && frame.ordinal >= edit.first && frame.ordinal < edit.end) {
        const auto& region = found->second;
        const Edit tracked{edit.first, edit.end, edit.roi, region.grid,
                           FrameMask{frame.ordinal, region.regions}};
        restore(frame.raster, frame.ordinal, tracked, iterations, stop);
    }
}

void export_video(const Media& media, const std::filesystem::path& source,
                  const VideoIndex& index, const Edit& edit, const ExportOptions& options,
                  std::stop_token stop, const Progress& progress, const Track* track) {
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
        restore_frame(*current, edit, track, 200, stop);
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
