#include "Screenshot.h"

#include <cstdio>
#include <string>

#include <QImage>

#include "Watermark.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

namespace re {

bool CaptureDesktopFrame(AVFrame** out_frame,
                         const ScreenshotOptions& opts) {
    if (!out_frame) return false;
    // gdigrab 位于 libavdevice，必须显式注册。
    avdevice_register_all();

    // 各阶段资源统一在 cleanup 中释放（每个指针都允许为 null）。
    AVFormatContext* in_fmt  = nullptr;  // gdigrab 桌面输入。
    AVCodecContext*  dec_ctx = nullptr;
    AVPacket*        pkt     = nullptr;
    AVFrame*         frame   = nullptr;  // 解码输出（BGRA/BGR0 打包 RGB）。

    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec_ctx);
        if (in_fmt) avformat_close_input(&in_fmt);
    };

    // ---- 1. 打开 gdigrab 桌面输入（参数与 GdiGrabVideoSource::OpenInput 一致）----
    const AVInputFormat* gdigrab = av_find_input_format("gdigrab");
    if (!gdigrab) {
        cleanup();
        return false;
    }

    AVDictionary* in_opts = nullptr;
    // 单帧抓取，帧率值不敏感，取与录屏一致的默认值。
    av_dict_set_int(&in_opts, "framerate", 30, 0);
    av_dict_set_int(&in_opts, "offset_x", opts.offset_x, 0);
    av_dict_set_int(&in_opts, "offset_y", opts.offset_y, 0);
    if (opts.width > 0 && opts.height > 0) {
        char video_size[64];
        std::snprintf(video_size, sizeof(video_size), "%dx%d", opts.width,
                      opts.height);
        av_dict_set(&in_opts, "video_size", video_size, 0);
    }
    // 截图不含鼠标光标:区域截图背景由 QML 十字准星接管,普通截图也不需要
    // 烤入光标。录屏走独立的 GdiGrabVideoSource,draw_mouse 不受影响。
    av_dict_set(&in_opts, "draw_mouse", "0", 0);

    int ret = avformat_open_input(&in_fmt, "desktop", gdigrab, &in_opts);
    av_dict_free(&in_opts);
    if (ret < 0) {
        cleanup();
        return false;
    }

    // 不调用 avformat_find_stream_info:gdigrab 在 read_header 里已经把
    // codecpar->codec_id(AV_CODEC_ID_BMP)填好,BMP 解码参数(宽高/像素格式)
    // 从帧数据本身解析,无需 probe。这一步会额外读一整帧(8MB packet)+
    // probe 缓冲,白白推高 32 位进程的堆压力,还引入 33ms 帧间隔等待。
    int in_index = av_find_best_stream(in_fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       nullptr, 0);
    if (in_index < 0) {
        cleanup();
        return false;
    }

    AVStream* in_stream = in_fmt->streams[in_index];
    const AVCodec* decoder =
        avcodec_find_decoder(in_stream->codecpar->codec_id);
    if (!decoder) {
        cleanup();
        return false;
    }
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        cleanup();
        return false;
    }
    avcodec_parameters_to_context(dec_ctx, in_stream->codecpar);
    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
        cleanup();
        return false;
    }

    // ---- 2. 读取并解码第一帧 ----
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        cleanup();
        return false;
    }

    int failures = 0;
    while (true) {
        ret = av_read_frame(in_fmt, pkt);
        if (ret < 0) {
            cleanup();
            return false;
        }
        if (pkt->stream_index != in_index) {
            av_packet_unref(pkt);
            continue;
        }
        ret = avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            // 个别包解码失败可以容忍，但设置上限避免病态死循环。
            if (++failures > 100) {
                cleanup();
                return false;
            }
            continue;
        }
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == 0) break;
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            cleanup();
            return false;
        }
    }

    // 把 frame 所有权移交给调用方：抽出本地指针置 null，cleanup 不再 free 它。
    *out_frame = frame;
    frame = nullptr;
    cleanup();
    return true;
}

QImage FrameToQImage(const AVFrame* frame) {
    if (!frame || !frame->data[0] || frame->width <= 0 || frame->height <= 0) {
        return QImage();
    }
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    if (fmt != AV_PIX_FMT_BGRA && fmt != AV_PIX_FMT_BGR0) {
        return QImage();
    }
    // 零拷贝构造 Format_RGB32（与 BGRX 内存布局一致：0x00RRGGBB，
    // QImage 把 32 位字解释为 0xAARRGGBB，但 alpha 在源里是 0xFF/0x00
    // 都无所谓——保存 PNG 时 Qt 用 Format_RGB32 不写 alpha 通道）。
    // 构造后立即 .copy() 把像素深拷贝出独立缓冲，使返回的 QImage 不依赖
    // frame->data[0]，调用方随后可立即 av_frame_free。
    QImage shallow(frame->data[0], frame->width, frame->height,
                   frame->linesize[0], QImage::Format_RGB32);
    return shallow.copy();
}

bool CapturePngScreenshot(const std::string& png_path,
                          const ScreenshotOptions& opts) {
    AVFrame* frame = nullptr;
    if (!CaptureDesktopFrame(&frame, opts)) return false;

    // 水印：与录屏保持一致（左上角 'test' 标签 + 右下角时间戳），
    // gdigrab 输出 BGRX 打包格式，可直接绘制。
    if (opts.draw_timestamp) {
        DrawLabelWatermark(frame, kWatermarkLabel);
        DrawTimestampWatermark(frame);
    }

    // 转 QImage 后用 Qt 内建 PNG 支持编码落盘（QString::fromStdString 按
    // UTF-8 处理路径，中文路径可用）。PNG 编码器内建于 Qt5Gui，Windows/Linux
    // 共用同一实现，无需系统编码组件。img 为深拷贝，释放 frame 不影响保存。
    const QImage img = FrameToQImage(frame);
    av_frame_free(&frame);
    if (img.isNull()) return false;
    return img.save(QString::fromStdString(png_path), "PNG");
}

}  // namespace re
