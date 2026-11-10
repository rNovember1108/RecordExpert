#include "mixed_audio_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Mp4Writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

namespace re {
namespace {

// 按优先级尝试的 AAC 编码器。
const char* kPreferredAudioEncoders[] = {"aac", "aac_mf"};

// 输入 FIFO resync 阈值（样本数）。当一路远超另一路（>0.5s @48k = 24000）时，
// 把较长一侧截到与较短一侧齐平，避免一路设备停推/慢推时另一路无限堆积。
constexpr int kResyncThreshold = 24000;

}  // namespace

MixedAudioSource::MixedAudioSource(AudioOptions options, Mp4Writer* writer)
    : opts_(std::move(options)), writer_(writer) {}

MixedAudioSource::~MixedAudioSource() { Close(); }

// static：停止标志置位后，dshow 内部阻塞等待会轮询此回调并中止本次 I/O。
int MixedAudioSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<MixedAudioSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool MixedAudioSource::Open() {
    avdevice_register_all();

    if (opts_.device.empty() || opts_.secondary_device.empty()) {
        return false;
    }
    if (opts_.device == opts_.secondary_device) {
        return false;
    }

    // 先建编码器：两路重采样器的输出格式（FLTP/48k/2ch）依赖 enc_ctx_。
    if (!OpenEncoder()) return false;

    // 主源（麦克风）与副源（系统声音）各自打开一路 dshow 音频输入。
    if (!OpenOneInput(opts_.device, &in_a_ctx_, &dec_a_, &idx_a_, &swr_a_)) {
        Close(); return false;
    }
    if (!OpenOneInput(opts_.secondary_device, &in_b_ctx_, &dec_b_, &idx_b_,
                      &swr_b_)) {
        Close(); return false;
    }

    // 三个 FIFO：主源、副源（FLTP 缓冲）+ 输出（混音后供编码器取 1024 帧）。
    fifo_a_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,
                                  enc_ctx_->ch_layout.nb_channels, 1);
    fifo_b_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,
                                  enc_ctx_->ch_layout.nb_channels, 1);
    fifo_out_ = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,
                                    enc_ctx_->ch_layout.nb_channels, 1);
    if (!fifo_a_ || !fifo_b_ || !fifo_out_) {
        Close(); return false;
    }
    return true;
}

// 打开单路 dshow 音频输入（与 DShowAudioSource::OpenInput/InitResampler 同逻辑）。
// 注意：enc_ctx_ 必须在调用前已创建（重采样器输出格式依赖它）。
bool MixedAudioSource::OpenOneInput(const std::string& device_name,
                                    AVFormatContext** fmt,
                                    AVCodecContext** dec, int* idx,
                                    SwrContext** swr) {
    const AVInputFormat* dshow = av_find_input_format("dshow");
    if (!dshow) {
        return false;
    }

    std::string url = "audio=" + device_name;
    AVDictionary* opts = nullptr;
    av_dict_set_int(&opts, "sample_rate", opts_.sample_rate, 0);
    av_dict_set_int(&opts, "channels", opts_.channels, 0);
    av_dict_set_int(&opts, "rtbufsize", 16 * 1024 * 1024, 0);

    *fmt = avformat_alloc_context();
    if (!*fmt) {
        av_dict_free(&opts);
        return false;
    }
    // 中断回调：停止标志置位后让阻塞采集立即返回。opaque 传 this，两路共用。
    (*fmt)->interrupt_callback.callback = &MixedAudioSource::InterruptCallback;
    (*fmt)->interrupt_callback.opaque = this;

    int ret = avformat_open_input(fmt, url.c_str(), dshow, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        // avformat_open_input 失败时 *fmt 可能为 nullptr 或已半初始化，
        // avformat_close_input 能安全处理 nullptr。
        avformat_close_input(fmt);
        return false;
    }
    if (avformat_find_stream_info(*fmt, nullptr) < 0) {
        avformat_close_input(fmt);
        return false;
    }
    *idx = av_find_best_stream(*fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (*idx < 0) {
        avformat_close_input(fmt);
        return false;
    }
    AVStream* in_stream = (*fmt)->streams[*idx];
    const AVCodec* decoder =
        avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(fmt);
        return false;
    }
    *dec = avcodec_alloc_context3(decoder);
    if (!*dec) { avformat_close_input(fmt); return false; }
    avcodec_parameters_to_context(*dec, in_stream->codecpar);
    if (avcodec_open2(*dec, decoder, nullptr) < 0) {
        avcodec_free_context(dec);
        avformat_close_input(fmt);
        return false;
    }

    // 重采样到编码器格式（FLTP/48k/2ch）。
    *swr = swr_alloc();
    if (!*swr) {
        avcodec_free_context(dec);
        avformat_close_input(fmt);
        return false;
    }
    av_opt_set_chlayout(*swr, "in_chlayout", &(*dec)->ch_layout, 0);
    av_opt_set_int(*swr, "in_sample_rate", (*dec)->sample_rate, 0);
    av_opt_set_sample_fmt(*swr, "in_sample_fmt", (*dec)->sample_fmt, 0);
    av_opt_set_chlayout(*swr, "out_chlayout", &enc_ctx_->ch_layout, 0);
    av_opt_set_int(*swr, "out_sample_rate", enc_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(*swr, "out_sample_fmt", enc_ctx_->sample_fmt, 0);
    if (swr_init(*swr) < 0) {
        swr_free(swr);
        avcodec_free_context(dec);
        avformat_close_input(fmt);
        return false;
    }

    return true;
}

bool MixedAudioSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredAudioEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) {
            break;
        }
    }
    if (!encoder) {
        return false;
    }
    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;
    enc_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc_ctx_->sample_rate = opts_.sample_rate;
    enc_ctx_->bit_rate = opts_.bit_rate;
    av_channel_layout_default(&enc_ctx_->ch_layout, opts_.channels);
    enc_ctx_->time_base = AVRational{1, opts_.sample_rate};
    if (writer_->NeedsGlobalHeader()) {
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) {
        return false;
    }
    stream_ = writer_->NewStream(enc_ctx_);
    return stream_ != nullptr;
}

// 从一路输入非阻塞读一个包并解码+重采样写入对应输入 FIFO。
int MixedAudioSource::PumpOne(AVFormatContext* in, AVCodecContext* dec,
                              int idx, SwrContext* swr, AVAudioFifo* fifo,
                              AVPacket* pkt, AVFrame* dec_frame) {
    int ret = av_read_frame(in, pkt);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            return 1;  // 暂无数据,下一轮再试。
        }
        // AVERROR_EXIT:中断回调因 stop 标志置位中止了阻塞采集,属于正常收尾。
        // 返回 1 让上层继续循环,外层 while(!stop) 会自然退出,不会走硬错误路径。
        if (ret == AVERROR_EXIT) {
            return 1;
        }
        return 0;  // 其他硬错误
    }
    if (pkt->stream_index != idx) {
        av_packet_unref(pkt);
        return 1;
    }
    ret = avcodec_send_packet(dec, pkt);
    av_packet_unref(pkt);
    if (ret < 0) {
        return 1;
    }
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            return 0;
        }
        // 重采样到 FLTP/48k/2ch。
        int out_samples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr, dec->sample_rate) + dec_frame->nb_samples,
            enc_ctx_->sample_rate, dec->sample_rate, AV_ROUND_UP));
        uint8_t** conv = nullptr;
        if (av_samples_alloc_array_and_samples(
                &conv, nullptr, enc_ctx_->ch_layout.nb_channels,
                out_samples, enc_ctx_->sample_fmt, 0) < 0) {
            av_frame_unref(dec_frame);
            return 0;
        }
        int converted = swr_convert(
            swr, conv, out_samples,
            const_cast<const uint8_t**>(dec_frame->data),
            dec_frame->nb_samples);
        av_frame_unref(dec_frame);
        if (converted < 0) {
            if (conv) av_freep(&conv[0]);
            av_freep(&conv);
            return 0;
        }
        if (converted > 0) {
            av_audio_fifo_write(fifo, reinterpret_cast<void**>(conv),
                                converted);
        }
        if (conv) av_freep(&conv[0]);
        av_freep(&conv);
    }
    return 1;
}

// 把两个输入 FIFO 中较短一侧的样本等权混音写入输出 FIFO。返回混入样本数。
int MixedAudioSource::MixOne() {
    const int na = av_audio_fifo_size(fifo_a_);
    const int nb = av_audio_fifo_size(fifo_b_);
    if (na <= 0 || nb <= 0) return 0;

    // resync：一路远超另一路时截断较长侧，防止停推/慢推时无限堆积。
    if (na - nb > kResyncThreshold) {
        av_audio_fifo_drain(fifo_a_, na - nb);
    } else if (nb - na > kResyncThreshold) {
        av_audio_fifo_drain(fifo_b_, nb - na);
    }
    const int n = std::min(av_audio_fifo_size(fifo_a_),
                           av_audio_fifo_size(fifo_b_));
    if (n <= 0) return 0;

    const int ch = enc_ctx_->ch_layout.nb_channels;
    std::vector<std::vector<float>> bufA(static_cast<size_t>(ch),
                                         std::vector<float>(static_cast<size_t>(n)));
    std::vector<std::vector<float>> bufB(static_cast<size_t>(ch),
                                         std::vector<float>(static_cast<size_t>(n)));
    std::vector<std::vector<float>> mixed(static_cast<size_t>(ch),
                                          std::vector<float>(static_cast<size_t>(n)));
    std::vector<float*> pA(static_cast<size_t>(ch));
    std::vector<float*> pB(static_cast<size_t>(ch));
    std::vector<float*> pM(static_cast<size_t>(ch));
    for (int c = 0; c < ch; ++c) {
        pA[static_cast<size_t>(c)] = bufA[static_cast<size_t>(c)].data();
        pB[static_cast<size_t>(c)] = bufB[static_cast<size_t>(c)].data();
        pM[static_cast<size_t>(c)] = mixed[static_cast<size_t>(c)].data();
    }
    if (av_audio_fifo_read(fifo_a_, reinterpret_cast<void**>(pA.data()), n) < n)
        return 0;
    if (av_audio_fifo_read(fifo_b_, reinterpret_cast<void**>(pB.data()), n) < n)
        return 0;
    // 等权混音 0.5+0.5，clamp 到 [-1,1] 防 AAC 过载爆音。
    for (int c = 0; c < ch; ++c) {
        const float* a = bufA[static_cast<size_t>(c)].data();
        const float* b = bufB[static_cast<size_t>(c)].data();
        float* m = mixed[static_cast<size_t>(c)].data();
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

bool MixedAudioSource::Run(std::chrono::steady_clock::time_point start,
                           const std::atomic<bool>& stop) {
    start_ = start;
    stop_flag_ = &stop;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* dec_frame_a = av_frame_alloc();
    AVFrame* dec_frame_b = av_frame_alloc();
    if (!pkt || !dec_frame_a || !dec_frame_b) {
        av_packet_free(&pkt);
        av_frame_free(&dec_frame_a);
        av_frame_free(&dec_frame_b);
        return false;
    }

    bool ok = true;
    const int frame_size = enc_ctx_->frame_size;  // AAC：通常 1024。

    while (!stop.load()) {
        // 暂停期间两路都持续读包丢弃，防 rtbufsize 溢出。
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            // 主源读包
            int ra = av_read_frame(in_a_ctx_, pkt);
            if (ra >= 0) av_packet_unref(pkt);
            else if (ra == AVERROR(EAGAIN)) { /* 继续下一轮 */ }
            else if (ra == AVERROR_EXIT) { break; }  // 正常停止收尾,不报错
            else { ok = false; break; }  // 硬错误
            if (!ok) break;
            // 副源读包
            int rb = av_read_frame(in_b_ctx_, pkt);
            if (rb >= 0) av_packet_unref(pkt);
            else if (rb == AVERROR(EAGAIN)) { /* 继续下一轮 */ }
            else if (rb == AVERROR_EXIT) { break; }  // 正常停止收尾
            else { ok = false; break; }  // 硬错误
            if (!ok) break;
            av_usleep(1000);
            continue;
        }

        // 非阻塞泵入两路。
        if (PumpOne(in_a_ctx_, dec_a_, idx_a_, swr_a_, fifo_a_,
                    pkt, dec_frame_a) == 0) { ok = false; break; }
        if (PumpOne(in_b_ctx_, dec_b_, idx_b_, swr_b_, fifo_b_,
                    pkt, dec_frame_b) == 0) { ok = false; break; }

        // 混音写入输出 FIFO。
        MixOne();

        // 倍率录制下采样：speed_multiplier>1 时每 N 个样本保留 1 个。
        const int speed = (opts_.speed_multiplier >= 1)
                              ? opts_.speed_multiplier : 1;
        if (speed > 1 && av_audio_fifo_size(fifo_out_) >= speed) {
            const int total = av_audio_fifo_size(fifo_out_);
            const int kept = (total + speed - 1) / speed;
            // 从 FIFO 读出全部，按 N 抽 1，再写回。
            const int ch = enc_ctx_->ch_layout.nb_channels;
            std::vector<std::vector<float>> tmp(static_cast<size_t>(ch),
                std::vector<float>(static_cast<size_t>(total)));
            std::vector<float*> pp(static_cast<size_t>(ch));
            for (int c = 0; c < ch; ++c)
                pp[static_cast<size_t>(c)] = tmp[static_cast<size_t>(c)].data();
            if (av_audio_fifo_read(fifo_out_, reinterpret_cast<void**>(pp.data()),
                                   total) < total) { ok = false; break; }
            std::vector<std::vector<float>> down(static_cast<size_t>(ch),
                std::vector<float>(static_cast<size_t>(kept)));
            std::vector<float*> dp(static_cast<size_t>(ch));
            for (int c = 0; c < ch; ++c) {
                const float* src = tmp[static_cast<size_t>(c)].data();
                for (int i = 0, j = 0; i < total; i += speed, ++j)
                    down[static_cast<size_t>(c)][static_cast<size_t>(j)] = src[i];
                dp[static_cast<size_t>(c)] = down[static_cast<size_t>(c)].data();
            }
            av_audio_fifo_write(fifo_out_, reinterpret_cast<void**>(dp.data()),
                                kept);
        }

        // 输出 FIFO 攒够整帧就编码。
        while (av_audio_fifo_size(fifo_out_) >= frame_size) {
            if (!DrainFifo(false)) { ok = false; break; }
        }
        if (!ok) break;
        // 无新数据时短暂让出 CPU，避免空转。
        if (av_audio_fifo_size(fifo_a_) == 0 &&
            av_audio_fifo_size(fifo_b_) == 0) {
            av_usleep(1000);
        }
    }

    if (!DrainFifo(true)) ok = false;

    av_packet_free(&pkt);
    av_frame_free(&dec_frame_a);
    av_frame_free(&dec_frame_b);
    return ok;
}

bool MixedAudioSource::DrainFifo(bool flush) {
    const int frame_size = enc_ctx_->frame_size;
    const int available = av_audio_fifo_size(fifo_out_);
    if (!flush && available < frame_size) return true;

    while (av_audio_fifo_size(fifo_out_) > 0) {
        const int take = flush
                             ? std::min(frame_size, av_audio_fifo_size(fifo_out_))
                             : frame_size;
        if (!flush && av_audio_fifo_size(fifo_out_) < frame_size) break;

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        frame->nb_samples = take;
        frame->format = enc_ctx_->sample_fmt;
        frame->sample_rate = enc_ctx_->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &enc_ctx_->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return false;
        }
        if (av_audio_fifo_read(fifo_out_,
                               reinterpret_cast<void**>(frame->data),
                               take) < take) {
            av_frame_free(&frame);
            return false;
        }

        // 墙钟 PTS（与 DShowAudioSource 同策略）：每帧按实际录制经过时间换算，
        // 不跟随声卡采样计数，长录制无晶振偏差累积；倍率模式除以 speed。
        // flush 残帧例外：样本在停止前已采集，改用接续 PTS（prev_pts_+frame_size），
        // 避免按 flush 时刻墙钟打戳把音轨顶长几十毫秒。
        int64_t pts;
        if (flush && prev_pts_ >= 0) {
            pts = prev_pts_ + frame_size;
        } else {
            const int speed = (opts_.speed_multiplier >= 1)
                                  ? opts_.speed_multiplier : 1;
            const double elapsed_sec =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_ - total_paused_)
                    .count();
            pts = std::llround(elapsed_sec *
                               enc_ctx_->sample_rate / speed);
            if (pts < 0) pts = 0;
        }
        if (pts <= prev_pts_) pts = prev_pts_ + 1;  // 撞槽强制单调
        prev_pts_ = pts;
        frame->pts = pts;

        bool wrote = writer_->EncodeAndWrite(enc_ctx_, stream_, frame);
        av_frame_free(&frame);
        if (!wrote) return false;
        if (!flush) break;
    }
    if (flush) return writer_->EncodeAndWrite(enc_ctx_, stream_, nullptr);
    return true;
}

void MixedAudioSource::Close() {
    if (swr_a_) swr_free(&swr_a_);
    if (swr_b_) swr_free(&swr_b_);
    if (fifo_a_) { av_audio_fifo_free(fifo_a_); fifo_a_ = nullptr; }
    if (fifo_b_) { av_audio_fifo_free(fifo_b_); fifo_b_ = nullptr; }
    if (fifo_out_) { av_audio_fifo_free(fifo_out_); fifo_out_ = nullptr; }
    if (dec_a_) avcodec_free_context(&dec_a_);
    if (dec_b_) avcodec_free_context(&dec_b_);
    if (enc_ctx_) avcodec_free_context(&enc_ctx_);
    if (in_a_ctx_) { avformat_close_input(&in_a_ctx_); in_a_ctx_ = nullptr; }
    if (in_b_ctx_) { avformat_close_input(&in_b_ctx_); in_b_ctx_ = nullptr; }
    stream_ = nullptr;
}

}  // namespace re
