#include "domain/detection.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

using namespace mosaic;

template<class Luma>
Raster make_raster(int width, int height, Luma luma) {
    const int stride = width * 3 + 7;
    Raster result{width, height, stride, std::vector<std::uint8_t>(stride * height, 255)};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            std::fill_n(result.pixels.data() + y * stride + x * 3, 3, luma(x, y));
    return result;
}

bool contains(Rect region, int x, int y) {
    return x >= region.left && x < region.right && y >= region.top && y < region.bottom;
}

void detection_tests() {
    const Rect scene{0, 0, 180, 156};
    const Rect mosaic_region{27, 29, 131, 125};
    auto mosaic_frame = make_raster(scene.right, scene.bottom, [&](int x, int y) {
        if (contains(mosaic_region, x, y)) {
            const int col = (x - mosaic_region.left) / 8;
            const int row = (y - mosaic_region.top) / 12;
            return 33 + (col * 61 + row * 41 + col * row * 7) % 174 + (x + y * 7) % 3;
        }
        return (x * 87 + y * 59 + x * y * 13) % 255;
    });
    const auto proposals = detect(mosaic_frame, scene);
    assert(!proposals.empty());
    const auto& candidate = proposals.front();
    assert(candidate.grid.width == 8 && candidate.grid.height == 12);
    assert(candidate.grid.phase_x == 3 && candidate.grid.phase_y == 5);
    int pixels = 0;
    for (const auto region : candidate.regions) {
        assert(intersect(region, mosaic_region) == region);
        pixels += (region.right - region.left) * (region.bottom - region.top);
    }
    assert(pixels > 9 * 104 * 96 / 10);

    const Rect tiny{62, 70, 63, 72};
    const auto precise = detect(mosaic_frame, tiny);
    assert(!precise.empty() && precise.front().regions == std::vector<Rect>{tiny});

    const Rect square_scene{0, 0, 99, 87};
    const auto square = make_raster(square_scene.right, square_scene.bottom, [](int x, int y) {
        return 30 + ((x + 4) / 9 * 61 + (y + 7) / 9 * 47) % 190;
    });
    const auto square_grid = detect(square, square_scene);
    assert(!square_grid.empty());
    assert(square_grid.front().grid.width == 9 && square_grid.front().grid.height == 9);
    assert(square_grid.front().grid.phase_x == 5 && square_grid.front().grid.phase_y == 2);

    const auto brickwork = make_raster(scene.right, scene.bottom, [](int x, int y) {
        const bool mortar = y % 10 < 2 || (x + (y / 10 % 2) * 10) % 20 < 2;
        return mortar ? 48 : 120 + (x * 13 + y * 7) % 48;
    });
    const auto flat = make_raster(scene.right, scene.bottom, [](int, int) { return 90; });
    assert(detect(brickwork, scene).empty());
    assert(detect(flat, scene).empty());

    std::stop_source stop;
    stop.request_stop();
    assert(detect(mosaic_frame, scene, stop.get_token()).empty());

    const Rect roi{35, 40, 100, 93};
    const std::vector<Rect> islands{{32, 34, 60, 60}, {80, 65, 120, 110}};
    const Edit masked{1, 4, roi, candidate.grid, FrameMask{2, islands}};
    auto restored = mosaic_frame;
    restore(restored, 1, masked, 80);
    restore(restored, 3, masked, 80);
    assert(restored.pixels == mosaic_frame.pixels);
    restore(restored, 2, masked, 80);
    int changed = 0;
    for (int y = 0; y < restored.height; ++y) {
        for (int byte = 0; byte < restored.stride; ++byte) {
            const int x = byte / 3;
            const auto i = y * restored.stride + byte;
            if (contains(roi, x, y) &&
                (contains(islands[0], x, y) || contains(islands[1], x, y)))
                changed += restored.pixels[i] != mosaic_frame.pixels[i];
            else
                assert(restored.pixels[i] == mosaic_frame.pixels[i]);
        }
    }
    assert(changed > 0);
    std::cout << "Mosaic detection, periodic texture rejection, and exact mask bounds passed.\n";
}

}

int main() {
    using namespace mosaic;
    detection_tests();
    constexpr int width = 15, height = 9, stride = width * 3 + 5;
    Raster source{width, height, stride, std::vector<std::uint8_t>(height * stride, 231)};
    Raster clean = source;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                const auto position = y * stride + x * 3 + c;
                clean.pixels[position] = 10 * x + 5 * y + c * 8;
                source.pixels[position] = x >= 1 && x < 13 && y >= 2 && y < 8
                    ? 10 * (1 + (x - 1) / 4 * 4) + 5 * (2 + (y - 2) / 3 * 3) + c * 8 + 20
                    : clean.pixels[position];
            }

    const Edit edit{11, 18, {1, 2, 13, 8}, {4, 3, -3, -1}};
    auto result = source;
    restore(result, 10, edit, 200);
    restore(result, 18, edit, 200);
    assert(result.pixels == source.pixels);

    for (const auto ordinal : {11, 17}) {
        result = source;
        restore(result, ordinal, edit, 200);
        int before = 0, after = 0;
        for (int y = 0; y < height; ++y)
            for (int byte = 0; byte < stride; ++byte) {
                const auto i = y * stride + byte;
                if (y >= 2 && y < 8 && byte >= 3 && byte < 39) {
                    before += std::abs(source.pixels[i] - clean.pixels[i]);
                    after += std::abs(result.pixels[i] - clean.pixels[i]);
                } else
                    assert(result.pixels[i] == source.pixels[i]);
            }
        assert(after < before);
    }

    result = source;
    const Edit one_pixel{0, 1, {1, 2, 2, 3}, {4, 3, 1, 2}};
    restore(result, 0, one_pixel, 200);
    for (std::size_t i = 0; i < result.pixels.size(); ++i)
        if (i < 2 * stride + 3 || i >= 2 * stride + 6)
            assert(result.pixels[i] == source.pixels[i]);

    std::stop_source stop;
    stop.request_stop();
    result = source;
    restore(result, 11, edit, 200, stop.get_token());
    assert(result.pixels == source.pixels);

    bool rejected = false;
    try {
        validate({0, 1, {0, 0, width, height}, {1, 1, 0, 0}}, source);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    std::cout << "Restoration, frame bounds, pixel bounds, and cancellation passed.\n";
}
