#include "capture/dshow_video_source.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace re {
namespace {

const char* kPreferredVideoEncoders[] = {"mpeg4"};

}  // namespace

DShowVideoSource::DShowVideoSource(VideoOptions options) : opts_(std::move(options)) {}
DShowVideoSource::~DShowVideoSource() { Close(); }

int DShowVideoSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<DShowVideoSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool DShowVideoSource::Open() {
    avdevice_register_all();
    if (!OpenInput()) return false;
    if (!OpenEncoder()) { Close(); return false; }
    if (!InitScaler())  { Close(); return false; }
    return true;
}

bool DShowVideoSource::OpenInput() {
    bool ok = false;
    AVDictionary* opts = nullptr;
    do {
        const AVInputFormat* dshow = av_find_input_format("dshow");
        if (!dshow) break;
        std::string url = "video=" + opts_.device;
        av_dict_set_int(&opts, "framerate", opts_.fps, 0);
        if (opts_.width > 0 && opts_.height > 0) {
            char video_size[64];
            std::snprintf(video_size, sizeof(video_size), "%dx%d",
                          opts_.width, opts_.height);
            av_dict_set(&opts, "video_size", video_size, 0);
        }
        av_dict_set_int(&opts, "rtbufsize", 64 * 1024 * 1024, 0);

        in_ = avformat_alloc_context();
        if (!in_) break;
        in_->interrupt_callback.callback =
            &DShowVideoSource::InterruptCallback;
        in_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&in_, url.c_str(), dshow, &opts);
        if (ret < 0) break;
        if (avformat_find_stream_info(in_, nullptr) < 0) break;
        idx_ = av_find_best_stream(in_, AVMEDIA_TYPE_VIDEO,
                                        -1, -1, nullptr, 0);
        if (idx_ < 0) break;
        AVStream* in_stream = in_->streams[idx_];
        const AVCodec* decoder =
            avcodec_find_decoder(in_stream->codecpar->codec_id);
        if (!decoder) break;
        dec_ = avcodec_alloc_context3(decoder);
        if (!dec_) break;
        avcodec_parameters_to_context(dec_, in_stream->codecpar);
        if (avcodec_open2(dec_, decoder, nullptr) < 0) break;
        ok = true;
    } while (false);
    av_dict_free(&opts);
    if (!ok) { Close(); return false; }
    return true;
}

// OpenEncoder：完整建 enc_ctx_ + open2。producer 全程持有，不分段重建。
// AV_CODEC_FLAG_GLOBAL_HEADER 预先设好（幂等，Mp4Writer::OpenMuxer 会重复检查）。
bool DShowVideoSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredVideoEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;

    enc_ctx_->width = dec_->width & ~1;
    enc_ctx_->height = dec_->height & ~1;
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx_->time_base = AVRational{1, opts_.fps};
    enc_ctx_->framerate = AVRational{opts_.fps, 1};
    enc_ctx_->bit_rate = opts_.bit_rate;
    enc_ctx_->gop_size = opts_.fps * 2;
    enc_ctx_->max_b_frames = 0;
    enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; return false; }
    return true;
}

bool DShowVideoSource::InitScaler() {
    sws_ctx_ = sws_getContext(dec_->width, dec_->height,
                              dec_->pix_fmt, enc_ctx_->width,
                              enc_ctx_->height, enc_ctx_->pix_fmt,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) return false;

    yuv_frame_ = av_frame_alloc();
    if (!yuv_frame_) return false;
    yuv_frame_->format = enc_ctx_->pix_fmt;
    yuv_frame_->width = enc_ctx_->width;
    yuv_frame_->height = enc_ctx_->height;
    if (av_frame_get_buffer(yuv_frame_, 32) < 0) return false;
    return true;
}

bool DShowVideoSource::EncodeFrameToQueue(AVFrame* frame) {
    if (!enc_ctx_ || !queue_) return false;
    int ret = avcodec_send_frame(enc_ctx_, frame);
    if (ret < 0) return false;
    while (true) {
        AVPacket pkt; av_init_packet(&pkt);
        ret = avcodec_receive_packet(enc_ctx_, &pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) return true;
        if (ret < 0) return false;
        if (stream_) {
            av_packet_rescale_ts(&pkt, enc_ctx_->time_base, stream_->time_base);
            pkt.stream_index = stream_->index;
        }
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

bool DShowVideoSource::FlushEncoderToQueue() {
    if (!enc_ctx_ || !queue_) return false;
    avcodec_send_frame(enc_ctx_, nullptr);
    while (true) {
        AVPacket pkt; av_init_packet(&pkt);
        int ret = avcodec_receive_packet(enc_ctx_, &pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) return false;
        if (stream_) {
            av_packet_rescale_ts(&pkt, enc_ctx_->time_base, stream_->time_base);
            pkt.stream_index = stream_->index;
        }
        int written = av_fifo_write(queue_, &pkt, 1);
        for (int retry = 0; written < 1 && retry < 200; ++retry) {
            av_usleep(1000);
            written = av_fifo_write(queue_, &pkt, 1);
        }
        av_packet_unref(&pkt);
    }
    // 退出 drain 模式：边界 flush 后 producer 还要继续编码（否则 send_frame 返回 EOF）。
    avcodec_flush_buffers(enc_ctx_);
    return true;
}

bool DShowVideoSource::Run(double duration_seconds,
                           std::chrono::steady_clock::time_point start,
                           const std::atomic<bool>& stop,
                           const std::atomic<bool>& flush_requested) {
    stop_flag_ = &stop;

    AVPacket* in_pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    if (!in_pkt || !dec_frame) {
        av_packet_free(&in_pkt); av_frame_free(&dec_frame);
        return false;
    }

    bool ok = true;
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
    int64_t seen_ = 0;

    while (!stop.load()) {
        // segment 边界 flush：必须放在 paused 检查之前（否则边界触发瞬间按暂停，
        // producer 卡在 paused 分支收不到 flush_requested → muxer 5s 超时误停整场）。
        // flush 期间不清理 pause_start_/total_paused_——它们是 PTS 墙钟计时的扣减
        // 基准，清零会导致边界后 PTS 跳变；也不强行解除暂停。
        if (flush_requested.load()) {
            FlushEncoderToQueue();
            if (flush_done_) flush_done_->store(true);
            while (flush_requested.load() && !stop.load()) av_usleep(200);
            continue;
        }

        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{})
                pause_start_ = std::chrono::steady_clock::now();
            int ret = av_read_frame(in_, in_pkt);
            if (ret >= 0) {
                av_packet_unref(in_pkt);
            } else if (ret == AVERROR(EAGAIN)) {
                av_usleep(1000);
            } else {
                break;
            }
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        auto paused_dur = total_paused_;
        if (pause_start_ != std::chrono::steady_clock::time_point{})
            paused_dur += now - pause_start_;
        double elapsed = std::chrono::duration<double>(now - start - paused_dur).count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

        int ret = av_read_frame(in_, in_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { av_usleep(1000); continue; }
            if (ret == AVERROR_EXIT) break;
            break;
        }
        if (in_pkt->stream_index != idx_) { av_packet_unref(in_pkt); continue; }

        ret = avcodec_send_packet(dec_, in_pkt);
        av_packet_unref(in_pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { ok = false; break; }

            ++seen_;
            if (speed > 1 && (seen_ % speed) != 0) {
                av_frame_unref(dec_frame); continue;
            }

            if (av_frame_make_writable(yuv_frame_) < 0) {
                av_frame_unref(dec_frame); ok = false; break;
            }
            sws_scale(sws_ctx_, dec_frame->data, dec_frame->linesize, 0,
                      dec_->height, yuv_frame_->data, yuv_frame_->linesize);
            av_frame_unref(dec_frame);

            int64_t pts = std::llround(elapsed * opts_.fps / speed);
            // 复用 prev_pts_ 保证撞槽单调（在 Run 局部维护，分段切换时重置由 flush_requested 触发）。
            if (pts <= prev_pts_) pts = prev_pts_ + 1;
            prev_pts_ = pts;
            yuv_frame_->pts = pts;

            if (!EncodeFrameToQueue(yuv_frame_)) { ok = false; break; }
        }
        if (!ok) break;
    }

    if (!FlushEncoderToQueue()) ok = false;
    av_packet_free(&in_pkt); av_frame_free(&dec_frame);
    return ok;
}

void DShowVideoSource::Close() {
    if (enc_ctx_) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; }
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (yuv_frame_) { av_frame_free(&yuv_frame_); yuv_frame_ = nullptr; }
    if (dec_) avcodec_free_context(&dec_);
    if (in_) { avformat_close_input(&in_); in_ = nullptr; }
    stream_ = nullptr; queue_ = nullptr; flush_done_ = nullptr;
}

}  // namespace re
