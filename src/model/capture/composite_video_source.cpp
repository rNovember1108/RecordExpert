#include "composite_video_source.h"

#include <cstring>
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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace re {
namespace {

// 纯软件编码器，不依赖 COM（与 GdiGrabVideoSource 同选型理由）。
const char* kPreferredVideoEncoders[] = {"mpeg4"};

}  // namespace

CompositeVideoSource::CompositeVideoSource(VideoOptions options, Mp4Writer* writer)
    : opts_(options), writer_(writer) {}

CompositeVideoSource::~CompositeVideoSource() { Close(); }

int CompositeVideoSource::InterruptCallback(void* opaque) {
    auto* self = static_cast<CompositeVideoSource*>(opaque);
    return (self && self->stop_flag_ && self->stop_flag_->load()) ? 1 : 0;
}

bool CompositeVideoSource::Open() {
    avdevice_register_all();
    if (opts_.secondary_device.empty()) return false;
    if (!OpenMainInput()) return false;
    if (!OpenCameraInput()) { Close(); return false; }
    if (!OpenEncoder())    { Close(); return false; }
    if (!InitScalers())    { Close(); return false; }
    return true;
}

// gdigrab 桌面输入(与 GdiGrabVideoSource::OpenInput 同逻辑,不含水印滤镜)。
bool CompositeVideoSource::OpenMainInput() {
    bool ok = false;
    AVDictionary* opts = nullptr;
    do {
        const AVInputFormat* gdigrab = av_find_input_format("gdigrab");
        if (!gdigrab) break;
        av_dict_set_int(&opts, "framerate", opts_.fps, 0);
        // 区域录制时设置偏移/尺寸;窗口模式与全屏模式不设。
        if (opts_.window_title.empty() && opts_.width > 0 && opts_.height > 0) {
            av_dict_set_int(&opts, "offset_x", opts_.offset_x, 0);
            av_dict_set_int(&opts, "offset_y", opts_.offset_y, 0);
            char video_size[64];
            std::snprintf(video_size, sizeof(video_size), "%dx%d",
                          opts_.width, opts_.height);
            av_dict_set(&opts, "video_size", video_size, 0);
        }
        av_dict_set(&opts, "draw_mouse", "1", 0);

        std::string input_url;
        if (!opts_.window_title.empty()) {
            input_url = "title=" + opts_.window_title;
        } else {
            input_url = "desktop";
        }

        main_in_ = avformat_alloc_context();
        if (!main_in_) break;
        main_in_->interrupt_callback.callback = &CompositeVideoSource::InterruptCallback;
        main_in_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&main_in_, input_url.c_str(), gdigrab, &opts);
        if (ret < 0) break;
        if (avformat_find_stream_info(main_in_, nullptr) < 0) break;
        main_idx_ = av_find_best_stream(main_in_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                        nullptr, 0);
        if (main_idx_ < 0) break;
        AVStream* in_stream = main_in_->streams[main_idx_];
        const AVCodec* decoder =
            avcodec_find_decoder(in_stream->codecpar->codec_id);
        if (!decoder) break;
        main_dec_ = avcodec_alloc_context3(decoder);
        if (!main_dec_) break;
        avcodec_parameters_to_context(main_dec_, in_stream->codecpar);
        if (avcodec_open2(main_dec_, decoder, nullptr) < 0) break;

        ok = true;
    } while (false);
    av_dict_free(&opts);
    if (!ok) { Close(); return false; }
    return true;
}

// dshow 摄像头输入(与 DShowVideoSource::OpenInput 同逻辑,不含缩放/编码)。
bool CompositeVideoSource::OpenCameraInput() {
    bool ok = false;
    AVDictionary* opts = nullptr;
    do {
        const AVInputFormat* dshow = av_find_input_format("dshow");
        if (!dshow) break;
        std::string url = "video=" + opts_.secondary_device;
        av_dict_set_int(&opts, "framerate", opts_.fps, 0);
        av_dict_set_int(&opts, "rtbufsize", 64 * 1024 * 1024, 0);

        cam_in_ = avformat_alloc_context();
        if (!cam_in_) break;
        cam_in_->interrupt_callback.callback = &CompositeVideoSource::InterruptCallback;
        cam_in_->interrupt_callback.opaque = this;

        int ret = avformat_open_input(&cam_in_, url.c_str(), dshow, &opts);
        if (ret < 0) break;
        if (avformat_find_stream_info(cam_in_, nullptr) < 0) break;
        cam_idx_ = av_find_best_stream(cam_in_, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       nullptr, 0);
        if (cam_idx_ < 0) break;
        AVStream* in_stream = cam_in_->streams[cam_idx_];
        const AVCodec* decoder =
            avcodec_find_decoder(in_stream->codecpar->codec_id);
        if (!decoder) break;
        cam_dec_ = avcodec_alloc_context3(decoder);
        if (!cam_dec_) break;
        avcodec_parameters_to_context(cam_dec_, in_stream->codecpar);
        if (avcodec_open2(cam_dec_, decoder, nullptr) < 0) break;

        ok = true;
    } while (false);
    av_dict_free(&opts);
    if (!ok) { Close(); return false; }
    return true;
}

bool CompositeVideoSource::OpenEncoder() {
    const AVCodec* encoder = nullptr;
    for (const char* name : kPreferredVideoEncoders) {
        encoder = avcodec_find_encoder_by_name(name);
        if (encoder) break;
    }
    if (!encoder) return false;
    enc_ctx_ = avcodec_alloc_context3(encoder);
    if (!enc_ctx_) return false;
    // YUV420P 要求宽高偶数；桌面尺寸向下对齐。
    enc_ctx_->width = main_dec_->width & ~1;
    enc_ctx_->height = main_dec_->height & ~1;
    enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx_->time_base = AVRational{1, opts_.fps};
    enc_ctx_->framerate = AVRational{opts_.fps, 1};
    enc_ctx_->bit_rate = opts_.bit_rate;
    enc_ctx_->gop_size = opts_.fps * 2;
    enc_ctx_->max_b_frames = 0;
    if (writer_->NeedsGlobalHeader()) {
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    int ret = avcodec_open2(enc_ctx_, encoder, nullptr);
    if (ret < 0) return false;
    stream_ = writer_->NewStream(enc_ctx_);
    return stream_ != nullptr;
}

bool CompositeVideoSource::InitScalers() {
    // 主缩放：桌面解码帧 → 主 YUV420P（编码尺寸）。
    sws_main_ = sws_getContext(main_dec_->width, main_dec_->height,
                               main_dec_->pix_fmt, enc_ctx_->width,
                               enc_ctx_->height, enc_ctx_->pix_fmt,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_main_) return false;
    // 画中画尺寸 = 主画面 1/4，向下对齐偶数；位置右下角留 16px 边距，对齐偶数
    // （YUV420P 色度平面是 1/2，坐标必须偶数）。
    pip_w_ = (enc_ctx_->width / 4) & ~1;
    pip_h_ = (enc_ctx_->height / 4) & ~1;
    if (pip_w_ < 2) pip_w_ = 2;
    if (pip_h_ < 2) pip_h_ = 2;
    pip_x_ = ((enc_ctx_->width - pip_w_ - 16) & ~1);
    pip_y_ = ((enc_ctx_->height - pip_h_ - 16) & ~1);
    if (pip_x_ < 0) pip_x_ = 0;
    if (pip_y_ < 0) pip_y_ = 0;

    // 摄像头缩放：摄像头解码帧 → PiP 小尺寸 YUV420P。
    sws_cam_ = sws_getContext(cam_dec_->width, cam_dec_->height,
                              cam_dec_->pix_fmt, pip_w_, pip_h_,
                              AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                              nullptr, nullptr, nullptr);
    if (!sws_cam_) return false;
    cam_small_ = av_frame_alloc();
    if (!cam_small_) return false;
    cam_small_->format = AV_PIX_FMT_YUV420P;
    cam_small_->width = pip_w_;
    cam_small_->height = pip_h_;
    if (av_frame_get_buffer(cam_small_, 32) < 0) return false;

    yuv_frame_ = av_frame_alloc();
    if (!yuv_frame_) return false;
    yuv_frame_->format = enc_ctx_->pix_fmt;
    yuv_frame_->width = enc_ctx_->width;
    yuv_frame_->height = enc_ctx_->height;
    if (av_frame_get_buffer(yuv_frame_, 32) < 0) return false;
    return true;
}

// 摄像头采集线程：持续读取、解码、缩放并更新最新帧槽（drop-old）。
// 暂停期间读包丢弃防 rtbufsize 溢出；停止时中断回调让 av_read_frame 返回
// AVERROR_EXIT 退出循环。
void CompositeVideoSource::CameraThread() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* dec = av_frame_alloc();
    AVFrame* tmp = av_frame_alloc();  // sws_cam 输出临时帧。
    if (!pkt || !dec || !tmp) {
        av_packet_free(&pkt);
        av_frame_free(&dec);
        av_frame_free(&tmp);
        return;
    }
    tmp->format = AV_PIX_FMT_YUV420P;
    tmp->width = pip_w_;
    tmp->height = pip_h_;
    if (av_frame_get_buffer(tmp, 32) < 0) {
        av_packet_free(&pkt);
        av_frame_free(&dec);
        av_frame_free(&tmp);
        return;
    }

    while (!cam_stop_.load() && stop_flag_ && !stop_flag_->load()) {
        // 暂停期间读包丢弃，防 dshow 缓冲溢出。
        if (paused_.load()) {
            int ret = av_read_frame(cam_in_, pkt);
            if (ret >= 0) {
                av_packet_unref(pkt);
            } else if (ret == AVERROR(EAGAIN)) {
                av_usleep(1000);
            } else {
                break;  // AVERROR_EXIT / 硬错误：退出线程。
            }
            continue;
        }
        int ret = av_read_frame(cam_in_, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { av_usleep(1000); continue; }
            if (ret == AVERROR_EXIT) break;
            break;
        }
        if (pkt->stream_index != cam_idx_) {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(cam_dec_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;
        while (ret >= 0) {
            ret = avcodec_receive_frame(cam_dec_, dec);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            // 缩放到 PiP 小尺寸。
            if (av_frame_make_writable(tmp) < 0) {
                av_frame_unref(dec);
                break;
            }
            sws_scale(sws_cam_, dec->data, dec->linesize, 0,
                      cam_dec_->height, tmp->data, tmp->linesize);
            av_frame_unref(dec);
            // drop-old：拷贝到 cam_small_（受锁保护）。
            {
                std::lock_guard<std::mutex> lk(cam_mutex_);
                av_image_copy(cam_small_->data, cam_small_->linesize,
                              const_cast<const uint8_t* const*>(tmp->data),
                              tmp->linesize, AV_PIX_FMT_YUV420P,
                              pip_w_, pip_h_);
                cam_has_frame_ = true;
            }
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&dec);
    av_frame_free(&tmp);
}

// 把 cam_small_（最新摄像头小尺寸 YUV 帧）blit 到 yuv_frame_ 右下角 PiP 区。
void CompositeVideoSource::BlitPiP() {
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(cam_mutex_);
        has = cam_has_frame_;
        if (!has) return;
        // Y 平面（全分辨率）逐行拷贝。
        for (int row = 0; row < pip_h_; ++row) {
            std::memcpy(
                yuv_frame_->data[0] + (pip_y_ + row) * yuv_frame_->linesize[0]
                    + pip_x_,
                cam_small_->data[0] + row * cam_small_->linesize[0],
                static_cast<size_t>(pip_w_));
        }
        // U/V 平面（半分辨率）。
        const int hw = pip_w_ / 2;
        const int hh = pip_h_ / 2;
        const int cx = pip_x_ / 2;
        const int cy = pip_y_ / 2;
        for (int row = 0; row < hh; ++row) {
            std::memcpy(
                yuv_frame_->data[1] + (cy + row) * yuv_frame_->linesize[1] + cx,
                cam_small_->data[1] + row * cam_small_->linesize[1],
                static_cast<size_t>(hw));
            std::memcpy(
                yuv_frame_->data[2] + (cy + row) * yuv_frame_->linesize[2] + cx,
                cam_small_->data[2] + row * cam_small_->linesize[2],
                static_cast<size_t>(hw));
        }
    }
}

bool CompositeVideoSource::Run(double duration_seconds,
             std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) {
    stop_flag_ = &stop;

    AVPacket* in_pkt = av_packet_alloc();
    AVFrame* dec_frame = av_frame_alloc();
    if (!in_pkt || !dec_frame) {
        av_packet_free(&in_pkt);
        av_frame_free(&dec_frame);
        return false;
    }

    // 启动摄像头采集线程（drop-old 最新帧槽）。
    cam_stop_.store(false);
    cam_thread_ = std::thread(&CompositeVideoSource::CameraThread, this);

    bool ok = true;
    int64_t frames_written = 0;
    const int speed = (opts_.speed_multiplier >= 1) ? opts_.speed_multiplier : 1;
    int64_t seen_ = 0;
    // 上一帧的 PTS:墙钟换算撞槽时强制单调 +1(编码器要求严格单调整数 PTS)。
    int64_t prev_pts = -1;

    while (!stop.load()) {
        if (paused_.load()) {
            if (pause_start_ == std::chrono::steady_clock::time_point{}) {
                pause_start_ = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        auto now = std::chrono::steady_clock::now();
        auto paused_dur = total_paused_;
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            paused_dur += now - pause_start_;
        }
        double elapsed = std::chrono::duration<double>(now - start - paused_dur)
                             .count();
        if (duration_seconds > 0 && elapsed >= duration_seconds) break;

        int ret = av_read_frame(main_in_, in_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { av_usleep(1000); continue; }
            if (ret == AVERROR_EXIT) break;
            break;
        }
        if (in_pkt->stream_index != main_idx_) {
            av_packet_unref(in_pkt);
            continue;
        }
        ret = avcodec_send_packet(main_dec_, in_pkt);
        av_packet_unref(in_pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(main_dec_, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ok = false;
                break;
            }
            // 倍率抽帧。
            ++seen_;
            if (speed > 1 && (seen_ % speed) != 0) {
                av_frame_unref(dec_frame);
                continue;
            }
            // 桌面帧水印（与 GdiGrabVideoSource 回退路径一致）。
            if (av_frame_make_writable(dec_frame) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }
            re::DrawLabelWatermark(dec_frame, re::kWatermarkLabel);
            re::DrawTimestampWatermark(dec_frame);

            // 桌面 → 主 YUV420P。
            if (av_frame_make_writable(yuv_frame_) < 0) {
                av_frame_unref(dec_frame);
                ok = false;
                break;
            }
            sws_scale(sws_main_, dec_frame->data, dec_frame->linesize, 0,
                      main_dec_->height, yuv_frame_->data,
                      yuv_frame_->linesize);
            av_frame_unref(dec_frame);

            // 叠加摄像头画中画。
            BlitPiP();

            // PTS:每帧按墙钟实时换算(共享零点 + 暂停扣减),不再用帧计数。
            // 实际采集帧率达不到标称 fps 时,帧计数会让视频时间轴比真实时间
            // 越走越慢,长录制后计时/音画持续漂移;墙钟换算无累积误差。
            // 倍率模式下墙钟 N 秒压缩成 1 秒视频,故乘以 fps/speed。
            int64_t pts = std::llround(elapsed * opts_.fps / speed);
            if (pts <= prev_pts) pts = prev_pts + 1;  // 撞槽强制单调
            prev_pts = pts;
            yuv_frame_->pts = pts;

            if (!writer_->EncodeAndWrite(enc_ctx_, stream_, yuv_frame_)) {
                ok = false;
                break;
            }
            ++frames_written;
        }
        if (!ok) break;
    }

    // 停止摄像头线程。
    cam_stop_.store(true);
    if (cam_thread_.joinable()) cam_thread_.join();

    if (!writer_->EncodeAndWrite(enc_ctx_, stream_, nullptr)) ok = false;

    av_packet_free(&in_pkt);
    av_frame_free(&dec_frame);
    return ok;
}

void CompositeVideoSource::Close() {
    cam_stop_.store(true);
    if (cam_thread_.joinable()) cam_thread_.join();
    if (sws_main_) { sws_freeContext(sws_main_); sws_main_ = nullptr; }
    if (sws_cam_)  { sws_freeContext(sws_cam_);  sws_cam_  = nullptr; }
    if (yuv_frame_) av_frame_free(&yuv_frame_);
    if (cam_small_) av_frame_free(&cam_small_);
    if (main_dec_) avcodec_free_context(&main_dec_);
    if (cam_dec_)  avcodec_free_context(&cam_dec_);
    if (enc_ctx_)  avcodec_free_context(&enc_ctx_);
    if (main_in_)  { avformat_close_input(&main_in_); main_in_ = nullptr; }
    if (cam_in_)   { avformat_close_input(&cam_in_);  cam_in_  = nullptr; }
    stream_ = nullptr;
}

}  // namespace re
