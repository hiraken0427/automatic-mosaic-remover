#include "domain/tracking.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace mosaic {
namespace {

constexpr int radius = 2;
constexpr int displacement = 24;

struct Shift {
    int x, y;
};

Rect bounds(const std::vector<Rect>& regions, const Raster& frame) {
    Rect result{frame.width, frame.height, 0, 0};
    for (auto rect : regions)
        result = {std::min(result.left, rect.left), std::min(result.top, rect.top),
                  std::max(result.right, rect.right), std::max(result.bottom, rect.bottom)};
    return result;
}

Rect translated(Rect rect, Shift shift) {
    return {rect.left + shift.x, rect.top + shift.y, rect.right + shift.x, rect.bottom + shift.y};
}

double area(Rect rect) {
    if (rect.empty())
        return 0;
    return static_cast<double>(rect.right - rect.left) * (rect.bottom - rect.top);
}

int luma(const Raster& frame, int x, int y) {
    const auto* rgb = frame.pixels.data() + static_cast<std::size_t>(y) * frame.stride + 3ULL * x;
    return (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2] + 128) >> 8;
}

bool patch_fits(const Raster& frame, int x, int y) {
    return x >= radius && y >= radius && x < frame.width - radius && y < frame.height - radius;
}

bool close(Shift a, Shift b, int tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

std::optional<Shift> match(const Raster& from, const Raster& to, int x, int y,
                           std::stop_token stop) {
    if (!patch_fits(from, x, y))
        return {};
    constexpr int width = 2 * radius + 1;
    std::array<int, width * width> patch{};
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            patch[(dy + radius) * width + dx + radius] = luma(from, x + dx, y + dy);
    const auto [dark, bright] = std::ranges::minmax(patch);
    if (bright - dark < 24)
        return {};

    constexpr int side = 2 * displacement + 1;
    constexpr int invalid = 40 * width * width + 1;
    std::array<int, side * side> scores;
    scores.fill(invalid);
    Shift best{};
    int best_score = invalid;
    for (int dy = -displacement; dy <= displacement && !stop.stop_requested(); ++dy) {
        for (int dx = -displacement; dx <= displacement; ++dx) {
            if (!patch_fits(to, x + dx, y + dy))
                continue;
            int score = 0;
            for (int row = 0; row < width; ++row)
                for (int col = 0; col < width; ++col)
                    score += std::min(40, std::abs(patch[row * width + col] -
                        luma(to, x + dx + col - radius, y + dy + row - radius)));
            scores[(dy + displacement) * side + dx + displacement] = score;
            if (score < best_score) {
                best = {dx, dy};
                best_score = score;
            }
        }
    }
    if (stop.stop_requested() || best_score > 10 * width * width)
        return {};
    for (int dy = -displacement; dy <= displacement; ++dy)
        for (int dx = -displacement; dx <= displacement; ++dx)
            if (!close(best, {dx, dy}, 1) && scores[(dy + displacement) * side + dx + displacement]
                                               < best_score + 2 * width * width)
                return {};
    return best;
}

std::optional<Shift> estimate_motion(const Raster& previous, const Raster& current,
                                     Rect rect, std::stop_token stop) {
    std::vector<Shift> matches;
    const auto examine = [&](int x, int y) {
        const auto shift = match(previous, current, x, y, stop);
        if (!shift)
            return;
        const auto reverse = match(current, previous, x + shift->x, y + shift->y, stop);
        if (reverse && close(*shift, {-reverse->x, -reverse->y}, 1))
            matches.push_back(*shift);
    };
    constexpr int margin = radius + 4;
    for (int i = 1; i <= 4 && !stop.stop_requested(); ++i) {
        const auto x = rect.left + (rect.right - rect.left) * i / 5;
        const auto y = rect.top + (rect.bottom - rect.top) * i / 5;
        examine(x, rect.top - margin);
        examine(x, rect.bottom + margin - 1);
        examine(rect.left - margin, y);
        examine(rect.right + margin - 1, y);
    }
    Shift best{};
    std::size_t support = 0;
    for (const auto shift : matches) {
        const auto count = static_cast<std::size_t>(std::ranges::count_if(matches, [&](Shift other) {
            return close(shift, other, 2);
        }));
        if (count > support) {
            best = shift;
            support = count;
        }
    }
    if (support < 2 || support * 2 <= matches.size())
        return {};
    return best;
}

std::array<int, 16> histogram(const Raster& frame) {
    std::array<int, 16> result{};
    const int step = std::max(1, std::min(frame.width, frame.height) / 48);
    for (int y = 0; y < frame.height; y += step)
        for (int x = 0; x < frame.width; x += step)
            ++result[luma(frame, x, y) / 16];
    return result;
}

bool scene_change(const Raster& previous, const Raster& current) {
    const auto a = histogram(previous), b = histogram(current);
    int difference = 0, samples = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference += std::abs(a[i] - b[i]);
        samples += a[i];
    }
    return difference > samples * 0.96;
}

std::optional<int> aligned_phase(int period, long long predicted, int observed_period, int observed_phase) {
    if (period == observed_period)
        return observed_phase;
    if (period <= 1 || observed_period <= 1)
        return {};
    // A local window can pick the first harmonic when some seams are faint.
    // Keep the accepted dimensions and use the detected seam to align their phase.
    if (observed_period == 2LL * period)
        return observed_phase % period;
    if (period != 2LL * observed_period)
        return {};
    const auto distance = [&](int phase) {
        const int delta = static_cast<int>(std::abs((predicted - phase) % period));
        return std::min(delta, period - delta);
    };
    const int alternative = observed_phase + observed_period;
    return distance(observed_phase) <= distance(alternative) ? observed_phase : alternative;
}

bool similar_region(const Raster& previous, const Raster& current, Rect old_bounds, Rect new_bounds) {
    int difference = 0;
    constexpr int samples = 8;
    for (int row = 0; row < samples; ++row) {
        for (int col = 0; col < samples; ++col) {
            const auto at = [=](const Raster& frame, Rect rect) {
                return luma(frame, rect.left + (rect.right - rect.left) * (2 * col + 1) / (2 * samples),
                            rect.top + (rect.bottom - rect.top) * (2 * row + 1) / (2 * samples));
            };
            difference += std::abs(at(previous, old_bounds) - at(current, new_bounds));
        }
    }
    return difference <= 24 * samples * samples;
}

}

std::optional<TrackRegion> track_next(const Raster& previous, const Raster& current,
                                      const TrackRegion& region, std::stop_token stop) {
    if (previous.width != current.width || previous.height != current.height || stop.stop_requested())
        return {};
    const Rect image{0, 0, current.width, current.height};
    const auto old = intersect(bounds(region.regions, previous), image);
    if (old.empty())
        return {};
    const auto motion = estimate_motion(previous, current, old, stop);
    if (stop.stop_requested() || (!motion && scene_change(previous, current)))
        return {};
    const auto predicted = intersect(translated(old, motion.value_or(Shift{})), image);
    if (predicted.empty())
        return {};
    const int margin = std::clamp(std::max(region.grid.width, region.grid.height), displacement, 64);
    const auto search = intersect({predicted.left - margin, predicted.top - margin,
                                   predicted.right + margin, predicted.bottom + margin}, image);
    auto candidates = detect(current, search, stop);
    const Candidate* best = nullptr;
    const Candidate* alternative = nullptr;
    Grid best_grid = region.grid;
    double best_score = 0, alternative_score = 0;
    for (const auto& candidate : candidates) {
        const auto observed = bounds(candidate.regions, current);
        const auto phase_x = aligned_phase(region.grid.width, static_cast<long long>(region.grid.phase_x) + observed.left - old.left,
                                           candidate.grid.width, candidate.grid.phase_x);
        const auto phase_y = aligned_phase(region.grid.height, static_cast<long long>(region.grid.phase_y) + observed.top - old.top,
                                           candidate.grid.height, candidate.grid.phase_y);
        if (!phase_x || !phase_y)
            continue;
        const auto overlap = area(intersect(predicted, observed));
        const auto combined = area(predicted) + area(observed) - overlap;
        if (overlap < 0.42 * combined)
            continue;
        if (!motion && !similar_region(previous, current, old, observed))
            continue;
        const bool same_grid = candidate.grid.width == region.grid.width &&
                               candidate.grid.height == region.grid.height;
        const auto score = overlap / combined * (0.7 + 0.3 * candidate.evidence) *
                           (same_grid ? 1.0 : 0.88);
        if (score > best_score) {
            alternative = best;
            alternative_score = best_score;
            best = &candidate;
            best_score = score;
            best_grid.phase_x = *phase_x;
            best_grid.phase_y = *phase_y;
        } else if (score > alternative_score) {
            alternative = &candidate;
            alternative_score = score;
        }
    }
    if (!best || stop.stop_requested())
        return {};
    if (alternative && alternative_score >= 0.9 * best_score) {
        const auto a = bounds(best->regions, current), b = bounds(alternative->regions, current);
        if (area(intersect(a, b)) < 0.6 * std::min(area(a), area(b)))
            return {};
    }
    return TrackRegion{best_grid, best->regions};
}

}
