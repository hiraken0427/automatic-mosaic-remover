#pragma once

#include "application/video.hpp"

namespace mosaic {

class FFmpegMedia final : public Media {
public:
    std::unique_ptr<Reader> open(const std::filesystem::path& path) const override;
    std::unique_ptr<Writer> create(const std::filesystem::path& source,
                                   const ExportOptions& options) const override;
};

}
