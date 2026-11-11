#ifndef RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H
#define RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "i_video_source.h"

// 前置声明 FFmpeg 类型。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace re {

// GdiGrabVideoSource 采用 Windows GDI BitBlt 抓屏 + FFmpeg gdigrab avdevice 封装。
// 本类只做 producer：采集 → 解码 → sws_scale → 水印（可选）→ send_frame/receive_packet
// → push AVFifo。分段切换/PTS 偏移/WritePacket 全在 muxer_consumer 侧。
//
// ffmpeg 的 Media Foundation 编码器（h264_mf）在 STA 线程上 avcodec_send_frame
// 必定失败；libx264 在本 FFmpeg 构建中不可用。因此直接使用纯软件的 mpeg4 编码器，
// 与具体运行时无关。
class GdiGrabVideoSource : public IVideoSource {
public:
    explicit GdiGrabVideoSource(VideoOptions options);
    ~GdiGrabVideoSource() override;

    GdiGrabVideoSource(const GdiGrabVideoSource&) = delete;
    GdiGrabVideoSource& operator=(const GdiGrabVideoSource&) = delete;

    // producer 侧注入（由 ScreenRecorder 在 Open 之前/之后分别调用）。
    void SetQueue(AVFifo* queue) override { queue_ = queue; }
    void SetStream(AVStream* stream) override { stream_ = stream; }
    void SetFlushDoneFlag(std::atomic<bool>* flag) override { flush_done_ = flag; }

    AVCodecContext* EncCtx() const override { return enc_ctx_; }

    bool Open() override;
    bool Run(double duration_seconds,
             std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop,
             const std::atomic<bool>& flush_requested) override;
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
    bool OpenMainInput();   // 打开 gdigrab
    bool OpenEncoder();     // 建 enc_ctx_ + open2（producer 全程持有，不重建）
    bool InitScaler();      // BGRA → mpeg4 的 YUV 输入格式（PIX_FMT_YUV420P）

    // encoder → queue 封装（Run 循环里每帧调用；flush 时 frame=nullptr）。
    // receive_packet 出来的 packet 会被 rescale 到 stream_->time_base 并 push queue_。
    // 返回 true 表示成功（包括 send EAGAIN 继续的情况）。
    bool EncodeFrameToQueue(AVFrame* frame);
    bool FlushEncoderToQueue();

    // FFmpeg I/O 中断回调。
    static int InterruptCallback(void* opaque);

    VideoOptions opts_;

    // Run 期间的停止请求指针（InterruptCallback 用它中断阻塞的 av_read_frame）。
    const std::atomic<bool>* stop_flag_ = nullptr;

    AVFifo* queue_ = nullptr;            // 输出队列（ScreenRecorder 注入）
    AVStream* stream_ = nullptr;         // muxer 上的流指针（Rotate 后刷新）
    std::atomic<bool>* flush_done_ = nullptr;  // flush 完 store(true) 通知 muxer

    AVFormatContext* main_in_ = nullptr; // gdigrab 输入
    int              main_idx_ = -1;
    AVCodecContext*  main_dec_ = nullptr;

    AVCodecContext* enc_ctx_ = nullptr;  // 编码器（producer 全程持有，不重建）
    SwsContext*     sws_ctx_ = nullptr;

    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_GDI_GRAB_VIDEO_SOURCE_H
