#ifndef RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H
#define RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>

#include "i_video_source.h"

// 前置声明 FFmpeg 类型，使调用方无需包含 FFmpeg 头文件。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;

namespace re {

class Mp4Writer;

// GdiGrabVideoSource 是 IVideoSource 的 gdigrab 具体实现：通过 FFmpeg 的 gdigrab
// 设备采集 Windows 桌面，用 libswscale 转换成 YUV420P，编码为 mpeg4（GUI 模式下
// h264_mf 与 QGuiApplication 的 COM STA 冲突），再经共享的 Mp4Writer 写包。
//
// 本类刻意对音频一无所知，只与音频侧共享两样东西：Mp4Writer（输出文件）和一个
// 用于打真实时间 PTS 的共同墙钟零点。水印复用公共模块 Watermark.h（QPainter
// 直接绘制在采集帧上），与截图链路共用同一份水印代码。
class GdiGrabVideoSource : public IVideoSource {
public:
    GdiGrabVideoSource(VideoOptions options, Mp4Writer* writer);
    ~GdiGrabVideoSource() override;

    GdiGrabVideoSource(const GdiGrabVideoSource&) = delete;
    GdiGrabVideoSource& operator=(const GdiGrabVideoSource&) = delete;

    bool Open() override;
    bool Run(double duration_seconds,
             std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) override;
    void Close() override;

    void Pause() override { paused_.store(true); }
    void Resume() override {
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            total_paused_ += std::chrono::steady_clock::now() - pause_start_;
            pause_start_ = std::chrono::steady_clock::time_point{};
        }
        paused_.store(false);
    }

private:
    bool OpenInput();
    bool OpenEncoder();  // 创建编码器，并创建输出视频流。
    bool InitScaler();   // sws_scale 转换为编码器像素格式（水印在采集帧上由
                         // QPainter 直接绘制，见 Watermark.h）。

    // FFmpeg I/O 中断回调：停止标志置位后让阻塞在 av_read_frame 里的
    // gdigrab 采集立即返回，而不是无限期等下一帧。
    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    VideoOptions opts_;
    Mp4Writer*  writer_ = nullptr;  // 不持有所有权。

    // ---- 输入（gdigrab）----
    AVFormatContext* in_fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_    = nullptr;
    int              in_index_   = -1;

    // ---- 编码器 / 流 / 缩放器 ----
    AVCodecContext* enc_ctx_   = nullptr;
    AVStream*       stream_    = nullptr;
    SwsContext*     sws_ctx_   = nullptr;   // sws_scale 转换用。
    AVFrame*        yuv_frame_ = nullptr;    // sws 输出（每帧循环复用）。

    // ---- 暂停状态 ----
    // 暂停标志置位后，Run 循环顶部跳过采集与编码，仅 sleep；继续时累加暂停时长
    // 以修正 PTS。
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H
