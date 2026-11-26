#pragma once

#include "domain/restoration.hpp"

namespace mosaic {

struct Candidate {
    Grid grid;
    std::vector<Rect> regions;
    float evidence;
};

std::vector<Candidate> detect(const Raster& frame, Rect roi, std::stop_token stop = {});

}
