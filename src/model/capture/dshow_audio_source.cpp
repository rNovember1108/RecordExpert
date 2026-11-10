#include "dshow_audio_source.h"

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

}  // namespace

DShowAudioSource::DShowAudioSource(AudioOptions options, Mp4Writer* writer)
    : opts_(std::move(options)), writer_(writer) {}

DShowAudioSource::~DShowAudioSource() { Close(); }

// static：停止标志置位后，dshow 内部的阻塞等待会轮询到此回调并中止本次 I/O，
// av_read_frame 随即返回 AVERROR_EXIT。打开设备阶段 stop_flag_ 尚为空，放行。
int DShowAudioSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<DShowAudioSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool DShowAudioSource::Open() {
    // dshow 位于 libavdevice，必须显式注册。可多次调用（视频轨也会注册）。
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

        // 在设备支持时,向其请求立体声 48 kHz 流;设备实际交付什么,交由重采样器处理。
        av_dict_set_int(&opts, "sample_rate", opts_.sample_rate, 0);
        av_dict_set_int(&opts, "channels", opts_.channels, 0);
        // 增大 dshow 实时缓冲区(默认仅 ~3MB,音频线程被 muxer 互斥锁短暂阻塞时容易溢出
        // 丢帧)。16MB 对 48kHz/2ch/s16(192 KB/s)约有 87 秒余量,远超任何偶发阻塞。
        av_dict_set_int(&opts, "rtbufsize", 16 * 1024 * 1024, 0);

        // 预先分配输入上下文并挂上中断回调(必须在 avformat_open_input 之前设置),
        // 使停止录制能打断阻塞中的 dshow 读取。
        in_fmt_ctx_ = avformat_alloc_context();
        if (!in_fmt_ctx_) break;
        in_fmt_ctx_->interrupt_callback.callback = &DShowAudioSource::InterruptCallback;
        in_fmt_ctx_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&in_fmt_ctx_, url.c_str(), dshow, &opts);
        if (ret < 0) break;

        if (avformat_find_stream_info(in_fmt_ctx_, nullptr) < 0) break;

        in_index_ = av_find_best_stream(in_fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1,
                                        nullptr, 0);
        if (in_index_ < 0) break;

        AVStream* in_stream = in_fmt_ctx_->streams[in_index_];
        const AVCodec* decoder =
            avcodec_find_decoder(in_stream->codecpar->codec_id);
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

bool DShowAudioSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredAudioEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;

    enc_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AAC 要求 planar float。
    enc_ctx_->sample_rate = opts_.sample_rate;
    enc_ctx_->bit_rate = opts_.bit_rate;
    av_channel_layout_default(&enc_ctx_->ch_layout, opts_.channels);
    enc_ctx_->time_base = AVRational{1, opts_.sample_rate};

    if (writer_->NeedsGlobalHeader()) {
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) return false;

    stream_ = writer_->NewStream(enc_ctx_);
    return stream_ != nullptr;
}

bool DShowAudioSource::InitResampler() {
    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) return false;

    // 配置从解码器格式到编码器 FLTP 的转换。
    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &dec_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", dec_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", dec_ctx_->sample_fmt, 0);

    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &enc_ctx_->ch_layout, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", enc_ctx_->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", enc_ctx_->sample_fmt, 0);

    if (swr_init(swr_ctx_) < 0) return false;

    fifo_ = av_audio_fifo_alloc(enc_ctx_->sample_fmt,
                                enc_ctx_->ch_layout.nb_channels, 1);
    if (!fifo_) return false;
    return true;
}

bool DShowAudioSource::Run(std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop) {
    start_ = start;
    // 让中断回调能看到停止标志（仅在 Run 期间生效）。
    stop_flag_ = &stop;

    AVPacket* in_pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    if (!in_pkt || !dec_frame) {
        av_packet_free(&in_pkt);
        av_frame_free(&dec_frame);
        return false;
    }

    bool ok = true;
    const int frame_size = enc_ctx_->frame_size;  // AAC：通常为 1024。

    while (!stop.load()) {
        // 暂停期间持续从 dshow 读包但直接丢弃。dshow 是实时推流模型：声卡不断向
        // 其内部缓冲（rtbufsize=16MB ≈ 87 秒）推数据，若暂停时不读，缓冲会持续
        // 堆积并在长暂停时溢出丢包；读出即 unref 丢弃等效于"输入暂停"且无溢出
        // 风险。输出侧自然暂停：丢弃的包不进 swr/FIFO/编码器；暂停时长累计进
        // total_paused_，音频 PTS（墙钟换算）在继续后自然接续。
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            int ret = av_read_frame(in_fmt_ctx_, in_pkt);
            if (ret >= 0) {
                // 正常包：直接丢弃，持续排空 dshow 缓冲。
                av_packet_unref(in_pkt);
            } else if (ret == AVERROR(EAGAIN)) {
                // 暂时没有新数据：短暂等待后继续排空。
                av_usleep(1000);
            } else {
                // AVERROR_EXIT（停止请求经中断回调中止）或设备被拔等硬错误：
                // 都跳出录制循环。停止请求走正常 flush/收尾；硬错误继续空转
                // 无意义，同样 break 让已录内容完整落盘。
                break;
            }
            continue;
        }

        int ret = av_read_frame(in_fmt_ctx_, in_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                av_usleep(1000);
                continue;
            }
            if (ret == AVERROR_EXIT) {
                // 中断回调因停止请求中止了阻塞采集：属于正常收尾，不报错。
                break;
            }
            break;
        }

        if (in_pkt->stream_index != in_index_) {
            av_packet_unref(in_pkt);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx_, in_pkt);
        av_packet_unref(in_pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ok = false;
                break;
            }

            // 把解码得到的样本重采样成 FLTP 并暂存到 FIFO。按转换后的样本数分配
            // 目标缓冲。
            int out_samples = static_cast<int>(av_rescale_rnd(
                swr_get_delay(swr_ctx_, dec_ctx_->sample_rate) +
                    dec_frame->nb_samples,
                enc_ctx_->sample_rate, dec_ctx_->sample_rate, AV_ROUND_UP));

            uint8_t** conv = nullptr;
            if (av_samples_alloc_array_and_samples(
                    &conv, nullptr, enc_ctx_->ch_layout.nb_channels,
                    out_samples, enc_ctx_->sample_fmt, 0) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }

            int converted =
                swr_convert(swr_ctx_, conv, out_samples,
                            const_cast<const uint8_t**>(dec_frame->data),
                            dec_frame->nb_samples);
            if (converted < 0) {
                if (conv) av_freep(&conv[0]);
                av_freep(&conv);
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }

            if (converted > 0) {
                // 倍率录制下采样：speed_multiplier>1 时每 N 个样本保留 1 个。
                // 不改变输出采样率（仍为 sample_rate Hz），但样本数缩短为 1/N，
                // 频率提升 N 倍（time-lapse 标准做法，变调可接受）。
                // 下采样后立即写入 FIFO，使后续 DrainFifo 只看到保留下来的样本，
                // 输出时间轴相应压缩为墙钟的 1/N。
                const int speed = (opts_.speed_multiplier >= 1)
                                      ? opts_.speed_multiplier
                                      : 1;
                const int kept = (converted + speed - 1) / speed;
                if (speed > 1 && kept > 0) {
                    const int ch = enc_ctx_->ch_layout.nb_channels;
                    std::vector<std::vector<float>> down(ch,
                                                         std::vector<float>(
                                                             static_cast<size_t>(
                                                                 kept)));
                    for (int c = 0; c < ch; ++c) {
                        const float* src =
                            reinterpret_cast<const float*>(conv[c]);
                        for (int i = 0, j = 0; i < converted; i += speed, ++j) {
                            down[static_cast<size_t>(c)]
                                [static_cast<size_t>(j)] = src[i];
                        }
                    }
                    std::vector<float*> down_ptrs(
                        static_cast<size_t>(ch));
                    for (int c = 0; c < ch; ++c) {
                        down_ptrs[static_cast<size_t>(c)] =
                            down[static_cast<size_t>(c)].data();
                    }
                    av_audio_fifo_write(
                        fifo_, reinterpret_cast<void**>(down_ptrs.data()),
                        kept);
                } else {
                    // N=1：原样写入，行为与 1:1 实时录制完全一致。
                    av_audio_fifo_write(fifo_, reinterpret_cast<void**>(conv),
                                        converted);
                }
            }
            if (conv) av_freep(&conv[0]);
            av_freep(&conv);

            av_frame_unref(dec_frame);

            // FIFO 里已攒够多少个整帧，就编码多少个。
            while (av_audio_fifo_size(fifo_) >= frame_size) {
                if (!DrainFifo(false)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }

    // 无论循环正常结束还是中途出错，都把 FIFO 残帧送入编码器并 flush，尽量让已
    // 录制的音频完整落盘（提前终止时文件仍可播放）。
    if (!DrainFifo(true)) ok = false;

    av_packet_free(&in_pkt);
    av_frame_free(&dec_frame);
    return ok;
}

bool DShowAudioSource::DrainFifo(bool flush) {
    const int frame_size = enc_ctx_->frame_size;
    const int available = av_audio_fifo_size(fifo_);

    // 常规调用需要一个完整帧；flush 时则有多少取多少。
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
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return false;
        }

        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data),
                               take) < take) {
            av_frame_free(&frame);
            return false;
        }

        // ---- 墙钟 PTS（与视频源同口径）----
        // 每帧按"此刻实际录制经过时间"（共享零点 + 扣除暂停）实时换算，不再跟随
        // 声卡采样计数——声卡晶振与系统钟的偏差会在长录制中累积成音画错位。
        // 首帧自然落在设备真实启动时刻（如实反映晚启动，而非硬贴 PTS 0）。
        // 倍率录制下墙钟 N 秒对应 1 秒输出，故除以 speed_multiplier。
        // 暂停期间不可能进入 drain（暂停分支不排 FIFO），故此处 paused_ 恒为 false。
        // flush 残帧例外：这些样本在停止前就已采集，若按 flush 时刻墙钟打戳会把
        // 音轨顶长几十毫秒（进度条总时长超过实际录制时长）；改用接续 PTS。
        // 正常帧恒为 frame_size，flush 循环里仅末帧可能为残帧，故 +frame_size 接续
        // 恒正确（残帧永远是最后一帧，不会被接续）。
        int64_t pts;
        if (flush && prev_pts_ >= 0) {
            pts = prev_pts_ + frame_size;
        } else {
            const int speed = (opts_.speed_multiplier >= 1)
                                  ? opts_.speed_multiplier
                                  : 1;
            const double elapsed_sec =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_ - total_paused_)
                    .count();
            pts = std::llround(elapsed_sec *
                               enc_ctx_->sample_rate / speed);
            if (pts < 0) pts = 0;
        }
        if (pts <= prev_pts_) pts = prev_pts_ + 1;  // 突发连排撞槽时强制单调
        prev_pts_ = pts;

        frame->pts = pts;

        bool ok = writer_->EncodeAndWrite(enc_ctx_, stream_, frame);
        av_frame_free(&frame);
        if (!ok) return false;

        if (!flush) break;  // 非 flush 路径只编码恰好一帧。
    }

    if (flush) return writer_->EncodeAndWrite(enc_ctx_, stream_, nullptr);
    return true;
}

void DShowAudioSource::Close() {
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (fifo_) {
        av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
    }
    if (dec_ctx_) avcodec_free_context(&dec_ctx_);
    if (enc_ctx_) avcodec_free_context(&enc_ctx_);
    if (in_fmt_ctx_) {
        avformat_close_input(&in_fmt_ctx_);
        in_fmt_ctx_ = nullptr;
    }
    // stream_ 由 Mp4Writer 的输出上下文拥有，此处不释放。
    stream_ = nullptr;
}

}  // namespace re
