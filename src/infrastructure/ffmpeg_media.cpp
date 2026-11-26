#include "infrastructure/ffmpeg_media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mosaic {
namespace {

void check(int result, const char* operation) {
    if (result >= 0)
        return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof message);
    throw std::runtime_error(std::string(operation) + ": " + message);
}

template<class T>
T* allocated(T* pointer) {
    if (!pointer)
        throw std::bad_alloc{};
    return pointer;
}

template<class T, void (*Free)(T**)>
struct Deleter {
    void operator()(T* pointer) const { Free(&pointer); }
};

using Input = std::unique_ptr<AVFormatContext, Deleter<AVFormatContext, avformat_close_input>>;
using Codec = std::unique_ptr<AVCodecContext, Deleter<AVCodecContext, avcodec_free_context>>;
using Image = std::unique_ptr<AVFrame, Deleter<AVFrame, av_frame_free>>;
using Packet = std::unique_ptr<AVPacket, Deleter<AVPacket, av_packet_free>>;
using Scaler = std::unique_ptr<SwsContext, decltype(&sws_freeContext)>;

struct OutputDeleter {
    void operator()(AVFormatContext* output) const {
        if (output->pb)
            avio_closep(&output->pb);
        avformat_free_context(output);
    }
};

std::string path_string(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

Input open_input(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path))
        throw std::invalid_argument("Choose an existing local video file.");
    const auto absolute = path_string(std::filesystem::absolute(path));
    AVFormatContext* raw = nullptr;
    AVDictionary* options = nullptr;
    check(av_dict_set(&options, "protocol_whitelist", "file", 0), "Set input protocol");
    const int result = avformat_open_input(&raw, absolute.c_str(), nullptr, &options);
    av_dict_free(&options);
    check(result, "Open video");
    Input input(raw);
    check(avformat_find_stream_info(raw, nullptr), "Read video streams");
    return input;
}

int video_stream(AVFormatContext* input) {
    const int stream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(stream, "Find video stream");
    return stream;
}

int audio_stream(AVFormatContext* input, int video) {
    return av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0);
}

const AVPacketSideData* display_matrix(const AVStream& stream) {
    return av_packet_side_data_get(stream.codecpar->coded_side_data,
                                   stream.codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
}

int color_matrix(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    default: return SWS_CS_DEFAULT;
    }
}

class FFmpegReader final : public Reader {
public:
    explicit FFmpegReader(const std::filesystem::path& path)
        : input_(open_input(path)), stream_(video_stream(input_.get())) {
        const auto* params = input_->streams[stream_]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(params->codec_id);
        if (!codec)
            throw std::runtime_error("A decoder is unavailable for this video.");
        decoder_.reset(allocated(avcodec_alloc_context3(codec)));
        check(avcodec_parameters_to_context(decoder_.get(), params), "Configure video decoder");
        decoder_->pkt_timebase = input_->streams[stream_]->time_base;
        check(avcodec_open2(decoder_.get(), codec, nullptr), "Start video decoder");
        if (decoder_->width <= 0 || decoder_->height <= 0 ||
            decoder_->width > std::numeric_limits<int>::max() / 3)
            throw std::runtime_error("The video has invalid raster dimensions.");
    }

    VideoInfo info() const override {
        auto& stream = *input_->streams[stream_];
        const auto aspect = av_guess_sample_aspect_ratio(input_.get(), &stream, nullptr);
        int turns = 0;
        if (const auto* matrix = display_matrix(stream); matrix && matrix->size == 9 * sizeof(std::int32_t)) {
            const auto* values = reinterpret_cast<const std::int32_t*>(matrix->data);
            const double angle = av_display_rotation_get(values);
            if (std::isfinite(angle))
                turns = (static_cast<int>(std::lround(angle / 90)) % 4 + 4) % 4;
        }
        return {decoder_->width, decoder_->height, stream.time_base.num, stream.time_base.den,
                aspect.num > 0 && aspect.den > 0 ? av_q2d(aspect) : 1.0, turns,
                audio_stream(input_.get(), stream_) >= 0};
    }

    std::optional<Frame> next(bool pixels) override {
        while (true) {
            av_frame_unref(decoded_.get());
            const int result = avcodec_receive_frame(decoder_.get(), decoded_.get());
            if (result == AVERROR_EOF)
                return {};
            if (result != AVERROR(EAGAIN)) {
                check(result, "Decode video frame");
                return frame(pixels);
            }
            int read;
            do {
                av_packet_unref(packet_.get());
                read = av_read_frame(input_.get(), packet_.get());
            } while (read >= 0 && packet_->stream_index != stream_);
            if (read != AVERROR_EOF)
                check(read, "Read video packet");
            check(avcodec_send_packet(decoder_.get(), read < 0 ? nullptr : packet_.get()),
                  "Send video packet");
        }
    }

private:
    Frame frame(bool pixels) {
        const auto& image = *decoded_;
        const auto& stream = *input_->streams[stream_];
        if (image.width != decoder_->width || image.height != decoder_->height ||
            (ordinal_ > 0 && (image.width != width_ || image.height != height_)))
            throw std::runtime_error("A resolution change is unsupported.");
        width_ = image.width;
        height_ = image.height;
        if (image.flags & AV_FRAME_FLAG_INTERLACED || image.color_trc == AVCOL_TRC_SMPTE2084 ||
            image.color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw std::runtime_error("Choose a progressive SDR video.");
        const auto rate = av_guess_frame_rate(input_.get(), input_->streams[stream_], nullptr);
        const auto fallback = rate.num > 0 && rate.den > 0
            ? std::max<std::int64_t>(1, av_rescale_q(1, av_inv_q(rate), stream.time_base)) : 1;
        const auto duration = image.duration > 0 ? image.duration : fallback;
        auto pts = image.best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = ordinal_ ? previous_pts_ + previous_duration_
                           : (stream.start_time == AV_NOPTS_VALUE ? 0 : stream.start_time);
        if (ordinal_ && pts <= previous_pts_)
            throw std::runtime_error("The video contains non-increasing display timestamps.");
        previous_pts_ = pts;
        previous_duration_ = duration;
        Frame result{{image.width, image.height, image.width * 3, {}}, ordinal_++, pts, duration};
        if (!pixels)
            return result;

        auto& raster = result.raster;
        raster.pixels.resize(static_cast<std::size_t>(raster.stride) * raster.height + AV_INPUT_BUFFER_PADDING_SIZE);
        const auto format = static_cast<AVPixelFormat>(image.format);
        scaler_.reset(allocated(sws_getCachedContext(scaler_.release(), image.width, image.height, format,
                                                     image.width, image.height, AV_PIX_FMT_RGB24,
                                                     SWS_BILINEAR, nullptr, nullptr, nullptr)));
        if (!(av_pix_fmt_desc_get(format)->flags & AV_PIX_FMT_FLAG_RGB)) {
            const auto* coefficients = sws_getCoefficients(color_matrix(image.colorspace));
            check(sws_setColorspaceDetails(scaler_.get(), coefficients, image.color_range == AVCOL_RANGE_JPEG,
                                           coefficients, 1, 0, 1 << 16, 1 << 16), "Set input color space");
        }
        std::uint8_t* destination[AV_NUM_DATA_POINTERS] = {raster.pixels.data()};
        const int strides[AV_NUM_DATA_POINTERS] = {raster.stride};
        check(sws_scale(scaler_.get(), image.data, image.linesize, 0, image.height, destination, strides),
              "Convert frame to RGB");
        return result;
    }

    Input input_;
    int stream_;
    Codec decoder_;
    Image decoded_{allocated(av_frame_alloc())};
    Packet packet_{allocated(av_packet_alloc())};
    Scaler scaler_{nullptr, sws_freeContext};
    int width_ = 0, height_ = 0;
    std::int64_t ordinal_ = 0, previous_pts_ = 0, previous_duration_ = 0;
};

class TemporaryOutput {
public:
    explicit TemporaryOutput(const std::filesystem::path& destination) {
        if (destination.filename().empty())
            throw std::invalid_argument("Enter an output file name.");
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = destination.parent_path() /
                (".mosaic-export-" + std::to_string(nonce) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Could not reserve a temporary output file.");
    }

    ~TemporaryOutput() {
        std::error_code ignored;
        std::filesystem::remove(file(), ignored);
        std::filesystem::remove(directory_, ignored);
    }

    std::filesystem::path file() const { return directory_ / "video"; }

private:
    std::filesystem::path directory_;
};

void publish(const std::filesystem::path& temporary, const ExportOptions& options) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), options.destination.c_str(),
                     options.overwrite ? MOVEFILE_REPLACE_EXISTING : 0))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Publish video");
#else
    if (options.overwrite)
        std::filesystem::rename(temporary, options.destination);
    else
        std::filesystem::create_hard_link(temporary, options.destination);
#endif
}

class FFmpegWriter final : public Writer {
public:
    FFmpegWriter(const std::filesystem::path& source, const ExportOptions& options)
        : options_(options), input_(open_input(source)), temporary_(options.destination) {
        if (std::filesystem::exists(options.destination)) {
            if (std::filesystem::equivalent(source, options.destination))
                throw std::invalid_argument("Choose an output different from the input video.");
            if (!options.overwrite)
                throw std::invalid_argument("The output already exists. Confirm replacement or choose another path.");
        }

        auto& video = *input_->streams[video_stream(input_.get())];
        const bool lossless = options.profile == Profile::lossless;
        AVFormatContext* raw = nullptr;
        const auto path = path_string(std::filesystem::absolute(temporary_.file()));
        const int result = avformat_alloc_output_context2(&raw, nullptr, lossless ? "matroska" : "mp4",
                                                          path.c_str());
        output_.reset(raw);
        check(result, "Create output container");
        const AVCodec* codec = lossless ? avcodec_find_encoder(AV_CODEC_ID_FFV1)
                                       : avcodec_find_encoder_by_name("libx264");
        if (!codec)
            throw std::runtime_error(lossless ? "The FFV1 encoder is unavailable."
                                              : "The libx264 encoder is unavailable. Try lossless export.");
        encoder_.reset(allocated(avcodec_alloc_context3(codec)));
        auto& encoder = *encoder_;
        encoder.width = video.codecpar->width;
        encoder.height = video.codecpar->height;
        encoder.time_base = video.time_base;
        encoder.framerate = av_guess_frame_rate(input_.get(), &video, nullptr);
        encoder.pix_fmt = lossless ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_YUV420P;
        if (!lossless && (encoder.width % 2 || encoder.height % 2))
            throw std::runtime_error("MP4 requires even video dimensions. Choose lossless export.");
        encoder.sample_aspect_ratio = av_guess_sample_aspect_ratio(input_.get(), &video, nullptr);
        encoder.color_primaries = video.codecpar->color_primaries;
        encoder.color_trc = video.codecpar->color_trc;
        encoder.colorspace = lossless ? AVCOL_SPC_RGB : video.codecpar->color_space;
        if (!lossless && (encoder.colorspace == AVCOL_SPC_UNSPECIFIED || encoder.colorspace == AVCOL_SPC_RGB))
            encoder.colorspace = AVCOL_SPC_SMPTE170M;
        encoder.color_range = lossless ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        encoder.max_b_frames = 0;
        encoder.flags |= AV_CODEC_FLAG_FRAME_DURATION;
        if (output_->oformat->flags & AVFMT_GLOBALHEADER)
            encoder.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(avcodec_open2(&encoder, codec, nullptr), "Start video encoder");

        video_ = allocated(avformat_new_stream(output_.get(), nullptr));
        video_->time_base = encoder.time_base;
        video_->sample_aspect_ratio = encoder.sample_aspect_ratio;
        check(avcodec_parameters_from_context(video_->codecpar, &encoder), "Set output video parameters");
        if (const auto* matrix = display_matrix(video)) {
            if (lossless)
                throw std::runtime_error("Lossless output cannot preserve this video's display rotation. Choose MP4.");
            auto* side = allocated(av_packet_side_data_new(&video_->codecpar->coded_side_data,
                &video_->codecpar->nb_coded_side_data, matrix->type, matrix->size, 0));
            std::memcpy(side->data, matrix->data, matrix->size);
        }

        audio_index_ = options.copy_audio ? audio_stream(input_.get(), video.index) : -1;
        if (audio_index_ >= 0) {
            const auto& audio = *input_->streams[audio_index_];
            if (avformat_query_codec(output_->oformat, audio.codecpar->codec_id, FF_COMPLIANCE_NORMAL) <= 0)
                throw std::runtime_error("This output cannot copy the audio codec. Choose another format or disable Copy audio.");
            audio_ = allocated(avformat_new_stream(output_.get(), nullptr));
            check(avcodec_parameters_copy(audio_->codecpar, audio.codecpar), "Copy audio stream parameters");
            audio_->codecpar->codec_tag = 0;
            audio_->time_base = audio.time_base;
        }

        check(avio_open(&output_->pb, path.c_str(), AVIO_FLAG_WRITE), "Open temporary video");
        check(avformat_write_header(output_.get(), nullptr), "Write video header");
        image_->format = encoder.pix_fmt;
        image_->width = encoder.width;
        image_->height = encoder.height;
        check(av_frame_get_buffer(image_.get(), 0), "Allocate output image");
        scaler_.reset(allocated(sws_getContext(encoder.width, encoder.height, AV_PIX_FMT_RGB24,
                                              encoder.width, encoder.height, encoder.pix_fmt,
                                              SWS_BILINEAR, nullptr, nullptr, nullptr)));
        if (!lossless) {
            const auto* coefficients = sws_getCoefficients(color_matrix(encoder.colorspace));
            check(sws_setColorspaceDetails(scaler_.get(), coefficients, 1, coefficients, 0,
                                           0, 1 << 16, 1 << 16), "Set output color space");
        }
    }

    void write(const Frame& frame, std::stop_token stop) override {
        const auto& raster = frame.raster;
        if (raster.width != image_->width || raster.height != image_->height)
            throw std::runtime_error("The output frame has unexpected dimensions.");
        check(av_frame_make_writable(image_.get()), "Prepare output frame");
        const std::uint8_t* data[AV_NUM_DATA_POINTERS] = {raster.pixels.data()};
        const int stride[AV_NUM_DATA_POINTERS] = {raster.stride};
        check(sws_scale(scaler_.get(), data, stride, 0, raster.height, image_->data, image_->linesize),
              "Convert output frame");
        image_->pts = frame.pts;
        image_->duration = frame.duration;
        durations_.emplace(frame.pts, frame.duration);
        check(avcodec_send_frame(encoder_.get(), image_.get()), "Send output video frame");
        drain();
        copy_audio_until(frame.pts, stop);
    }

    void finish(std::stop_token stop) override {
        check_cancelled(stop);
        check(avcodec_send_frame(encoder_.get(), nullptr), "Flush video encoder");
        drain();
        copy_audio_until(std::nullopt, stop);
        check(av_write_trailer(output_.get()), "Finalize video");
        check(avio_closep(&output_->pb), "Close output video");
        check_cancelled(stop);
        publish(temporary_.file(), options_);
    }

private:
    void drain() {
        while (true) {
            const int result = avcodec_receive_packet(encoder_.get(), packet_.get());
            if (result == AVERROR_EOF || result == AVERROR(EAGAIN))
                return;
            check(result, "Encode video frame");
            if (const auto duration = durations_.find(packet_->pts); duration != durations_.end()) {
                packet_->duration = duration->second;
                durations_.erase(duration);
            }
            av_packet_rescale_ts(packet_.get(), encoder_->time_base, video_->time_base);
            packet_->stream_index = video_->index;
            check(av_interleaved_write_frame(output_.get(), packet_.get()), "Write video packet");
        }
    }

    void copy_audio_until(std::optional<std::int64_t> pts, std::stop_token stop) {
        if (!audio_ || audio_eof_)
            return;
        const auto time_base = input_->streams[audio_index_]->time_base;
        while (true) {
            check_cancelled(stop);
            if (!audio_pending_) {
                const int result = av_read_frame(input_.get(), audio_packet_.get());
                if (result == AVERROR_EOF) {
                    audio_eof_ = true;
                    return;
                }
                check(result, "Read source audio");
                if (audio_packet_->stream_index != audio_index_) {
                    av_packet_unref(audio_packet_.get());
                    continue;
                }
                audio_pending_ = true;
            }
            const auto timestamp = audio_packet_->dts == AV_NOPTS_VALUE ? audio_packet_->pts : audio_packet_->dts;
            if (pts && timestamp != AV_NOPTS_VALUE && av_compare_ts(timestamp, time_base, *pts, encoder_->time_base) > 0)
                return;
            av_packet_rescale_ts(audio_packet_.get(), time_base, audio_->time_base);
            audio_packet_->stream_index = audio_->index;
            audio_packet_->pos = -1;
            check(av_interleaved_write_frame(output_.get(), audio_packet_.get()), "Copy source audio packet");
            audio_pending_ = false;
        }
    }

    ExportOptions options_;
    Input input_;
    TemporaryOutput temporary_;
    std::unique_ptr<AVFormatContext, OutputDeleter> output_;
    Codec encoder_;
    Image image_{allocated(av_frame_alloc())};
    Packet packet_{allocated(av_packet_alloc())};
    Packet audio_packet_{allocated(av_packet_alloc())};
    Scaler scaler_{nullptr, sws_freeContext};
    std::map<std::int64_t, std::int64_t> durations_;
    AVStream* video_ = nullptr;
    AVStream* audio_ = nullptr;
    int audio_index_ = -1;
    bool audio_pending_ = false;
    bool audio_eof_ = false;
};

}

std::unique_ptr<Reader> FFmpegMedia::open(const std::filesystem::path& path) const {
    return std::make_unique<FFmpegReader>(path);
}

std::unique_ptr<Writer> FFmpegMedia::create(const std::filesystem::path& source,
                                            const ExportOptions& options) const {
    return std::make_unique<FFmpegWriter>(source, options);
}

}
