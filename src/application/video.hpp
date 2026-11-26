#pragma once

#include "domain/restoration.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace mosaic {

struct VideoInfo {
    int width;
    int height;
    int time_base_num;
    int time_base_den;
    double pixel_aspect;
    int quarter_turns;
    bool has_audio;
};

struct Frame {
    Raster raster;
    std::int64_t ordinal;
    std::int64_t pts;
    std::int64_t duration;
};

enum class Profile { mp4, lossless };

struct ExportOptions {
    std::filesystem::path destination;
    Profile profile;
    bool copy_audio;
    bool overwrite;
};

class Reader {
public:
    virtual ~Reader() = default;
    virtual VideoInfo info() const = 0;
    virtual std::optional<Frame> next(bool pixels = true) = 0;
};

class Writer {
public:
    virtual ~Writer() = default;
    virtual void write(const Frame& frame, std::stop_token stop) = 0;
    virtual void finish(std::stop_token stop) = 0;
};

class Media {
public:
    virtual ~Media() = default;
    virtual std::unique_ptr<Reader> open(const std::filesystem::path& path) const = 0;
    virtual std::unique_ptr<Writer> create(const std::filesystem::path& source,
                                           const ExportOptions& options) const = 0;
};

struct VideoIndex {
    VideoInfo info;
    std::int64_t frames;
};

using Progress = std::function<void(std::int64_t)>;
VideoIndex index_video(const Media& media, const std::filesystem::path& source,
                       std::stop_token stop, const Progress& progress);

class Preview {
public:
    Frame frame(const Media& media, const std::filesystem::path& source,
                std::int64_t ordinal, std::stop_token stop);
private:
    std::filesystem::path source_;
    std::unique_ptr<Reader> reader_;
    std::optional<Frame> frame_;
};

void export_video(const Media& media, const std::filesystem::path& source,
                  const VideoIndex& index, const Edit& edit, const ExportOptions& options,
                  std::stop_token stop, const Progress& progress);
void check_cancelled(std::stop_token stop);

}
