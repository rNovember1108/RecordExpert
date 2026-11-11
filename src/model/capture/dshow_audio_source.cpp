#include "capture/dshow_audio_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

namespace re {
namespace {

const char* kPreferredAudioEncoders[] = {"aac", "aac_mf"};

}  // namespace

DShowAudioSource::DShowAudioSource(AudioOptions options) : opts_(std::move(options)) {}
DShowAudioSource::~DShowAudioSource() { Close(); }

int DShowAudioSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<DShowAudioSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool DShowAudioSource::Open() {
    avdevice_register_all();
    if (!OpenInput()) return false;
    if (!OpenEncoder())   { Close(); return false; }
    if (!InitResampler()) { Close(); return false; }
    return true;
}

bool DShowAudioSource::OpenInput() {
    bool ok = false;
    AVDictionary* opts = nullptr;
    do {
        const AVInputFormat* dshow = av_find_input_format("dshow");
        if (!dshow) break;
        std::string url = "audio=" + opts_.device;
        av_dict_set_int(&opts, "sample_rate", opts_.sample_rate, 0);
        av_dict_set_int(&opts, "channels", opts_.channels, 0);
        av_dict_set_int(&opts, "rtbufsize", 16 * 1024 * 1024, 0);
        in_fmt_ctx_ = avformat_alloc_context();
        if (!in_fmt_ctx_) break;
        in_fmt_ctx_->interrupt_callback.callback =
            &DShowAudioSource::InterruptCallback;
        in_fmt_ctx_->interrupt_callback.opaque = this;
        int ret = avformat_open_input(&in_fmt_ctx_, url.c_str(), dshow, &opts);
        if (ret < 0) break;
        if (avformat_find_stream_info(in_fmt_ctx_, nullptr) < 0) break;
        in_index_ = av_find_best_stream(in_fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (in_index_ < 0) break;
        AVStream* in_stream = in_fmt_ctx_->streams[in_index_];
        const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
        if (!decoder) break;
        dec_ctx_ = avcodec_alloc_context3(decoder);
        if (!dec_ctx_) break;
        avcodec_parameters_to_context(dec_ctx_, in_stream->codecpar);
        if (avcodec_open2(dec_ctx_, decoder, nullptr) < 0) break;
        ok = true;
    } while (false);
    av_dict_free(&opts);
    if (!ok) { Close(); return false; }
    return true;
}

// OpenEncoder：完整 enc_ctx_ + open2。producer 全程持有，不分段重建。
// AV_CODEC_FLAG_GLOBAL_HEADER 预先设好（幂等，Mp4Writer::OpenMuxer 会重复检查）。
bool DShowAudioSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredAudioEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;
    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;
    enc_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc_ctx_->sample_rate = opts_.sample_rate;
    enc_ctx_->bit_rate = opts_.bit_rate;
    av_channel_layout_default(&enc_ctx_->ch_layout, opts_.channels);
    enc_ctx_->time_base = AVRational{1, opts_.sample_rate};
    enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; return false; }
    return true;
}

bool DShowAudioSource::InitResampler() {
    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) return false;
    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &dec_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", dec_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", dec_ctx_->sample_fmt, 0);
    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &enc_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", enc_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", enc_ctx_->sample_fmt, 0);
    if (swr_init(swr_ctx_) < 0) return false;
    fifo_ = av_audio_fifo_alloc(enc_ctx_->sample_fmt, enc_ctx_->ch_layout.nb_channels, 1);
    if (!fifo_) return false;
    return true;
}

bool DShowAudioSource::EncodeFrameToQueue(AVFrame* frame) {
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
        // 最多重试 ~200ms 防止 muxer 卡死时死等；超时丢包（音频按样本数打 PTS）。
        int written = av_fifo_write(queue_, &pkt, 1);
        for (int retry = 0; written < 1 && retry < 200; ++retry) {
            av_usleep(1000);
            written = av_fifo_write(queue_, &pkt, 1);
        }
        av_packet_unref(&pkt);
    }
}

bool DShowAudioSource::FlushEncoderToQueue() {
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

bool DShowAudioSource::Run(double duration_seconds,
                           std::chrono::steady_clock::time_point start,
                           const std::atomic<bool>& stop,
                           const std::atomic<bool>& flush_requested) {
    start_ = start;
    stop_flag_ = &stop;

    AVPacket* in_pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    if (!in_pkt || !dec_frame) { av_packet_free(&in_pkt); av_frame_free(&dec_frame); return false; }

    bool ok = true;
    const int frame_size = enc_ctx_->frame_size;

    while (!stop.load()) {
        // segment 边界 flush：必须放在 paused 检查之前（否则边界触发瞬间按暂停，
        // producer 卡在 paused 分支收不到 flush_requested → muxer 5s 超时误停整场）。
        // flush 期间不清理 pause_start_/total_paused_（PTS 计时扣减基准，清零会跳变），
        // 也不强行解除暂停；prev_pts_ 复位无害保留。
        if (flush_requested.load()) {
            // DrainFifo(true) + FlushEncoderToQueue + reset FIFO（跨 segment 残帧清空）。
            DrainFifo(true);
            if (fifo_) av_audio_fifo_reset(fifo_);
            if (flush_done_) flush_done_->store(true);
            while (flush_requested.load() && !stop.load()) av_usleep(200);
            prev_pts_ = -1;
            continue;
        }

        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{})
                pause_start_ = std::chrono::steady_clock::now();
            int ret = av_read_frame(in_fmt_ctx_, in_pkt);
            if (ret >= 0) { av_packet_unref(in_pkt); }
            else if (ret == AVERROR(EAGAIN)) av_usleep(1000);
            else break;
            continue;
        }

        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_ - total_paused_).count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

        int ret = av_read_frame(in_fmt_ctx_, in_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { av_usleep(1000); continue; }
            if (ret == AVERROR_EXIT) break;
            break;
        }
        if (in_pkt->stream_index != in_index_) { av_packet_unref(in_pkt); continue; }
        ret = avcodec_send_packet(dec_ctx_, in_pkt);
        av_packet_unref(in_pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { ok = false; break; }

            int out_samples = static_cast<int>(av_rescale_rnd(
                swr_get_delay(swr_ctx_, dec_ctx_->sample_rate) + dec_frame->nb_samples,
                enc_ctx_->sample_rate, dec_ctx_->sample_rate, AV_ROUND_UP));
            uint8_t** conv = nullptr;
            if (av_samples_alloc_array_and_samples(
                    &conv, nullptr, enc_ctx_->ch_layout.nb_channels,
                    out_samples, enc_ctx_->sample_fmt, 0) < 0) {
                av_frame_unref(dec_frame); ok = false; break;
            }
            int converted = swr_convert(swr_ctx_, conv, out_samples,
                const_cast<const uint8_t**>(dec_frame->data), dec_frame->nb_samples);
            if (converted < 0) { if (conv) av_freep(&conv[0]); av_freep(&conv); av_frame_unref(dec_frame); ok = false; break; }
            if (converted > 0) {
                const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
                const int kept = (converted + speed - 1) / speed;
                if (speed > 1 && kept > 0) {
                    const int ch = enc_ctx_->ch_layout.nb_channels;
                    std::vector<std::vector<float>> down(ch, std::vector<float>(kept));
                    std::vector<float*> down_ptrs(ch);
                    for (int c = 0; c < ch; ++c) {
                        const float* src = reinterpret_cast<const float*>(conv[c]);
                        for (int i = 0, j = 0; i < converted; i += speed, ++j)
                            down[c][j] = src[i];
                        down_ptrs[c] = down[c].data();
                    }
                    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(down_ptrs.data()), kept);
                } else {
                    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(conv), converted);
                }
            }
            if (conv) av_freep(&conv[0]);
            av_freep(&conv);
            av_frame_unref(dec_frame);

            while (av_audio_fifo_size(fifo_) >= frame_size) {
                if (!DrainFifo(false)) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }

    if (!DrainFifo(true)) ok = false;

    av_packet_free(&in_pkt); av_frame_free(&dec_frame);
    return ok;
}

bool DShowAudioSource::DrainFifo(bool flush) {
    const int frame_size = enc_ctx_->frame_size;
    const int available = av_audio_fifo_size(fifo_);
    if (!flush && available < frame_size) return true;

    while (av_audio_fifo_size(fifo_) > 0) {
        const int take = flush
                             ? std::min(frame_size, av_audio_fifo_size(fifo_))
                             : frame_size;
        if (!flush && av_audio_fifo_size(fifo_) < frame_size) break;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        frame->nb_samples = take;
        frame->format = enc_ctx_->sample_fmt;
        frame->sample_rate = enc_ctx_->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &enc_ctx_->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0) { av_frame_free(&frame); return false; }
        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data), take) < take) {
            av_frame_free(&frame); return false;
        }

        int64_t pts;
        if (flush && prev_pts_ >= 0) {
            pts = prev_pts_ + frame_size;
        } else {
            const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
            const double elapsed_sec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_ - total_paused_).count();
            pts = std::llround(elapsed_sec * enc_ctx_->sample_rate / speed);
            if (pts < 0) pts = 0;
        }
        if (pts <= prev_pts_) pts = prev_pts_ + 1;
        prev_pts_ = pts;
        frame->pts = pts;

        bool ok = EncodeFrameToQueue(frame);
        av_frame_free(&frame);
        if (!ok) return false;
        if (!flush) break;
    }

    if (flush) return FlushEncoderToQueue();
    return true;
}

void DShowAudioSource::Close() {
    if (enc_ctx_) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; }
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (fifo_) { av_audio_fifo_free(fifo_); fifo_ = nullptr; }
    if (dec_ctx_) avcodec_free_context(&dec_ctx_);
    if (in_fmt_ctx_) { avformat_close_input(&in_fmt_ctx_); in_fmt_ctx_ = nullptr; }
    stream_ = nullptr; queue_ = nullptr; flush_done_ = nullptr;
}

}  // namespace re
