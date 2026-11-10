#include "gdi_grab_video_source.h"

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include "Mp4Writer.h"
#include "Watermark.h"

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

// 视频编码器。GUI 模式下 QGuiApplication 会把主线程初始化为 COM STA，而
// Media Foundation 编码器（h264_mf）在 STA 线程上 avcodec_send_frame 必定
// 失败；libx264 在本 FFmpeg 构建中不可用。因此直接使用纯软件的 mpeg4 编码器，
// 不依赖 COM，GUI 下稳定可用。
const char* kPreferredVideoEncoders[] = {"mpeg4"};

}  // namespace

GdiGrabVideoSource::GdiGrabVideoSource(VideoOptions options, Mp4Writer* writer)
    : opts_(options), writer_(writer) {}

GdiGrabVideoSource::~GdiGrabVideoSource() { Close(); }

// static：FFmpeg 在阻塞式采集等待期间会反复轮询此回调；返回 1 即中止本次 I/O，
// av_read_frame 随之返回 AVERROR_EXIT。打开设备阶段 stop_flag_ 尚未设置（为
// nullptr），此时始终放行，保持原有行为。
int GdiGrabVideoSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<GdiGrabVideoSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool GdiGrabVideoSource::Open() {
    // gdigrab 位于 libavdevice，必须显式注册。可多次调用（音频轨也会注册）。
    avdevice_register_all();

    if (!OpenInput()) return false;
    if (!OpenEncoder()) { Close(); return false; }
    if (!InitScaler()) { Close(); return false; }
    return true;
}

bool GdiGrabVideoSource::OpenInput() {
    // do-while(false) 模式:中途任何步骤失败都 break,跳出后统一 Close() 清理。
    // Close() 幂等,对所有成员做了 null 检查,即使什么资源都没分配也能安全调用。
    bool ok = false;
    AVDictionary* opts = nullptr;
    do {
        const AVInputFormat* gdigrab = av_find_input_format("gdigrab");
        if (!gdigrab) break;

        // gdigrab 选项:帧率、采集区域(区域录制时有效),以及可选的窗口标题。
        av_dict_set_int(&opts, "framerate", opts_.fps, 0);
        // 仅区域录制(width/height > 0 且非窗口模式)时才设置偏移/尺寸。
        // 窗口模式下 gdigrab 的 title= 会忽略 video_size 字段,硬设也无妨但冗余。
        if (opts_.window_title.empty() && opts_.width > 0 && opts_.height > 0) {
            av_dict_set_int(&opts, "offset_x", opts_.offset_x, 0);
            av_dict_set_int(&opts, "offset_y", opts_.offset_y, 0);
            char video_size[64];
            std::snprintf(video_size, sizeof(video_size), "%dx%d", opts_.width,
                          opts_.height);
            av_dict_set(&opts, "video_size", video_size, 0);
        }
        av_dict_set(&opts, "draw_mouse", "1", 0);

        // 决定 gdigrab 输入 URL:
        //   空 window_title → "desktop"(全屏或区域,由 video_size 决定)
        //   非空            → "title=<窗口标题>"(指定窗口)
        std::string input_url;
        if (!opts_.window_title.empty()) {
            input_url = "title=" + opts_.window_title;
        } else {
            input_url = "desktop";
        }

        // 预先分配输入上下文并挂上中断回调(必须在 avformat_open_input 之前设置)。
        // 否则停止录制时若 gdigrab 暂时无帧可给,av_read_frame 会无限阻塞,线程
        // 无法退出,麦克风设备也会因迟迟不关闭而实时缓冲区暴涨、丢帧。
        in_fmt_ctx_ = avformat_alloc_context();
        if (!in_fmt_ctx_) break;
        in_fmt_ctx_->interrupt_callback.callback = &GdiGrabVideoSource::InterruptCallback;
        in_fmt_ctx_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&in_fmt_ctx_, input_url.c_str(), gdigrab, &opts);
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

    // opts 在循环后统一释放(avformat_open_input 成功后会内部 dict_free &opts,
    // 但我们自己的副本必须手动 av_dict_free;失败时 &opts 也需要释放)。
    av_dict_free(&opts);
    if (!ok) { Close(); return false; }
    return true;
}

bool GdiGrabVideoSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredVideoEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;

    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;

    // YUV420P 要求 width/height 均为偶数(色度平面是亮度的一半)。
    // 区域/窗口录制的尺寸可能为奇数,导致编码器输出不标准流,
    // 播放器报"缺少编解码器"。这里向下对齐到最近的偶数。
    enc_ctx_->width = dec_ctx_->width & ~1;   // 奇数 → 偶数(截断 1px)
    enc_ctx_->height = dec_ctx_->height & ~1;
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx_->time_base = AVRational{1, opts_.fps};
    enc_ctx_->framerate = AVRational{opts_.fps, 1};
    enc_ctx_->bit_rate = opts_.bit_rate;
    enc_ctx_->gop_size = opts_.fps * 2;  // 大约每 2 秒一个关键帧。
    enc_ctx_->max_b_frames = 0;

    av_opt_set(enc_ctx_->priv_data, "preset", "veryfast", 0);
    av_opt_set(enc_ctx_->priv_data, "tune", "zerolatency", 0);

    if (writer_->NeedsGlobalHeader()) {
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) return false;

    stream_ = writer_->NewStream(enc_ctx_);
    return stream_ != nullptr;
}

bool GdiGrabVideoSource::InitScaler() {
    sws_ctx_ = sws_getContext(dec_ctx_->width, dec_ctx_->height,
                              dec_ctx_->pix_fmt, enc_ctx_->width,
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

bool GdiGrabVideoSource::Run(double duration_seconds,
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
    // 倍率录制（time-lapse）抽帧计数：speed_multiplier>1 时每 N 帧保留 1 帧，
    // 把 N 秒墙钟内容压缩到 1 秒 mp4 输出。seen_ 累计所有解码出来的帧。
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
    int64_t seen_ = 0;
    // 上一帧的 PTS:墙钟换算撞槽时强制单调 +1(编码器要求严格单调整数 PTS)。
    int64_t prev_pts = -1;

    while (!stop.load()) {
        // 暂停期间不调用 av_read_frame、不抓帧、不写包；只在首次进入时记录暂停
        // 起点以便后续扣除暂停时长。gdigrab 是按需 BitBlt，不抓即不发新帧，
        // 无内部缓冲堆积。
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 真实经过时间扣除暂停总时长，使视频时间轴贴合"实际录制墙钟"。
        auto now = std::chrono::steady_clock::now();
        auto paused_dur = total_paused_;
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            // 理论上此处 paused_ 已为 false，pause_start_ 应在 Resume 中被清空；
            // 此分支仅为防御性兜底。
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
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(dec_ctx_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ok = false;
                break;
            }

            // 倍率录制抽帧：speed_multiplier>1 时丢弃 (N-1)/N 的帧，仅保留序号
            // 能被 N 整除的帧。丢弃的帧立即 unref 释放并继续内层循环。N=1 时
            // 全部保留，行为与原 1:1 实时录制完全一致。
            ++seen_;
            if (speed > 1 && (seen_ % speed) != 0) {
                av_frame_unref(dec_frame);
                continue;
            }

            AVFrame* out_frame = nullptr;  // 最终送入编码器的帧。

            // ---- 水印（QPainter 直接画在采集帧上）+ sws_scale 转换 ----
            if (av_frame_make_writable(dec_frame) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }
            re::DrawLabelWatermark(dec_frame, kWatermarkLabel);
            re::DrawTimestampWatermark(dec_frame);

            if (av_frame_make_writable(yuv_frame_) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }
            sws_scale(sws_ctx_, dec_frame->data, dec_frame->linesize, 0,
                      dec_ctx_->height, yuv_frame_->data,
                      yuv_frame_->linesize);
            av_frame_unref(dec_frame);
            out_frame = yuv_frame_;

            // PTS:每帧按墙钟实时换算(共享零点 + 暂停扣减),不再用帧计数。
            // 实际采集帧率达不到标称 fps 时,帧计数会让视频时间轴比真实时间
            // 越走越慢,长录制后计时/音画持续漂移;墙钟换算无累积误差。
            // 倍率模式下墙钟 N 秒压缩成 1 秒视频,故乘以 fps/speed。
            int64_t pts = std::llround(elapsed * opts_.fps / speed);
            if (pts <= prev_pts) pts = prev_pts + 1;  // 撞槽强制单调
            prev_pts = pts;
            out_frame->pts = pts;

            bool wrote = writer_->EncodeAndWrite(enc_ctx_, stream_, out_frame);
            // yuv_frame_ 是 InitScaler() 里一次性分配、每帧循环复用的缩放输出
            // 缓冲（编码器若需保留会自行 av_frame_ref），绝不能在这里 unref——
            // 否则底层数据缓冲第一帧后即被释放，第二帧 av_frame_make_writable/
            // sws_scale 必然失败，表现为"只录到 1 帧视频"。
            if (!wrote) {
                ok = false;
                break;
            }
            ++frames_written;
        }
        if (!ok) break;
    }

    // 无论循环是正常结束还是中途出错，都向编码器送 NULL 冲刷残余帧（编码器可能
    // 缓存若干帧），尽量把已经编码的画面落盘——这是提前终止时文件仍可播放的
    // 关键。冲刷本身失败再把结果标记为失败。
    if (!writer_->EncodeAndWrite(enc_ctx_, stream_, nullptr)) ok = false;

    av_packet_free(&in_pkt);
    av_frame_free(&dec_frame);
    return ok;
}

void GdiGrabVideoSource::Close() {
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
