#include "dshow_video_source.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "Mp4Writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace re {
namespace {

// 与 GdiGrabVideoSource 相同的编码器选型：纯软件 mpeg4，不依赖 COM。
const char* kPreferredVideoEncoders[] = {"mpeg4"};

}  // namespace

DShowVideoSource::DShowVideoSource(VideoOptions options, Mp4Writer* writer)
    : opts_(options), writer_(writer) {}

DShowVideoSource::~DShowVideoSource() { Close(); }

// static：停止标志置位后，dshow 内部的阻塞等待会轮询到此回调并中止本次 I/O，
// av_read_frame 随即返回 AVERROR_EXIT。打开设备阶段 stop_flag_ 尚为空，放行。
int DShowVideoSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<DShowVideoSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool DShowVideoSource::Open() {
    // dshow 位于 libavdevice，必须显式注册。可多次调用（音频轨也会注册）。
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

        // dshow 视频输入 URL:"video=<设备名>"。设备名来自枚举接口返回的
        // device_name(可直接用于 open_input)。
        std::string url = "video=" + opts_.device;

        // 向设备请求目标帧率;设备实际交付什么以其能力为准(请求 30fps 的摄像头
        // 可能只支持 15fps)。输出侧按墙钟实时换算 PTS,交付帧率偏低只是帧变稀,
        // 时间轴仍贴合真实时间。
        av_dict_set_int(&opts, "framerate", opts_.fps, 0);
        // 仅当调用方显式给出尺寸时才请求;默认 0 表示用设备原生分辨率。
        if (opts_.width > 0 && opts_.height > 0) {
            char video_size[64];
            std::snprintf(video_size, sizeof(video_size), "%dx%d", opts_.width,
                          opts_.height);
            av_dict_set(&opts, "video_size", video_size, 0);
        }
        // 增大 dshow 实时缓冲区(默认 25MB,1080p 原始帧约 8MB/帧,muxer 互斥锁
        // 短暂阻塞或编码慢时容易溢出丢帧)。64MB 给予足够余量。
        av_dict_set_int(&opts, "rtbufsize", 64 * 1024 * 1024, 0);

        // 预先分配输入上下文并挂上中断回调(必须在 avformat_open_input 之前设置),
        // 使停止录制能打断阻塞中的 dshow 读取。
        in_fmt_ctx_ = avformat_alloc_context();
        if (!in_fmt_ctx_) break;
        in_fmt_ctx_->interrupt_callback.callback = &DShowVideoSource::InterruptCallback;
        in_fmt_ctx_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&in_fmt_ctx_, url.c_str(), dshow, &opts);
        if (ret < 0) break;

        if (avformat_find_stream_info(in_fmt_ctx_, nullptr) < 0) break;

        in_index_ = av_find_best_stream(in_fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1,
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

bool DShowVideoSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredVideoEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) {
            break;
        }
    }
    if (!encoder) {
        return false;
    }

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) {
        return false;
    }

    // YUV420P 要求宽高均为偶数；摄像头可能交付奇数尺寸，向下对齐。
    enc_ctx_->width = dec_ctx_->width & ~1;
    enc_ctx_->height = dec_ctx_->height & ~1;
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx_->time_base = AVRational{1, opts_.fps};
    enc_ctx_->framerate = AVRational{opts_.fps, 1};
    enc_ctx_->bit_rate = opts_.bit_rate;
    enc_ctx_->gop_size = opts_.fps * 2;  // 大约每 2 秒一个关键帧。
    enc_ctx_->max_b_frames = 0;

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

bool DShowVideoSource::InitScaler() {
    sws_ctx_ = sws_getContext(dec_ctx_->width, dec_ctx_->height,
                              dec_ctx_->pix_fmt, enc_ctx_->width,
                              enc_ctx_->height, enc_ctx_->pix_fmt,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        return false;
    }

    yuv_frame_ = av_frame_alloc();
    if (!yuv_frame_) return false;
    yuv_frame_->format = enc_ctx_->pix_fmt;
    yuv_frame_->width = enc_ctx_->width;
    yuv_frame_->height = enc_ctx_->height;
    if (av_frame_get_buffer(yuv_frame_, 32) < 0) {
        return false;
    }
    return true;
}

bool DShowVideoSource::Run(double duration_seconds,
                     std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop) {
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
    int64_t frames_written = 0;   // 实际已编码的帧数。
    // 倍率录制抽帧：speed_multiplier>1 时每 N 帧保留 1 帧（与 gdigrab 一致）。
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
    int64_t seen_ = 0;
    // 上一帧的 PTS:墙钟换算撞槽时强制单调 +1(编码器要求严格单调整数 PTS)。
    int64_t prev_pts = -1;

    while (!stop.load()) {
        // 暂停期间必须持续读包丢弃：dshow 摄像头是实时推流模型，设备不断向
        // rtbufsize 缓冲推帧，不读会持续堆积并在长暂停时溢出丢包；读出即丢
        // 等效于"输入暂停"且无溢出风险。输出侧自然暂停：暂停时长计入
        // total_paused_，PTS（墙钟换算）在继续后自然接续。
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            int ret = av_read_frame(in_fmt_ctx_, in_pkt);
            if (ret >= 0) {
                av_packet_unref(in_pkt);  // 排空 dshow 缓冲。
            } else if (ret == AVERROR(EAGAIN)) {
                av_usleep(1000);
            } else {
                // AVERROR_EXIT（停止请求）或设备被拔等硬错误：跳出循环收尾。
                break;
            }
            continue;
        }

        // 真实经过时间扣除暂停总时长，用于时长上限判断。
        auto now = std::chrono::steady_clock::now();
        auto paused_dur = total_paused_;
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            paused_dur += now - pause_start_;
        }
        double elapsed = std::chrono::duration<double>(now - start - paused_dur)
                             .count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

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
        if (ret < 0) {
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ok = false;
                break;
            }

            // 倍率录制抽帧：丢弃 (N-1)/N 的帧，保留序号能被 N 整除的帧。
            ++seen_;
            if (speed > 1 && (seen_ % speed) != 0) {
                av_frame_unref(dec_frame);
                continue;
            }

            // 转换到 YUV420P。yuv_frame_ 是一次性分配、循环复用的输出缓冲
            // （编码器若需保留会自行 ref），绝不能在此 unref。
            if (av_frame_make_writable(yuv_frame_) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }
            sws_scale(sws_ctx_, dec_frame->data, dec_frame->linesize, 0,
                      dec_ctx_->height, yuv_frame_->data,
                      yuv_frame_->linesize);
            av_frame_unref(dec_frame);

            // PTS:每帧按墙钟实时换算(共享零点 + 暂停扣减),不再用帧计数。
            // 实际采集帧率达不到标称 fps 时,帧计数会让视频时间轴比真实时间
            // 越走越慢,长录制后计时/音画持续漂移;墙钟换算无累积误差。
            // 倍率模式下墙钟 N 秒压缩成 1 秒视频,故乘以 fps/speed。
            int64_t pts = std::llround(elapsed * opts_.fps / speed);
            if (pts <= prev_pts) pts = prev_pts + 1;  // 撞槽强制单调
            prev_pts = pts;
            yuv_frame_->pts = pts;

            bool wrote = writer_->EncodeAndWrite(enc_ctx_, stream_, yuv_frame_);
            if (!wrote) {
                ok = false;
                break;
            }
            ++frames_written;
        }
        if (!ok) break;
    }

    // 无论循环正常结束还是中途出错，都向编码器送 NULL 冲刷残余帧，尽量把已
    // 编码画面落盘——这是提前终止时文件仍可播放的关键。
    if (!writer_->EncodeAndWrite(enc_ctx_, stream_, nullptr)) ok = false;

    av_packet_free(&in_pkt);
    av_frame_free(&dec_frame);
    return ok;
}

void DShowVideoSource::Close() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (yuv_frame_) av_frame_free(&yuv_frame_);
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
