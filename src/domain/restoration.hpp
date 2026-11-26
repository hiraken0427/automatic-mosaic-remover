#pragma once

#include <cstdint>
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
};

struct Grid {
    int width, height, phase_x, phase_y;
};

struct Edit {
    std::int64_t first, end;
    Rect roi;
    Grid grid;
};

void validate(const Edit& edit, const Raster& frame);
void restore(Raster& frame, std::int64_t ordinal, const Edit& edit,
             int iterations, std::stop_token stop = {});

}
