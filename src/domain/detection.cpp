#include "domain/detection.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
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
        energy += std::max(0.0F, edge - 0.75F);
    }
    if (energy < 5)
        return {};

    Axis best;
    for (int period = 2; period <= max_period && period < length; ++period) {
        std::array<float, max_period> contrast{}, edge_energy{};
        std::array<int, max_period> count{}, peaks{};
        for (int i = 0; i < length; ++i) {
            const int phase = i % period;
            contrast[phase] += lines[i];
            edge_energy[phase] += std::max(0.0F, lines[i] - 0.75F);
            ++count[phase];
            peaks[phase] += lines[i] >= 2.5F;
        }
        for (int phase = 0; phase < period; ++phase) {
            const int boundaries = count[phase];
            if (boundaries < 2 || peaks[phase] < 2 || peaks[phase] * 2 < boundaries)
                continue;
            const float boundary_mean = contrast[phase] / boundaries;
            float interior_sum = total - contrast[phase];
            int interior_count = length - boundaries;
            float aligned_energy = edge_energy[phase];
            if (period >= 5) {
                // Allow compression to spread edge energy to the pixels beside a grid line.
                const int before = (phase + period - 1) % period, after = (phase + 1) % period;
                if (boundary_mean < std::max(contrast[before] / count[before],
                                             contrast[after] / count[after]))
                    continue;
                interior_sum -= contrast[before] + contrast[after];
                interior_count -= count[before] + count[after];
                aligned_energy += 0.65F * (edge_energy[before] + edge_energy[after]);
            }
            const float interior_mean = std::max(0.0F, interior_sum) / interior_count;
            const float dominance = (boundary_mean - interior_mean) / (boundary_mean + interior_mean + 1);
            const float coverage = aligned_energy / energy;
            if (dominance < 0.36F || coverage < 0.50F)
                continue;
            const float score = 0.6F * dominance + 0.4F * coverage;
            if (score > best.score + 0.01F)
                best = {period, (start + phase) % period, score};
        }
    }
    return best;
}

struct CellEvidence {
    float inside_x, inside_y, edge_x, edge_y;
};

CellEvidence measure_cell(const GradientSums& sums, Rect cell, Grid grid) {
    // Keep the inner-texture measurement clear of spill from the two bordering edges.
    const int trim_x = cell.right - cell.left >= 6;
    const int trim_y = cell.bottom - cell.top >= 6;
    const Rect inside{cell.left + trim_x, cell.top + trim_y,
                      cell.right - trim_x, cell.bottom - trim_y};
    const float width = inside.right - inside.left, height = inside.bottom - inside.top;
    const float inside_x = width > 1 ? sums.sum({inside.left + 1, inside.top, inside.right, inside.bottom}).x
                                        / ((width - 1) * height) : 0;
    const float inside_y = height > 1 ? sums.sum({inside.left, inside.top + 1, inside.right, inside.bottom}).y
                                         / (width * (height - 1)) : 0;
    const auto scope = sums.scope();
    const auto x_line = [&](int x) {
        return x > scope.left && x < scope.right
            ? sums.sum({x, inside.top, x + 1, inside.bottom}).x / height : 0.0F;
    };
    const auto y_line = [&](int y) {
        return y > scope.top && y < scope.bottom
            ? sums.sum({inside.left, y, inside.right, y + 1}).y / width : 0.0F;
    };
    const auto strongest = [](auto line, int first, int last, int period) {
        float edge = std::max(line(first), line(last));
        if (period >= 5) {
            // Use the inner neighboring lines, so exterior texture cannot strengthen a border.
            edge = std::max(edge, 0.8F * std::max(line(first + 1), line(last - 1)));
        }
        return edge;
    };
    return {inside_x, inside_y, strongest(x_line, cell.left, cell.right, grid.width),
            strongest(y_line, cell.top, cell.bottom, grid.height)};
}

bool smooth_cell(CellEvidence cell) {
    constexpr float texture_limit = 7;
    return cell.inside_x <= texture_limit && cell.inside_y <= texture_limit;
}

bool supports_edge(float edge, float inside, float floor, float ratio) {
    return edge >= std::max(floor, ratio * inside + 0.5F);
}

bool supports_grid(CellEvidence cell, Grid grid, float floor, float ratio) {
    return smooth_cell(cell) &&
           (grid.width == 1 || supports_edge(cell.edge_x, cell.inside_x, floor, ratio)) &&
           (grid.height == 1 || supports_edge(cell.edge_y, cell.inside_y, floor, ratio));
}

using GridKey = std::array<int, 4>;
using Position = std::pair<int, int>;
using Cells = std::map<Position, float>;

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
            if (observed.empty() || area(observed) * 2 < area(cell) ||
                !supports_grid(measure_cell(sums, cell, grid), grid, 3.0F, 2.0F))
                continue;
            auto& evidence = cells[{top, left}];
            evidence = std::max(evidence, std::min(x.score, y.score));
        }
    }
}

Rect grid_cell(Rect scope, Grid grid, Position position) {
    const auto [y, x] = position;
    return intersect({x, y, x + grid.width, y + grid.height}, scope);
}

// A low-contrast band can lose its cross-grid seams. Follow the remaining
// seams into that band only if they recur in neighboring cells along it.
bool supports_band(const GradientSums& sums, Grid grid, Position position,
                   CellEvidence evidence, bool vertical_step) {
    constexpr float floor = 1.25F, ratio = 1.1F;
    const auto aligned = [&](CellEvidence cell) {
        return smooth_cell(cell) && (vertical_step
            ? supports_edge(cell.edge_x, cell.inside_x, floor, ratio)
            : supports_edge(cell.edge_y, cell.inside_y, floor, ratio));
    };
    if (!aligned(evidence))
        return false;

    const auto [y, x] = position;
    const int dx = vertical_step ? grid.width : 0;
    const int dy = vertical_step ? 0 : grid.height;
    int support = 1;
    for (const int direction : {-1, 1}) {
        for (int offset = 1; offset <= 2; ++offset) {
            const auto cell = grid_cell(sums.scope(), grid,
                                        {y + direction * offset * dy, x + direction * offset * dx});
            if (cell.empty() || !aligned(measure_cell(sums, cell, grid)))
                break;
            if (++support == 3)
                return true;
        }
    }
    return false;
}

std::set<Position> bridge_short_gaps(const GradientSums& sums, Grid grid,
                                     const std::set<Position>& supported, std::stop_token stop) {
    std::set<Position> filled;
    for (const auto& [y, x] : supported) {
        if (stop.stop_requested())
            break;
        for (const auto& [dy, dx] : {Position{grid.height, 0}, Position{0, grid.width}}) {
            for (int gap = 1; gap <= 2; ++gap) {
                const Position missing{y + gap * dy, x + gap * dx};
                if (supported.contains(missing))
                    break;
                const auto cell = grid_cell(sums.scope(), grid, missing);
                if (cell.empty())
                    break;
                const auto evidence = measure_cell(sums, cell, grid);
                if (evidence.inside_x > 3 || evidence.inside_y > 3)
                    break;
                if (supported.contains({missing.first + dy, missing.second + dx})) {
                    for (int step = 1; step <= gap; ++step)
                        filled.emplace(y + step * dy, x + step * dx);
                    break;
                }
            }
        }
    }
    return filled;
}

void collect(const GradientSums& sums, Grid grid, Cells& cells, Rect roi, std::vector<Candidate>& candidates,
             std::stop_token stop) {
    // A weak edge can extend an existing two-dimensional grid but cannot start one.
    if (cells.size() < 4)
        return;
    const bool grow = grid.width > 1 && grid.height > 1;
    const auto adjacent = [&](Position position) {
        const auto [y, x] = position;
        return std::array{Position{y, x - grid.width}, Position{y, x + grid.width},
                          Position{y - grid.height, x}, Position{y + grid.height, x}};
    };
    std::set<Position> visited;
    while (!cells.empty() && !stop.stop_requested()) {
        std::vector<Position> pending{cells.begin()->first};
        Candidate candidate{grid, {}, 0};
        float score = cells.begin()->second;
        const float weak_score = std::min(0.55F, 0.85F * score);
        int seed_count = 1;
        visited.insert(cells.begin()->first);
        cells.erase(cells.begin());
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (stop.stop_requested())
                return;
            const auto position = pending[i];
            for (auto neighbor : adjacent(position)) {
                if (visited.contains(neighbor))
                    continue;
                if (auto found = cells.find(neighbor); found != cells.end()) {
                    pending.push_back(neighbor);
                    visited.insert(neighbor);
                    score += found->second;
                    ++seed_count;
                    cells.erase(found);
                    continue;
                }
                if (!grow)
                    continue;
                const auto cell = grid_cell(sums.scope(), grid, neighbor);
                if (cell.empty())
                    continue;
                const auto evidence = measure_cell(sums, cell, grid);
                if (supports_grid(evidence, grid, 1.25F, 1.1F) ||
                    supports_band(sums, grid, neighbor, evidence, position.first != neighbor.first)) {
                    pending.push_back(neighbor);
                    visited.insert(neighbor);
                    score += weak_score;
                }
            }
        }
        if (pending.size() < 4 || seed_count < 2)
            continue;

        std::set<Position> component(pending.begin(), pending.end());
        if (grow) {
            // Similar neighboring blocks can lose their shared edge. Only fill short
            // smooth gaps bracketed by cells within this already connected component.
            auto filled = bridge_short_gaps(sums, grid, component, stop);
            if (stop.stop_requested())
                return;
            score += 0.73F * weak_score * filled.size();
            visited.insert(filled.begin(), filled.end());
            component.merge(filled);
        }
        auto& runs = candidate.regions;
        for (const auto& [y, x] : component) {
            const Rect region = intersect({x, y, x + grid.width, y + grid.height}, roi);
            if (region.empty())
                continue;
            if (!runs.empty() && runs.back().top == region.top && runs.back().right == region.left)
                runs.back().right = region.right;
            else
                runs.push_back(region);
        }
        if (runs.empty())
            continue;
        candidate.evidence = score / component.size();
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
        collect(sums, {key[0], key[1], key[2], key[3]}, cells, roi, candidates, stop);
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
