#include "capture/gdi_grab_video_source.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace re {
namespace {

// 按优先级尝试的视频编码器（mpeg4 / mpeg4_v2 / mpeg1video）。
// 纯软件，与平台无关；Media Foundation / NVENC / QSV 需要 worker 线程 MTA
// 初始化且与 Qt QGuiApplication 线程模型冲突，因此在本实现里不启用。
const char* kPreferredVideoEncoders[] = {"mpeg4", "mpeg4_v2", "mpeg1video"};

}  // namespace

GdiGrabVideoSource::GdiGrabVideoSource(VideoOptions options)
    : opts_(std::move(options)) {}

GdiGrabVideoSource::~GdiGrabVideoSource() { Close(); }

int GdiGrabVideoSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<GdiGrabVideoSource*>(opaque);
    // 与其余 4 个 source 一致：仅在 Run 请求停止时中断阻塞的 av_read_frame，
    // 否则恒返回 0（不中断），否则 gdigrab 打开/读帧会被立即中止。
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool GdiGrabVideoSource::Open() {
    avdevice_register_all();

    if (!OpenMainInput()) return false;
    if (!OpenEncoder()) { Close(); return false; }
    if (!InitScaler()) { Close(); return false; }

    return true;
}

bool GdiGrabVideoSource::OpenMainInput() {
    const AVInputFormat* gdigrab = av_find_input_format("gdigrab");
    if (!gdigrab) return false;

    main_in_ = avformat_alloc_context();
    if (!main_in_) return false;
    main_in_->interrupt_callback.callback = &GdiGrabVideoSource::InterruptCallback;
    main_in_->interrupt_callback.opaque = this;

    std::string url = "desktop";
    std::string area;
    if (opts_.width > 0 && opts_.height > 0) {
        area = ":offset_x=" + std::to_string(opts_.offset_x) +
               ":offset_y=" + std::to_string(opts_.offset_y) +
               ":width=" + std::to_string(opts_.width) +
               ":height=" + std::to_string(opts_.height);
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "framerate", std::to_string(opts_.fps).c_str(), 0);
    av_dict_set(&opts, "draw_mouse", "1", 0);
    if (!area.empty()) av_dict_set(&opts, "video_size", area.substr(1).c_str(), 0);

    int ret = avformat_open_input(&main_in_, url.c_str(), gdigrab, &opts);
    av_dict_free(&opts);
    if (ret < 0) return false;
    if (avformat_find_stream_info(main_in_, nullptr) < 0) return false;

    main_idx_ = av_find_best_stream(main_in_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (main_idx_ < 0) return false;
    AVStream* in_stream = main_in_->streams[main_idx_];
    const AVCodec* dec = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!dec) return false;
    main_dec_ = avcodec_alloc_context3(dec);
    if (!main_dec_) return false;
    if (avcodec_parameters_to_context(main_dec_, in_stream->codecpar) < 0) return false;
    // rawvideo 解码器不需要 time_base，手动对齐到 gdigrab 的输出帧率。
    main_dec_->time_base = AVRational{1, opts_.fps};
    return avcodec_open2(main_dec_, dec, nullptr) >= 0;
}

// OpenEncoder：完整建 enc_ctx_ + open2。producer 全程持有同一个 enc_ctx_，
// segment 边界不重建；muxer 通过手动 pts 偏移让新 segment 从 PTS=0 起算。
//
// 注意：Open 时 stream_ 还未注入（ScreenRecorder 在 OpenMuxer 后 SetStream），
// NeedsGlobalHeader 和 AV_CODEC_FLAG_GLOBAL_HEADER 设置挪到 Mp4Writer::OpenMuxer
// 里做（它是唯一知道 muxer flags 的线程）。
bool GdiGrabVideoSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredVideoEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;

    enc_ctx_->width = opts_.width > 0 ? opts_.width : main_dec_->width;
    enc_ctx_->height = opts_.height > 0 ? opts_.height : main_dec_->height;
    enc_ctx_->pix_fmt = encoder->pix_fmts ? encoder->pix_fmts[0] : AV_PIX_FMT_YUV420P;
    enc_ctx_->time_base = AVRational{1, opts_.fps};
    enc_ctx_->framerate = AVRational{opts_.fps, 1};
    enc_ctx_->bit_rate = opts_.bit_rate;
    enc_ctx_->gop_size = 250;
    enc_ctx_->max_b_frames = 0;

    // 手动把 AV_CODEC_FLAG_GLOBAL_HEADER 预先设好。Mp4Writer::OpenMuxer 里
    // NeedsGlobalHeader() 检查之后可能重复设置，但这是幂等的，不影响正确性。
    enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return avcodec_open2(enc_ctx_, encoder, nullptr) >= 0;
}

bool GdiGrabVideoSource::InitScaler() {
    sws_ctx_ = sws_getContext(
        main_dec_->width, main_dec_->height, main_dec_->pix_fmt,
        enc_ctx_->width, enc_ctx_->height, enc_ctx_->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    return sws_ctx_ != nullptr;
}

// encoder → queue 封装：send_frame + 循环 receive_packet（处理 EAGAIN 等情况）
// + rescale 到 stream_->time_base + push queue_ + unref。
// 返回 false 只有硬错误（receive_packet 返回非 EAGAIN/EOF 的负值、或 queue 满）。
bool GdiGrabVideoSource::EncodeFrameToQueue(AVFrame* frame) {
    if (!enc_ctx_ || !queue_) return false;

    int ret = avcodec_send_frame(enc_ctx_, frame);
    if (ret < 0) return false;

    while (true) {
        AVPacket pkt; av_init_packet(&pkt);
        ret = avcodec_receive_packet(enc_ctx_, &pkt);
        if (ret == AVERROR_EOF) return true;     // encoder 内部没包了，正常
        if (ret == AVERROR(EAGAIN)) return true; // 需要更多帧，正常
        if (ret < 0) return false;               // 硬错误

        if (stream_) {
            av_packet_rescale_ts(&pkt, enc_ctx_->time_base, stream_->time_base);
            pkt.stream_index = stream_->index;
        }

        // push 到 AVFifo（producer 侧；consumer 侧会 av_packet_unref）。
        // FIFO 满时 pkt 尚未 unref，可安全重试；等 muxer 消费后重写。
        // 最多重试 ~200ms 防止 muxer 卡死时死等；超时丢包（PTS 是墙钟，丢帧不破坏时间轴）。
        int written = av_fifo_write(queue_, &pkt, 1);
        for (int retry = 0; written < 1 && retry < 200; ++retry) {
            av_usleep(1000);
            written = av_fifo_write(queue_, &pkt, 1);
        }
        av_packet_unref(&pkt);
    }
}

// Run 末尾 / segment 边界 flush encoder：send_frame(nullptr) + receive_packet
// 循环，把 encoder 内残留 packet 全部 push queue_。
bool GdiGrabVideoSource::FlushEncoderToQueue() {
    if (!enc_ctx_ || !queue_) return false;

    avcodec_send_frame(enc_ctx_, nullptr);

    while (true) {
        AVPacket pkt; av_init_packet(&pkt);
        int ret = avcodec_receive_packet(enc_ctx_, &pkt);
        if (ret == AVERROR_EOF) break;           // encoder 内部没包了，正常
        if (ret == AVERROR(EAGAIN)) break;       // 需要更多帧，正常
        if (ret < 0) return false;               // 硬错误

        if (stream_) {
            av_packet_rescale_ts(&pkt, enc_ctx_->time_base, stream_->time_base);
            pkt.stream_index = stream_->index;
        }

        // push 到 AVFifo（producer 侧；consumer 侧会 av_packet_unref）。
        // FIFO 满时 pkt 尚未 unref，可安全重试；等 muxer 消费后重写。
        // 最多重试 ~200ms 防止 muxer 卡死时死等；超时丢包（PTS 是墙钟，丢帧不破坏时间轴）。
        int written = av_fifo_write(queue_, &pkt, 1);
        for (int retry = 0; written < 1 && retry < 200; ++retry) {
            av_usleep(1000);
            written = av_fifo_write(queue_, &pkt, 1);
        }
        av_packet_unref(&pkt);
    }

    // send_frame(nullptr) 会让 encoder 进入 drain 模式（之后 send_frame 一律返回
    // AVERROR_EOF）。segment 边界 flush 后 producer 还要继续编码，必须用
    // flush_buffers 退出 drain 模式；Run 退出前的最终 flush 多调一次无副作用。
    avcodec_flush_buffers(enc_ctx_);
    return true;
}

bool GdiGrabVideoSource::Run(double duration_seconds,
                             std::chrono::steady_clock::time_point start,
                             const std::atomic<bool>& stop,
                             const std::atomic<bool>& flush_requested) {
    auto reset_pause = [this]() {
        paused_.store(false);
        pause_start_ = std::chrono::steady_clock::time_point{};
        total_paused_ = std::chrono::steady_clock::duration{};
    };
    reset_pause();

    start_ = start;
    stop_flag_ = &stop;
    main_in_->interrupt_callback.opaque = this;
    main_in_->interrupt_callback.callback = &GdiGrabVideoSource::InterruptCallback;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    AVFrame* enc_frame = av_frame_alloc();
    if (!pkt || !dec_frame || !enc_frame) {
        av_packet_free(&pkt); av_frame_free(&dec_frame); av_frame_free(&enc_frame);
        return false;
    }

    bool ok = true;
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
    int frame_idx = 0;

    while (!stop.load()) {
        // segment 边界 flush：必须放在 paused 检查之前——若边界触发瞬间用户按下
        // 暂停，producer 会卡在 paused 分支收不到 flush_requested，muxer 5s 超时
        // 就误停整场录制。flush 期间不重置暂停状态（边界 flush 进行中按下的暂停
        // 必须保留，否则 UI 显示暂停中、实际继续录）。
        if (flush_requested.load()) {
            FlushEncoderToQueue();
            if (flush_done_) flush_done_->store(true);
            // flush 完后继续等 flush_requested 被重置（muxer Rotate 完后 reset）。
            // 用 spin-wait + 短暂 sleep，避免空转耗 CPU。
            while (flush_requested.load() && !stop.load()) {
                av_usleep(200);
            }
            continue;
        }

        // 暂停期间 gdigrab 是拉模型：不调 av_read_frame 就不抓帧。纯 sleep，不读。
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_ - total_paused_).count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

        // 采集 + 解码。
        int ret = av_read_frame(main_in_, pkt);
        if (ret == AVERROR_EXIT) break;    // 中断回调触发，正常收尾
        if (ret != 0) { ok = false; break; }
        if (pkt->stream_index != main_idx_) { av_packet_unref(pkt); continue; }

        ret = avcodec_send_packet(main_dec_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) { ok = false; break; }

        while (true) {
            ret = avcodec_receive_frame(main_dec_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { ok = false; break; }

            // 倍率录制：每 speed 帧保留 1 帧。
            if (speed > 1) {
                if (frame_idx % speed != 0) {
                    av_frame_unref(dec_frame);
                    continue;
                }
            }
            ++frame_idx;

            // sws_scale + 墙钟 PTS。
            enc_frame->format = enc_ctx_->pix_fmt;
            enc_frame->width = enc_ctx_->width;
            enc_frame->height = enc_ctx_->height;
            enc_frame->color_range = AVCOL_RANGE_MPEG;
            if (av_frame_get_buffer(enc_frame, 32) < 0) {
                av_frame_unref(dec_frame); ok = false; break;
            }
            sws_scale(sws_ctx_, dec_frame->data, dec_frame->linesize, 0,
                      dec_frame->height, enc_frame->data, enc_frame->linesize);
            av_frame_unref(dec_frame);

            const double pts_sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_ - total_paused_).count();
            enc_frame->pts = std::llround(
                pts_sec * av_q2d(enc_ctx_->time_base) * enc_ctx_->framerate.num);
            if (enc_frame->pts < 0) enc_frame->pts = 0;

            if (!EncodeFrameToQueue(enc_frame)) { ok = false; break; }
            av_frame_unref(enc_frame);
        }
        if (!ok) break;
    }

    // Run 退出前 flush encoder，把残包全部 push 进 queue。
    if (!FlushEncoderToQueue()) ok = false;

    av_packet_free(&pkt);
    av_frame_free(&dec_frame);
    av_frame_free(&enc_frame);
    return ok;
}

void GdiGrabVideoSource::Close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (enc_ctx_) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; }
    if (main_dec_) avcodec_free_context(&main_dec_);
    if (main_in_) { avformat_close_input(&main_in_); main_in_ = nullptr; }
    main_idx_ = -1;
    stream_ = nullptr;
    queue_ = nullptr;
    flush_done_ = nullptr;
    stop_flag_ = nullptr;
}

}  // namespace re
