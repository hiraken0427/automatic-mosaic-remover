#include "domain/restoration.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mosaic {
namespace {

int phase(int value, int period) {
    const int remainder = value % period;
    return remainder < 0 ? remainder + period : remainder;
}

std::int64_t cell_start(int pixel, int offset, int period) {
    return static_cast<std::int64_t>(pixel) - phase(pixel - phase(offset, period), period);
}

struct Cell {
    int samples = 0;
    int editable = 0;
    float observed = 0;
    float scale = 0;
    float dual = 0;
    float sum = 0;
};

struct Sample {
    float value = 0;
    float extrapolated = 0;
    float horizontal = 0;
    float vertical = 0;
    std::size_t cell = 0;
    bool editable = false;
};

std::size_t byte_index(const Raster& frame, int x, int y, int channel) {
    return static_cast<std::size_t>(y) * frame.stride + 3ULL * x + channel;
}

// The combined squared norm is at most eight for differences plus one for block sums.
constexpr float step = 0.30F;
constexpr float regularization = 0.025F;

}

void validate(const Edit& edit, const Raster& frame) {
    const auto [left, top, right, bottom] = edit.roi;
    const auto grid = edit.grid;
    if (edit.first < 0 || edit.end <= edit.first)
        throw std::invalid_argument("Select a valid frame range.");
    if (left < 0 || top < 0 || right > frame.width || bottom > frame.height ||
        left >= right || top >= bottom)
        throw std::invalid_argument("Select a nonempty region inside the video.");
    if (grid.width < 1 || grid.height < 1 || (grid.width == 1 && grid.height == 1))
        throw std::invalid_argument("Block sizes must be positive, with one dimension above one.");
}

void restore(Raster& frame, std::int64_t ordinal, const Edit& edit,
             int iterations, std::stop_token stop) {
    validate(edit, frame);
    if (ordinal < edit.first || ordinal >= edit.end || iterations <= 0 || stop.stop_requested())
        return;

    const auto [left, top, right, bottom] = edit.roi;
    const auto grid = edit.grid;
    const auto grid_left = cell_start(left, grid.phase_x, grid.width);
    const auto grid_top = cell_start(top, grid.phase_y, grid.height);
    const auto grid_right = cell_start(right - 1, grid.phase_x, grid.width) + grid.width;
    const auto grid_bottom = cell_start(bottom - 1, grid.phase_y, grid.height) + grid.height;
    const int x0 = static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(left - 1, grid_left)));
    const int y0 = static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(top - 1, grid_top)));
    const int x1 = static_cast<int>(std::min<std::int64_t>(frame.width, std::max<std::int64_t>(right + 1LL, grid_right)));
    const int y1 = static_cast<int>(std::min<std::int64_t>(frame.height, std::max<std::int64_t>(bottom + 1LL, grid_bottom)));
    const int width = x1 - x0, height = y1 - y0;
    const auto columns = (grid_right - grid_left) / grid.width;
    const auto rows = (grid_bottom - grid_top) / grid.height;
    const auto cell_count = static_cast<std::size_t>(columns * rows);
    std::vector<Sample> samples(static_cast<std::size_t>(width) * height);
    std::vector<Cell> cells(cell_count + 1);
    std::vector<std::uint8_t> restored(static_cast<std::size_t>(right - left) * (bottom - top) * 3);
    const auto roi_index = [=](int x, int y, int channel) {
        return (static_cast<std::size_t>(y - top) * (right - left) + x - left) * 3 + channel;
    };

    for (int channel = 0; channel < 3; ++channel) {
        std::fill(cells.begin(), cells.end(), Cell{});
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                auto& pixel = samples[static_cast<std::size_t>(y - y0) * width + x - x0];
                pixel = {};
                pixel.value = frame.pixels[byte_index(frame, x, y, channel)] / 255.0F;
                pixel.extrapolated = pixel.value;
                pixel.editable = x >= left && x < right && y >= top && y < bottom;
                pixel.cell = cell_count;
                if (x >= grid_left && x < grid_right && y >= grid_top && y < grid_bottom) {
                    pixel.cell = static_cast<std::size_t>(((y - grid_top) / grid.height) * columns
                                                         + (x - grid_left) / grid.width);
                    auto& cell = cells[pixel.cell];
                    ++cell.samples;
                    cell.editable += pixel.editable;
                    cell.observed += pixel.value;
                }
            }
        }
        for (auto& cell : cells) {
            // Normalize the averaging operator and downweight partially edited cells.
            if (cell.editable)
                cell.scale = std::sqrt(static_cast<float>(cell.editable)) / cell.samples;
            cell.observed *= cell.scale;
        }

        int stable = 0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            if (stop.stop_requested())
                return;
            for (auto& cell : cells)
                cell.sum = 0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto i = static_cast<std::size_t>(y) * width + x;
                    auto& pixel = samples[i];
                    cells[pixel.cell].sum += pixel.extrapolated;
                    const float dx = x + 1 < width ? samples[i + 1].extrapolated - pixel.extrapolated : 0;
                    const float dy = y + 1 < height ? samples[i + width].extrapolated - pixel.extrapolated : 0;
                    pixel.horizontal += step * dx;
                    pixel.vertical += step * dy;
                    const float norm = std::sqrt(pixel.horizontal * pixel.horizontal +
                                                 pixel.vertical * pixel.vertical);
                    const float divisor = std::max(1.0F, norm / regularization);
                    pixel.horizontal /= divisor;
                    pixel.vertical /= divisor;
                }
            }
            for (auto& cell : cells)
                cell.dual = (cell.dual + step * (cell.sum * cell.scale - cell.observed)) / (1 + step);
            float max_change = 0;
            for (int y = top - y0; y < bottom - y0; ++y) {
                for (int x = left - x0; x < right - x0; ++x) {
                    const auto i = static_cast<std::size_t>(y) * width + x;
                    auto& pixel = samples[i];
                    const auto& cell = cells[pixel.cell];
                    const float divergence = pixel.horizontal + pixel.vertical
                        - (x ? samples[i - 1].horizontal : 0) - (y ? samples[i - width].vertical : 0);
                    const float next = std::clamp(pixel.value + step * (divergence - cell.scale * cell.dual),
                                                  0.0F, 1.0F);
                    const float change = next - pixel.value;
                    max_change = std::max(max_change, std::abs(change));
                    pixel.extrapolated = next + change;
                    pixel.value = next;
                }
            }
            stable = max_change < 1e-4F ? stable + 1 : 0;
            if (stable == 5)
                break;
        }

        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x)
                restored[roi_index(x, y, channel)] = static_cast<std::uint8_t>(
                    std::lround(255 * samples[static_cast<std::size_t>(y - y0) * width + x - x0].value));
    }

    if (stop.stop_requested())
        return;
    for (int y = top; y < bottom; ++y)
        std::copy_n(restored.data() + roi_index(left, y, 0), (right - left) * 3,
                    frame.pixels.data() + byte_index(frame, left, y, 0));
}

}
