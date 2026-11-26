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

enum class Job { open, original, restored, export_video };

struct Result {
    std::optional<VideoIndex> index;
    std::optional<Frame> frame;
    std::string error;
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
                    restored_.reset();
                    show_restored_ = false;
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
            }
            if (job_ == Job::export_video)
                status_ = "Export complete.";
        });
        result_ = {};
    }

    Edit edit() const {
        if (first_ < 1 || last_ > index_->frames)
            throw std::invalid_argument("Select frames within the video.");
        Edit value{first_ - 1, last_, roi_, grid_};
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
            roi_ = {roi[0], roi[1], roi[2], roi[3]};
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Whole frame")) {
            roi_ = {0, 0, index_->info.width, index_->info.height};
            changed = true;
        }
        int grid[] = {grid_.width, grid_.height, grid_.phase_x, grid_.phase_y};
        ImGui::SetNextItemWidth(360);
        if (ImGui::InputInt4("Grid (W, H, X, Y)", grid)) {
            grid_ = {grid[0], grid[1], grid[2], grid[3]};
            changed = true;
        }
        if (changed)
            clear_preview();

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

    void save(Edit plan, ExportOptions options) {
        start(Job::export_video, [this, plan, options = std::move(options),
                                  source = source_, index = *index_](std::stop_token stop) {
            export_video(media_, source, index, plan, options, stop,
                         [this](auto count) { progress_ = count; });
            return Result{};
        });
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
            roi_ = {std::min((*drag_)[0], current[0]), std::min((*drag_)[1], current[1]),
                    std::max((*drag_)[0], current[0]) + 1, std::max((*drag_)[1], current[1]) + 1};
            clear_preview();
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                drag_.reset();
        }
        auto* draw = ImGui::GetWindowDrawList();
        auto* texture = show_restored_ && restored_ ? restored_.get() : original_.get();
        draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture), position(0, 0), position(info.width, 0),
                           position(info.width, info.height), position(0, info.height));
        const auto a = position(static_cast<float>(std::clamp(roi_.left, 0, info.width)),
                                static_cast<float>(std::clamp(roi_.top, 0, info.height)));
        const auto b = position(static_cast<float>(std::clamp(roi_.right, 0, info.width)),
                                static_cast<float>(std::clamp(roi_.bottom, 0, info.height)));
        draw->AddRect({std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)},
                      IM_COL32(60, 220, 190, 255));
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
