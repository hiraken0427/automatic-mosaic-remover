#pragma once

#include "domain/detection.hpp"

namespace mosaic {

struct TrackRegion {
    Grid grid;
    std::vector<Rect> regions;
};

std::optional<TrackRegion> track_next(const Raster& previous, const Raster& current,
                                      const TrackRegion& region, std::stop_token stop = {});

}
