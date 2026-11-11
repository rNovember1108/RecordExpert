#ifndef RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H
#define RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include "i_video_source.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;

namespace re {

// CompositeVideoSource 是 IVideoSource 的"桌面 + 摄像头画中画"具体实现：
// 主源为 gdigrab 桌面（device 固定 "desktop"），副源为 dshow 摄像头
// （secondary_device）。摄像头以独立线程持续采集，仅保留最新一帧（drop-old
// 策略），主 Run 循环每抓到一帧桌面 → 解码 + 水印 + sws 转 YUV420P → 把最新
// 摄像头帧缩放到主画面 1/4 尺寸、blit 到右下角（16px 边距）画中画区 → encoder
// → receive_packet → push AVFifo。
//
// 本类在方案 C 里完全是 producer：只负责合成 + encoder + queue push。
// 摄像头线程 / 输入设备 / sws_main_ / sws_cam_ 全程保持不变（不重建、不重建）。
// 分段切换由 muxer_consumer 负责，此处 encoder 全程一个。
class CompositeVideoSource : public IVideoSource {
public:
    explicit CompositeVideoSource(VideoOptions options);
    ~CompositeVideoSource() override;

    CompositeVideoSource(const CompositeVideoSource&) = delete;
    CompositeVideoSource& operator=(const CompositeVideoSource&) = delete;

    // producer 注入式接口（ScreenRecorder 调用）。
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
    bool OpenMainInput();
    bool OpenCameraInput();
    bool OpenEncoder();     // 完整：enc_ctx_ + open2，producer 全程持有
    bool InitScalers();     // 主 sws（桌面→YUV）+ 副 sws（摄像头→小尺寸 YUV）

    bool EncodeFrameToQueue(AVFrame* frame);
    bool FlushEncoderToQueue();

    void CameraThread();
    void BlitPiP();

    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    VideoOptions opts_;

    AVFifo* queue_ = nullptr;
    AVStream* stream_ = nullptr;
    std::atomic<bool>* flush_done_ = nullptr;

    // ---- 主源（gdigrab 桌面）----
    AVFormatContext* main_in_  = nullptr;
    AVCodecContext*  main_dec_ = nullptr;
    int              main_idx_ = -1;

    // ---- 副源（dshow 摄像头）----
    AVFormatContext* cam_in_  = nullptr;
    AVCodecContext*  cam_dec_ = nullptr;
    int              cam_idx_ = -1;
    SwsContext*      sws_cam_  = nullptr;
    AVFrame*         cam_small_ = nullptr;
    std::mutex       cam_mutex_;
    bool             cam_has_frame_ = false;
    std::thread      cam_thread_;
    std::atomic<bool> cam_stop_{false};

    // ---- 编码器 / 流 / 主缩放器 / 主 YUV 缓冲 ----
    AVCodecContext* enc_ctx_   = nullptr;
    SwsContext*     sws_main_  = nullptr;
    AVFrame*        yuv_frame_ = nullptr;

    // ---- 画中画几何 ----
    int pip_x_ = 0, pip_y_ = 0, pip_w_ = 0, pip_h_ = 0;

    // ---- 暂停状态 ----
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};

    int64_t prev_pts_ = -1;
};

}  // namespace re

#endif  // RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H
