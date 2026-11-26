#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

namespace mosaic {

struct Raster {
    int width;
    int height;
    int stride;
    std::vector<std::uint8_t> pixels;
};

struct Rect {
    int left, top, right, bottom;

    bool empty() const { return left >= right || top >= bottom; }
    bool operator==(const Rect&) const = default;
};

inline Rect intersect(Rect a, Rect b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top),
            std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}

struct Grid {
    int width, height, phase_x, phase_y;
};

struct FrameMask {
    std::int64_t ordinal;
    std::vector<Rect> regions;
};

struct Edit {
    std::int64_t first, end;
    Rect roi;
    Grid grid;
    std::optional<FrameMask> mask = std::nullopt;
};

void validate(const Edit& edit, const Raster& frame);
void restore(Raster& frame, std::int64_t ordinal, const Edit& edit,
             int iterations, std::stop_token stop = {});

}
