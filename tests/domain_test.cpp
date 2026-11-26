#include "domain/restoration.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace mosaic;
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
