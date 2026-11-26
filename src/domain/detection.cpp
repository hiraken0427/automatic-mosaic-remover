#include "domain/detection.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <utility>

namespace mosaic {
namespace {

constexpr int max_period = 64;
constexpr int minimum_window = 64;
constexpr std::size_t max_candidates = 12;

long long area(Rect r) {
    return static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
}

struct Differences {
    long long x = 0, y = 0;
    Differences operator+(Differences b) const { return {x + b.x, y + b.y}; }
    Differences operator-(Differences b) const { return {x - b.x, y - b.y}; }
};

int luma(const std::uint8_t* pixel) {
    return (54 * pixel[0] + 183 * pixel[1] + 19 * pixel[2] + 128) >> 8;
}

class GradientSums {
public:
    GradientSums(const Raster& frame, Rect scope, std::stop_token stop)
        : scope_(scope), stride_(static_cast<std::size_t>(scope.right - scope.left) + 1),
          sums_(stride_ * (scope.bottom - scope.top + 1ULL)) {
        for (int y = scope.top; y < scope.bottom && !stop.stop_requested(); ++y) {
            Differences row;
            for (int x = scope.left; x < scope.right; ++x) {
                const auto* p = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + 3ULL * x;
                const int center = luma(p);
                if (x > 0)
                    row.x += std::abs(center - luma(p - 3));
                if (y > 0)
                    row.y += std::abs(center - luma(p - frame.stride));
                sums_[index(x + 1, y + 1)] = sums_[index(x + 1, y)] + row;
            }
        }
    }

    Differences sum(Rect r) const {
        return sums_[index(r.right, r.bottom)] - sums_[index(r.left, r.bottom)] -
               sums_[index(r.right, r.top)] + sums_[index(r.left, r.top)];
    }

    Rect scope() const { return scope_; }

private:
    std::size_t index(int x, int y) const {
        return static_cast<std::size_t>(y - scope_.top) * stride_ + (x - scope_.left);
    }

    Rect scope_;
    std::size_t stride_;
    std::vector<Differences> sums_;
};

struct Axis {
    int period = 0, phase = 0;
    float score = 0;
};

Axis detect_axis(const GradientSums& sums, Rect window, bool horizontal) {
    const int start = (horizontal ? window.left : window.top) + 1;
    const int end = horizontal ? window.right : window.bottom;
    const int length = end - start;
    const float across = horizontal ? window.bottom - window.top : window.right - window.left;
    if (length < 4)
        return {};
    std::vector<float> lines(static_cast<std::size_t>(length));
    float total = 0, energy = 0;
    for (int p = start; p < end; ++p) {
        const float edge = horizontal ? sums.sum({p, window.top, p + 1, window.bottom}).x / across
                                      : sums.sum({window.left, p, window.right, p + 1}).y / across;
        lines[p - start] = edge;
        total += edge;
        energy += std::max(0.0F, edge - 2);
    }
    if (energy < 10)
        return {};

    Axis best;
    for (int period = 2; period <= max_period && period < length; ++period) {
        std::array<float, max_period> contrast{}, edge_energy{};
        std::array<int, max_period> count{}, peaks{};
        for (int i = 0; i < length; ++i) {
            const int phase = i % period;
            contrast[phase] += lines[i];
            edge_energy[phase] += std::max(0.0F, lines[i] - 2);
            ++count[phase];
            peaks[phase] += lines[i] >= 5;
        }
        for (int phase = 0; phase < period; ++phase) {
            const int boundaries = count[phase];
            if (boundaries < 2 || peaks[phase] * 3 < boundaries * 2)
                continue;
            const float boundary_mean = contrast[phase] / boundaries;
            const float interior_mean = (total - contrast[phase]) / (length - boundaries);
            const float dominance = (boundary_mean - interior_mean) / (boundary_mean + interior_mean + 2);
            const float coverage = edge_energy[phase] / energy;
            if (dominance < 0.48F || coverage < 0.55F)
                continue;
            const float score = 0.6F * dominance + 0.4F * coverage;
            if (score > best.score + 0.01F)
                best = {period, (start + phase) % period, score};
        }
    }
    return best;
}

bool supports_cell(const GradientSums& sums, Rect cell, Grid grid) {
    const float width = cell.right - cell.left, height = cell.bottom - cell.top;
    const auto inner = sums.sum({cell.left + 1, cell.top + 1, cell.right, cell.bottom});
    const float inside_x = width > 1 ? (inner.x + sums.sum({cell.left + 1, cell.top, cell.right, cell.top + 1}).x)
                                      / ((width - 1) * height) : 0;
    const float inside_y = height > 1 ? (inner.y + sums.sum({cell.left, cell.top + 1, cell.left + 1, cell.bottom}).y)
                                        / (width * (height - 1)) : 0;
    const auto scope = sums.scope();
    const auto x_edge = [&](int x) {
        return x > scope.left && x < scope.right &&
               sums.sum({x, cell.top, x + 1, cell.bottom}).x / height >= std::max(4.0F, 3 * inside_x);
    };
    const auto y_edge = [&](int y) {
        return y > scope.top && y < scope.bottom &&
               sums.sum({cell.left, y, cell.right, y + 1}).y / width >= std::max(4.0F, 3 * inside_y);
    };
    return inside_x <= 5 && inside_y <= 5 &&
           (grid.width == 1 || x_edge(cell.left) || x_edge(cell.right)) &&
           (grid.height == 1 || y_edge(cell.top) || y_edge(cell.bottom));
}

using GridKey = std::array<int, 4>;
using Cells = std::map<std::pair<int, int>, float>;

void examine(const GradientSums& sums, Rect window, std::map<GridKey, Cells>& groups) {
    auto x = detect_axis(sums, window, true);
    auto y = detect_axis(sums, window, false);
    if (!x.period && y.score > 0.8F)
        x = {1, 0, y.score - 0.12F};
    if (!y.period && x.score > 0.8F)
        y = {1, 0, x.score - 0.12F};
    if (!x.period || !y.period)
        return;
    const Grid grid{x.period, y.period, x.phase, y.phase};
    auto& cells = groups[{grid.width, grid.height, grid.phase_x, grid.phase_y}];
    const auto scope = sums.scope();
    const int first_x = window.left - (window.left + x.period - x.phase) % x.period;
    const int first_y = window.top - (window.top + y.period - y.phase) % y.period;
    for (int top = first_y; top < window.bottom; top += y.period) {
        for (int left = first_x; left < window.right; left += x.period) {
            const Rect cell = intersect({left, top, left + x.period, top + y.period}, scope);
            const Rect observed = intersect(cell, window);
            if (observed.empty() || area(observed) * 2 < area(cell) || !supports_cell(sums, cell, grid))
                continue;
            auto& evidence = cells[{top, left}];
            evidence = std::max(evidence, std::min(x.score, y.score));
        }
    }
}

void collect(Grid grid, Cells& cells, Rect roi, std::vector<Candidate>& candidates,
             std::stop_token stop) {
    while (!cells.empty() && !stop.stop_requested()) {
        std::vector<std::pair<int, int>> pending{cells.begin()->first};
        Candidate candidate{grid, {}, 0};
        float score = cells.begin()->second;
        cells.erase(cells.begin());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (stop.stop_requested())
                return;
            const auto [y, x] = pending[i];
            const Rect region = intersect({x, y, x + grid.width, y + grid.height}, roi);
            if (!region.empty())
                candidate.regions.push_back(region);
            for (auto neighbor : {std::pair{y, x - grid.width}, std::pair{y, x + grid.width},
                                  std::pair{y - grid.height, x}, std::pair{y + grid.height, x}}) {
                if (auto found = cells.find(neighbor); found != cells.end()) {
                    pending.push_back(neighbor);
                    score += found->second;
                    cells.erase(found);
                }
            }
        }
        if (pending.size() < 4 || candidate.regions.empty())
            continue;
        candidate.evidence = score / pending.size();
        std::ranges::sort(candidate.regions, {}, [](Rect r) { return std::pair{r.top, r.left}; });
        std::vector<Rect> runs;
        for (Rect region : candidate.regions) {
            if (!runs.empty() && runs.back().top == region.top && runs.back().right == region.left)
                runs.back().right = region.right;
            else
                runs.push_back(region);
        }
        candidate.regions = std::move(runs);
        candidates.push_back(std::move(candidate));
    }
}

}

std::vector<Candidate> detect(const Raster& frame, Rect roi, std::stop_token stop) {
    if (roi.empty() || intersect(roi, {0, 0, frame.width, frame.height}) != roi)
        throw std::invalid_argument("Select a nonempty region inside the video.");
    if (stop.stop_requested())
        return {};

    // Read a margin around a small selection so repeated boundaries can be measured.
    // Only pixels within the selected ROI can be returned in a mask.
    const int margin_x = std::max(0, minimum_window - (roi.right - roi.left)) / 2;
    const int margin_y = std::max(0, minimum_window - (roi.bottom - roi.top)) / 2;
    const Rect scope = intersect({roi.left - margin_x, roi.top - margin_y,
                                  roi.right + margin_x, roi.bottom + margin_y},
                                 {0, 0, frame.width, frame.height});
    const GradientSums sums(frame, scope, stop);
    if (stop.stop_requested())
        return {};
    std::map<GridKey, Cells> groups;
    if (roi != scope && roi.right - roi.left <= 256 && roi.bottom - roi.top <= 256)
        examine(sums, roi, groups);
    for (const int size : {32, 64, 128, 256}) {
        const int width = std::min(size, scope.right - scope.left);
        const int height = std::min(size, scope.bottom - scope.top);
        for (int top = scope.top;; top = std::min(top + height / 2, scope.bottom - height)) {
            for (int left = scope.left;; left = std::min(left + width / 2, scope.right - width)) {
                if (stop.stop_requested())
                    return {};
                examine(sums, {left, top, left + width, top + height}, groups);
                if (left + width == scope.right)
                    break;
            }
            if (top + height == scope.bottom)
                break;
        }
        if (width == scope.right - scope.left && height == scope.bottom - scope.top)
            break;
    }
    std::vector<Candidate> candidates;
    for (auto& [key, cells] : groups)
        collect({key[0], key[1], key[2], key[3]}, cells, roi, candidates, stop);
    if (stop.stop_requested())
        return {};

    const auto rank = [](const Candidate& candidate) {
        long long pixels = 0;
        for (const auto region : candidate.regions)
            pixels += area(region);
        return candidate.evidence * std::sqrt(static_cast<double>(pixels));
    };
    std::ranges::sort(candidates, std::greater{}, rank);
    if (candidates.size() > max_candidates)
        candidates.resize(max_candidates);
    return candidates;
}

}
