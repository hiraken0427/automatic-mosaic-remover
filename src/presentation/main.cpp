#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

#include "infrastructure/ffmpeg_media.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

using namespace mosaic;
namespace fs = std::filesystem;

template<class T>
T* sdl(T* pointer) {
    if (!pointer)
        throw std::runtime_error(SDL_GetError());
    return pointer;
}

struct SDLSession {
    SDLSession() {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            throw std::runtime_error(SDL_GetError());
    }
    ~SDLSession() { SDL_Quit(); }
};

struct GUIContext {
    GUIContext() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
    }
    ~GUIContext() { ImGui::DestroyContext(); }
};

struct GUIBackends {
    GUIBackends(SDL_Window* window, SDL_Renderer* renderer) {
        if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer))
            throw std::runtime_error("Could not start the SDL input backend.");
        if (!ImGui_ImplSDLRenderer2_Init(renderer)) {
            ImGui_ImplSDL2_Shutdown();
            throw std::runtime_error("Could not start the SDL renderer backend.");
        }
    }
    ~GUIBackends() {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
    }
};

using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

Texture upload(SDL_Renderer* renderer, const Raster& raster) {
    Texture result(sdl(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STATIC,
                                        raster.width, raster.height)), SDL_DestroyTexture);
    if (SDL_UpdateTexture(result.get(), nullptr, raster.pixels.data(), raster.stride) != 0)
        throw std::runtime_error(SDL_GetError());
    return result;
}

fs::path path_from_input(const char* value) {
    const auto* utf8 = reinterpret_cast<const char8_t*>(value);
    return fs::path(std::u8string(utf8));
}

enum class Job { open, original, detect, restored, export_video };

struct Result {
    std::optional<VideoIndex> index;
    std::optional<Frame> frame;
    std::string error;
    std::vector<Candidate> candidates{};
};

ImVec2 orient(ImVec2 point, int turns) {
    for (int i = 0; i < turns; ++i)
        point = {point.y, 1 - point.x};
    return point;
}

class Editor {
public:
    explicit Editor(SDL_Renderer* renderer) : renderer_(renderer) {}

    void open(const fs::path& source) {
        start(Job::open, [this, source](std::stop_token stop) {
            auto index = index_video(media_, source, stop, [this](auto count) { progress_ = count; });
            preview_ = Preview{};
            auto frame = preview_.frame(media_, source, 0, stop);
            return Result{index, std::move(frame), {}};
        });
        pending_source_ = source;
        const auto name = source.u8string();
        if (name.size() < input_.size()) {
            std::copy_n(reinterpret_cast<const char*>(name.data()), name.size(), input_.data());
            input_[name.size()] = '\0';
        }
    }

    void draw() {
        collect();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Mosaic Remover", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings);
        const bool running = busy();
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(-125);
        ImGui::InputTextWithHint("##input", "Video path or drop a video here", input_.data(), input_.size());
        ImGui::SameLine();
        if (ImGui::Button("Open video", {106, 0}))
            request([this] { open(path_from_input(input_.data())); });
        if (index_)
            controls();
        ImGui::EndDisabled();
        if (running) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                worker_.request_stop();
        }

        if (running && job_ == Job::export_video)
            ImGui::ProgressBar(static_cast<float>(progress_.load()) / index_->frames);
        else if (running && job_ == Job::open)
            ImGui::Text("Indexing video (%lld frames)", static_cast<long long>(progress_.load()));
        else if (running && job_ == Job::detect)
            ImGui::TextUnformatted("Detecting mosaic in this frame...");
        else if (running)
            ImGui::TextUnformatted("Preparing preview...");
        else if (!status_.empty())
            ImGui::TextWrapped("%s", status_.c_str());

        if (original_) {
            const auto available = ImGui::GetContentRegionAvail();
            if (available.x > 0 && available.y > 0)
                draw_preview(available, running);
        }
        ImGui::End();
    }

    bool busy() const { return running_.load() || worker_.joinable(); }

private:
    template<class Function>
    void request(Function work) {
        try {
            work();
        } catch (const std::exception& error) {
            status_ = error.what();
        }
    }

    template<class Function>
    void start(Job job, Function work) {
        if (busy())
            return;
        status_.clear();
        job_ = job;
        progress_ = 0;
        running_ = true;
        try {
            worker_ = std::jthread([this, work = std::move(work)](std::stop_token stop) {
                result_ = {};
                try {
                    result_ = work(stop);
                    if (job_ != Job::export_video)
                        check_cancelled(stop);
                } catch (const std::exception& error) {
                    result_ = {{}, {}, error.what()};
                }
                running_ = false;
            });
        } catch (...) {
            running_ = false;
            throw;
        }
    }

    void collect() {
        if (running_ || !worker_.joinable())
            return;
        worker_.join();
        if (!result_.error.empty()) {
            status_ = std::move(result_.error);
            requested_ = shown_;
            return;
        }
        request([this] {
            if (result_.frame) {
                auto texture = upload(renderer_, result_.frame->raster);
                if (job_ == Job::restored) {
                    restored_ = std::move(texture);
                    show_restored_ = true;
                } else {
                    original_ = std::move(texture);
                    clear_preview();
                    candidates_.clear();
                    shown_ = result_.frame->ordinal + 1;
                    requested_ = shown_;
                    shown_pts_ = result_.frame->pts;
                }
            }
            if (result_.index) {
                index_ = *result_.index;
                source_ = std::move(pending_source_);
                roi_ = {0, 0, index_->info.width, index_->info.height};
                first_ = 1;
                last_ = index_->frames;
                copy_audio_ = index_->info.has_audio;
                accepted_.reset();
            }
            if (job_ == Job::detect) {
                candidates_ = std::move(result_.candidates);
                selected_candidate_ = 0;
                if (candidates_.empty())
                    status_ = "No mosaic candidate found in this region.";
            }
            if (job_ == Job::export_video)
                status_ = "Export complete.";
        });
        result_ = {};
    }

    Edit edit() const {
        if (first_ < 1 || last_ > index_->frames)
            throw std::invalid_argument("Select frames within the video.");
        Edit value{first_ - 1, last_, roi_, grid_, accepted_};
        if (value.mask && (value.mask->ordinal < value.first || value.mask->ordinal >= value.end))
            throw std::invalid_argument("Include the accepted frame in the selected range.");
        validate(value, {index_->info.width, index_->info.height, 0, {}});
        return value;
    }

    void load_frame() {
        requested_ = std::clamp<std::int64_t>(requested_, 1, index_->frames);
        start(Job::original, [this, source = source_, ordinal = requested_ - 1](std::stop_token stop) {
            return Result{{}, preview_.frame(media_, source, ordinal, stop), {}};
        });
    }

    void controls() {
        const std::int64_t one = 1;
        ImGui::SetNextItemWidth(-120);
        ImGui::SliderScalar("##frame", ImGuiDataType_S64, &requested_, &one, &index_->frames);
        if (ImGui::IsItemDeactivatedAfterEdit() && requested_ != shown_)
            request([this] { load_frame(); });
        ImGui::SameLine();
        ImGui::Text("Frame %lld", static_cast<long long>(shown_));
        if (ImGui::Button("Previous") && shown_ > 1) {
            requested_ = shown_ - 1;
            request([this] { load_frame(); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Next") && shown_ < index_->frames) {
            requested_ = shown_ + 1;
            request([this] { load_frame(); });
        }
        ImGui::SameLine();
        ImGui::Text("%.3f s", static_cast<double>(shown_pts_) * index_->info.time_base_num /
                             index_->info.time_base_den);
        ImGui::SetNextItemWidth(110);
        bool changed = ImGui::InputScalar("First frame", ImGuiDataType_S64, &first_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        changed |= ImGui::InputScalar("Last frame", ImGuiDataType_S64, &last_);
        int roi[] = {roi_.left, roi_.top, roi_.right, roi_.bottom};
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputInt4("ROI edges (L, T, R, B)", roi)) {
            set_roi({roi[0], roi[1], roi[2], roi[3]});
        }
        ImGui::SameLine();
        if (ImGui::Button("Whole frame"))
            set_roi({0, 0, index_->info.width, index_->info.height});
        int grid[] = {grid_.width, grid_.height, grid_.phase_x, grid_.phase_y};
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputInt4("Grid (W, H, X, Y)", grid)) {
            grid_ = {grid[0], grid[1], grid[2], grid[3]};
            changed = true;
        }
        if (changed)
            clear_preview();

        detection_controls();

        if (ImGui::Button("Preview restoration"))
            request([this] {
                const auto plan = edit();
                start(Job::restored, [this, source = source_, ordinal = shown_ - 1, plan](std::stop_token stop) {
                    auto frame = preview_.frame(media_, source, ordinal, stop);
                    restore(frame.raster, ordinal, plan, 60, stop);
                    return Result{{}, std::move(frame), {}};
                });
            });
        ImGui::SameLine();
        ImGui::BeginDisabled(!restored_);
        ImGui::Checkbox("Show restored", &show_restored_);
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(-100);
        ImGui::InputTextWithHint("##output", "Output video path", output_.data(), output_.size());
        ImGui::SameLine();
        if (ImGui::Button("Export", {81, 0}))
            request([this] {
                const auto plan = edit();
                const auto destination = path_from_input(output_.data());
                if (destination.empty())
                    throw std::invalid_argument("Enter an output video path.");
                ExportOptions options{destination, lossless_ ? Profile::lossless : Profile::mp4,
                                      copy_audio_, false};
                if (fs::exists(destination)) {
                    overwrite_ = options;
                    overwrite_edit_ = plan;
                    ImGui::OpenPopup("Replace output?");
                } else
                    save(plan, options);
            });
        ImGui::Checkbox("Lossless FFV1 (MKV)", &lossless_);
        ImGui::SameLine();
        ImGui::BeginDisabled(!index_->info.has_audio);
        ImGui::Checkbox("Copy audio", &copy_audio_);
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Replace output?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("An output file already exists. Replace it?");
            if (ImGui::Button("Replace")) {
                overwrite_.overwrite = true;
                request([this] { save(overwrite_edit_, overwrite_); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep existing"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void detection_controls() {
        if (ImGui::Button("Detect mosaic"))
            request([this] {
                start(Job::detect, [this, source = source_, ordinal = shown_ - 1,
                                    roi = roi_](std::stop_token stop) {
                    Result result;
                    result.candidates = preview_.candidates(media_, source, ordinal, roi, stop);
                    return result;
                });
                candidates_.clear();
                show_restored_ = false;
            });
        if (accepted_) {
            ImGui::SameLine();
            ImGui::Text("Mask applied to frame %lld", static_cast<long long>(accepted_->ordinal + 1));
            ImGui::SameLine();
            if (ImGui::Button("Use manual ROI")) {
                accepted_.reset();
                clear_preview();
            }
        }
        if (candidates_.empty())
            return;

        std::array<char, 100> label{};
        const auto candidate_label = [&](std::size_t i) {
            const auto& candidate = candidates_[i];
            const auto grid = candidate.grid;
            std::snprintf(label.data(), label.size(), "%zu: %d x %d, offset %d, %d, evidence %.2f",
                          i + 1, grid.width, grid.height, grid.phase_x, grid.phase_y, candidate.evidence);
            return label.data();
        };
        ImGui::SetNextItemWidth(335);
        if (ImGui::BeginCombo("##candidate", candidate_label(selected_candidate_))) {
            for (std::size_t i = 0; i < candidates_.size(); ++i)
                if (ImGui::Selectable(candidate_label(i), i == selected_candidate_))
                    selected_candidate_ = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use candidate")) {
            request([this] {
                if (shown_ < first_ || shown_ > last_)
                    throw std::invalid_argument("Include this frame in the selected range.");
                auto& candidate = candidates_[selected_candidate_];
                grid_ = candidate.grid;
                accepted_ = FrameMask{shown_ - 1, std::move(candidate.regions)};
                candidates_.clear();
                clear_preview();
                status_.clear();
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Dismiss candidate") && !candidates_.empty()) {
            candidates_.erase(candidates_.begin() + selected_candidate_);
            if (selected_candidate_ && selected_candidate_ >= candidates_.size())
                --selected_candidate_;
        }
    }

    void save(Edit plan, ExportOptions options) {
        start(Job::export_video, [this, plan, options = std::move(options),
                                  source = source_, index = *index_](std::stop_token stop) {
            export_video(media_, source, index, plan, options, stop,
                         [this](auto count) { progress_ = count; });
            return Result{};
        });
    }

    void set_roi(Rect roi) {
        if (roi_ == roi)
            return;
        roi_ = roi;
        candidates_.clear();
        clear_preview();
    }

    void clear_preview() {
        restored_.reset();
        show_restored_ = false;
    }

    void draw_preview(ImVec2 available, bool running) {
        const auto info = index_->info;
        ImVec2 size{static_cast<float>(info.width * info.pixel_aspect), static_cast<float>(info.height)};
        if (info.quarter_turns % 2)
            std::swap(size.x, size.y);
        const float scale = std::min(available.x / size.x, available.y / size.y);
        size = {size.x * scale, size.y * scale};
        const auto cursor = ImGui::GetCursorScreenPos();
        const ImVec2 origin{cursor.x + (available.x - size.x) * 0.5F, cursor.y};
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("Draw region", size);
        const auto position = [&](float x, float y) {
            const auto p = orient({x / info.width, y / info.height}, info.quarter_turns);
            return ImVec2{origin.x + p.x * size.x, origin.y + p.y * size.y};
        };
        const auto pixel = [&](ImVec2 point) {
            const auto p = orient({(point.x - origin.x) / size.x, (point.y - origin.y) / size.y},
                                  (4 - info.quarter_turns) % 4);
            return std::array{static_cast<int>(std::clamp(p.x * info.width, 0.0F, float(info.width - 1))),
                              static_cast<int>(std::clamp(p.y * info.height, 0.0F, float(info.height - 1)))};
        };
        if (!running && ImGui::IsItemActivated())
            drag_ = pixel(ImGui::GetIO().MousePos);
        if (drag_) {
            const auto current = pixel(ImGui::GetIO().MousePos);
            set_roi({std::min((*drag_)[0], current[0]), std::min((*drag_)[1], current[1]),
                     std::max((*drag_)[0], current[0]) + 1, std::max((*drag_)[1], current[1]) + 1});
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                drag_.reset();
        }
        auto* draw = ImGui::GetWindowDrawList();
        auto* texture = show_restored_ && restored_ ? restored_.get() : original_.get();
        draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture), position(0, 0), position(info.width, 0),
                           position(info.width, info.height), position(0, info.height));
        const auto corners = [&](Rect rect) {
            const auto a = position(rect.left, rect.top);
            const auto b = position(rect.right, rect.bottom);
            return std::array<ImVec2, 2>{{{std::min(a.x, b.x), std::min(a.y, b.y)},
                                          {std::max(a.x, b.x), std::max(a.y, b.y)}}};
        };
        if (!show_restored_) {
            const auto fill_mask = [&](const std::vector<Rect>& regions, ImU32 color) {
                for (auto region : regions) {
                    region = intersect(region, roi_);
                    if (!region.empty()) {
                        const auto [a, b] = corners(region);
                        draw->AddRectFilled(a, b, color);
                    }
                }
            };
            if (!candidates_.empty())
                fill_mask(candidates_[selected_candidate_].regions, IM_COL32(255, 177, 62, 105));
            else if (accepted_ && accepted_->ordinal == shown_ - 1)
                fill_mask(accepted_->regions, IM_COL32(60, 220, 190, 90));
        }
        const auto clipped = intersect(roi_, {0, 0, info.width, info.height});
        if (!clipped.empty()) {
            const auto [a, b] = corners(clipped);
            draw->AddRect(a, b, IM_COL32(60, 220, 190, 255));
        }
    }

    SDL_Renderer* renderer_;
    FFmpegMedia media_;
    Preview preview_;
    Job job_ = Job::open;
    std::atomic<bool> running_ = false;
    std::atomic<std::int64_t> progress_ = 0;
    Result result_;
    std::optional<VideoIndex> index_;
    fs::path source_, pending_source_;
    std::array<char, 4096> input_{}, output_{};
    std::string status_;
    std::int64_t first_ = 1, last_ = 1, shown_ = 1, requested_ = 1, shown_pts_ = 0;
    Rect roi_{};
    Grid grid_{8, 8, 0, 0};
    std::optional<FrameMask> accepted_;
    std::vector<Candidate> candidates_;
    std::size_t selected_candidate_ = 0;
    Texture original_{nullptr, SDL_DestroyTexture}, restored_{nullptr, SDL_DestroyTexture};
    bool show_restored_ = false, lossless_ = false, copy_audio_ = false;
    std::optional<std::array<int, 2>> drag_;
    Edit overwrite_edit_{};
    ExportOptions overwrite_{};
    std::jthread worker_;
};

int run(int argc, char** argv) {
    SDLSession sdl_session;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        sdl(SDL_CreateWindow("Mosaic Remover", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1050, 750, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)), SDL_DestroyWindow);
    SDL_SetWindowMinimumSize(window.get(), 650, 410);
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        sdl(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_PRESENTVSYNC)), SDL_DestroyRenderer);
    GUIContext context;
    GUIBackends backends(window.get(), renderer.get());
    Editor editor(renderer.get());
    if (argc > 1)
        editor.open(path_from_input(argv[1]));

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_DROPFILE) {
                std::unique_ptr<char, decltype(&SDL_free)> path(event.drop.file, SDL_free);
                if (!editor.busy())
                    editor.open(path_from_input(path.get()));
            }
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        editor.draw();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer.get(), 16, 17, 21, 255);
        SDL_RenderClear(renderer.get());
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(renderer.get());
    }
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mosaic Remover", error.what(), nullptr);
        return 1;
    }
}
