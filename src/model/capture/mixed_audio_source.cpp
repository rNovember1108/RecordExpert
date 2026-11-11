#include "capture/mixed_audio_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <chrono>
#include <cstdint>
#include <mutex>
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
constexpr int kResyncThreshold = 24000;

}  // namespace

MixedAudioSource::MixedAudioSource(AudioOptions options) : opts_(std::move(options)) {}
MixedAudioSource::~MixedAudioSource() { Close(); }

int MixedAudioSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<MixedAudioSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool MixedAudioSource::Open() {
    avdevice_register_all();
    if (opts_.device.empty() || opts_.secondary_device.empty()) return false;
    if (opts_.device == opts_.secondary_device) return false;
    if (!OpenEncoder()) return false;
    if (!OpenOneInput(opts_.device, &in_a_ctx_, &dec_a_, &idx_a_, &swr_a_)) { Close(); return false; }
    if (!OpenOneInput(opts_.secondary_device, &in_b_ctx_, &dec_b_, &idx_b_, &swr_b_)) { Close(); return false; }
    fifo_a_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, enc_ctx_->ch_layout.nb_channels, 1);
    fifo_b_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, enc_ctx_->ch_layout.nb_channels, 1);
    fifo_out_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, enc_ctx_->ch_layout.nb_channels, 1);
    if (!fifo_a_ || !fifo_b_ || !fifo_out_) { Close(); return false; }
    return true;
}

bool MixedAudioSource::OpenOneInput(const std::string& device_name,
                                    AVFormatContext** fmt, AVCodecContext** dec,
                                    int* idx, SwrContext** swr) {
    const AVInputFormat* dshow = av_find_input_format("dshow");
    if (!dshow) return false;
    std::string url = "audio=" + device_name;
    AVDictionary* opts = nullptr;
    av_dict_set_int(&opts, "sample_rate", opts_.sample_rate, 0);
    av_dict_set_int(&opts, "channels", opts_.channels, 0);
    av_dict_set_int(&opts, "rtbufsize", 16 * 1024 * 1024, 0);
    *fmt = avformat_alloc_context();
    if (!*fmt) { av_dict_free(&opts); return false; }
    (*fmt)->interrupt_callback.callback = &MixedAudioSource::InterruptCallback;
    (*fmt)->interrupt_callback.opaque = this;
    int ret = avformat_open_input(fmt, url.c_str(), dshow, &opts);
    av_dict_free(&opts);
    if (ret < 0) { avformat_close_input(fmt); return false; }
    if (avformat_find_stream_info(*fmt, nullptr) < 0) { avformat_close_input(fmt); return false; }
    *idx = av_find_best_stream(*fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (*idx < 0) { avformat_close_input(fmt); return false; }
    AVStream* in_stream = (*fmt)->streams[*idx];
    const AVCodec* decoder = avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) { avformat_close_input(fmt); return false; }
    *dec = avcodec_alloc_context3(decoder);
    if (!*dec) { avformat_close_input(fmt); return false; }
    avcodec_parameters_to_context(*dec, in_stream->codecpar);
    if (avcodec_open2(*dec, decoder, nullptr) < 0) { avcodec_free_context(dec); avformat_close_input(fmt); return false; }
    *swr = swr_alloc();
    if (!*swr) { avcodec_free_context(dec); avformat_close_input(fmt); return false; }
    av_opt_set_chlayout(*swr, "in_chlayout", &(*dec)->ch_layout, 0);
    av_opt_set_int(*swr, "in_sample_rate", (*dec)->sample_rate, 0);
    av_opt_set_sample_fmt(*swr, "in_sample_fmt", (*dec)->sample_fmt, 0);
    av_opt_set_chlayout(*swr, "out_chlayout", &enc_ctx_->ch_layout, 0);
    av_opt_set_int(*swr, "out_sample_rate", enc_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(*swr, "out_sample_fmt", enc_ctx_->sample_fmt, 0);
    if (swr_init(*swr) < 0) { swr_free(swr); avcodec_free_context(dec); avformat_close_input(fmt); return false; }
    return true;
}

bool MixedAudioSource::OpenEncoder() {
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

int MixedAudioSource::PumpOne(AVFormatContext* in, AVCodecContext* dec, int idx,
                              SwrContext* swr, AVAudioFifo* fifo,
                              AVPacket* pkt, AVFrame* dec_frame) {
    int ret = av_read_frame(in, pkt);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) return 1;
        if (ret == AVERROR_EXIT) return 1;
        return 0;
    }
    if (pkt->stream_index != idx) { av_packet_unref(pkt); return 1; }
    ret = avcodec_send_packet(dec, pkt);
    av_packet_unref(pkt);
    if (ret < 0) return 1;
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return 0;
        int out_samples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr, dec->sample_rate) + dec_frame->nb_samples,
            enc_ctx_->sample_rate, dec->sample_rate, AV_ROUND_UP));
        uint8_t** conv = nullptr;
        if (av_samples_alloc_array_and_samples(&conv, nullptr, enc_ctx_->ch_layout.nb_channels,
                                               out_samples, enc_ctx_->sample_fmt, 0) < 0) {
            av_frame_unref(dec_frame); return 0;
        }
        int converted = swr_convert(swr, conv, out_samples,
            const_cast<const uint8_t**>(dec_frame->data), dec_frame->nb_samples);
        av_frame_unref(dec_frame);
        if (converted < 0) { if (conv) av_freep(&conv[0]); av_freep(&conv); return 0; }
        if (converted > 0) av_audio_fifo_write(fifo, reinterpret_cast<void**>(conv), converted);
        if (conv) av_freep(&conv[0]);
        av_freep(&conv);
    }
    return 1;
}

int MixedAudioSource::MixOne() {
    const int na = av_audio_fifo_size(fifo_a_);
    const int nb = av_audio_fifo_size(fifo_b_);
    if (na <= 0 || nb <= 0) return 0;
    if (na - nb > kResyncThreshold) av_audio_fifo_drain(fifo_a_, na - nb);
    else if (nb - na > kResyncThreshold) av_audio_fifo_drain(fifo_b_, nb - na);
    const int n = std::min(av_audio_fifo_size(fifo_a_), av_audio_fifo_size(fifo_b_));
    if (n <= 0) return 0;
    const int ch = enc_ctx_->ch_layout.nb_channels;
    std::vector<std::vector<float>> bufA(ch, std::vector<float>(n));
    std::vector<std::vector<float>> bufB(ch, std::vector<float>(n));
    std::vector<std::vector<float>> mixed(ch, std::vector<float>(n));
    std::vector<float*> pA(ch), pB(ch), pM(ch);
    for (int c = 0; c < ch; ++c) {
        pA[c] = bufA[c].data(); pB[c] = bufB[c].data(); pM[c] = mixed[c].data();
    }
    av_audio_fifo_read(fifo_a_, reinterpret_cast<void**>(pA.data()), n);
    av_audio_fifo_read(fifo_b_, reinterpret_cast<void**>(pB.data()), n);
    for (int c = 0; c < ch; ++c) {
        const float* a = bufA[c].data();
        const float* b = bufB[c].data();
        float* m = mixed[c].data();
        for (int i = 0; i < n; ++i) {
            float v = 0.5f * a[i] + 0.5f * b[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            m[i] = v;
        }
    }
    av_audio_fifo_write(fifo_out_, reinterpret_cast<void**>(pM.data()), n);
    return n;
}

bool MixedAudioSource::EncodeFrameToQueue(AVFrame* frame) {
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

bool MixedAudioSource::FlushEncoderToQueue() {
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

bool MixedAudioSource::Run(double duration_seconds,
                           std::chrono::steady_clock::time_point start,
                           const std::atomic<bool>& stop,
                           const std::atomic<bool>& flush_requested) {
    start_ = start;
    stop_flag_ = &stop;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* dec_frame_a = av_frame_alloc();
    AVFrame* dec_frame_b = av_frame_alloc();
    if (!pkt || !dec_frame_a || !dec_frame_b) {
        av_packet_free(&pkt); av_frame_free(&dec_frame_a); av_frame_free(&dec_frame_b);
        return false;
    }

    bool ok = true;
    const int frame_size = enc_ctx_->frame_size;
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;

    while (!stop.load()) {
        // segment 边界 flush：必须放在 paused 检查之前（否则边界触发瞬间按暂停，
        // producer 卡在 paused 分支收不到 flush_requested → muxer 5s 超时误停整场）。
        // flush 期间不清理 pause_start_/total_paused_（PTS 计时扣减基准，清零会跳变），
        // 也不强行解除暂停；prev_pts_ 复位无害保留。
        if (flush_requested.load()) {
            DrainFifo(true);
            if (fifo_a_) av_audio_fifo_reset(fifo_a_);
            if (fifo_b_) av_audio_fifo_reset(fifo_b_);
            if (fifo_out_) av_audio_fifo_reset(fifo_out_);
            if (flush_done_) flush_done_->store(true);
            while (flush_requested.load() && !stop.load()) av_usleep(200);
            prev_pts_ = -1; continue;
        }

        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{})
                pause_start_ = std::chrono::steady_clock::now();
            int ra = av_read_frame(in_a_ctx_, pkt);
            if (ra >= 0) av_packet_unref(pkt);
            else if (ra == AVERROR(EAGAIN)) {}
            else { ok = false; break; }
            if (!ok) break;
            int rb = av_read_frame(in_b_ctx_, pkt);
            if (rb >= 0) av_packet_unref(pkt);
            else if (rb == AVERROR(EAGAIN)) {}
            else { ok = false; break; }
            if (!ok) break;
            av_usleep(1000); continue;
        }

        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_ - total_paused_).count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

        if (PumpOne(in_a_ctx_, dec_a_, idx_a_, swr_a_, fifo_a_, pkt, dec_frame_a) == 0) { ok = false; break; }
        if (PumpOne(in_b_ctx_, dec_b_, idx_b_, swr_b_, fifo_b_, pkt, dec_frame_b) == 0) { ok = false; break; }

        MixOne();

        if (speed > 1 && av_audio_fifo_size(fifo_out_) >= speed) {
            const int total = av_audio_fifo_size(fifo_out_);
            const int kept = (total + speed - 1) / speed;
            const int ch = enc_ctx_->ch_layout.nb_channels;
            std::vector<std::vector<float>> tmp(ch, std::vector<float>(total));
            std::vector<float*> pp(ch);
            for (int c = 0; c < ch; ++c) pp[c] = tmp[c].data();
            av_audio_fifo_read(fifo_out_, reinterpret_cast<void**>(pp.data()), total);
            std::vector<std::vector<float>> down(ch, std::vector<float>(kept));
            std::vector<float*> dp(ch);
            for (int c = 0; c < ch; ++c) {
                const float* src = tmp[c].data();
                for (int i = 0, j = 0; i < total; i += speed, ++j) down[c][j] = src[i];
                dp[c] = down[c].data();
            }
            av_audio_fifo_write(fifo_out_, reinterpret_cast<void**>(dp.data()), kept);
        }

        while (av_audio_fifo_size(fifo_out_) >= frame_size) {
            if (!DrainFifo(false)) { ok = false; break; }
        }
        if (!ok) break;
        if (av_audio_fifo_size(fifo_a_) == 0 && av_audio_fifo_size(fifo_b_) == 0) av_usleep(1000);
    }

    if (!DrainFifo(true)) ok = false;

    av_packet_free(&pkt); av_frame_free(&dec_frame_a); av_frame_free(&dec_frame_b);
    return ok;
}

bool MixedAudioSource::DrainFifo(bool flush) {
    const int frame_size = enc_ctx_->frame_size;
    const int available = av_audio_fifo_size(fifo_out_);
    if (!flush && available < frame_size) return true;

    while (av_audio_fifo_size(fifo_out_) > 0) {
        const int take = flush ? std::min(frame_size, av_audio_fifo_size(fifo_out_)) : frame_size;
        if (!flush && av_audio_fifo_size(fifo_out_) < frame_size) break;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        frame->nb_samples = take;
        frame->format = enc_ctx_->sample_fmt;
        frame->sample_rate = enc_ctx_->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &enc_ctx_->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0) { av_frame_free(&frame); return false; }
        if (av_audio_fifo_read(fifo_out_, reinterpret_cast<void**>(frame->data), take) < take) {
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

void MixedAudioSource::Close() {
    if (enc_ctx_) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; }
    if (swr_a_) swr_free(&swr_a_);
    if (swr_b_) swr_free(&swr_b_);
    if (fifo_a_) { av_audio_fifo_free(fifo_a_); fifo_a_ = nullptr; }
    if (fifo_b_) { av_audio_fifo_free(fifo_b_); fifo_b_ = nullptr; }
    if (fifo_out_) { av_audio_fifo_free(fifo_out_); fifo_out_ = nullptr; }
    if (dec_a_) avcodec_free_context(&dec_a_);
    if (dec_b_) avcodec_free_context(&dec_b_);
    if (in_a_ctx_) { avformat_close_input(&in_a_ctx_); in_a_ctx_ = nullptr; }
    if (in_b_ctx_) { avformat_close_input(&in_b_ctx_); in_b_ctx_ = nullptr; }
    stream_ = nullptr; queue_ = nullptr; flush_done_ = nullptr;
}

}  // namespace re
