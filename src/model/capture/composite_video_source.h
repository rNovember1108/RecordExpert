#ifndef RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H
#define RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include "i_video_source.h"

// 前置声明 FFmpeg 类型，使调用方无需包含 FFmpeg 头文件。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace re {

class Mp4Writer;

// CompositeVideoSource 是 IVideoSource 的“桌面 + 摄像头画中画”具体实现：
// 主源为 gdigrab 桌面（device 固定 "desktop"），副源为 dshow 摄像头
// （secondary_device）。摄像头以独立线程持续采集，仅保留最新一帧（drop-old
// 策略），主 Run 循环每抓到一帧桌面 → 解码 + 水印 + sws 转 YUV420P → 把最新
// 摄像头帧缩放到主画面 1/4 尺寸、blit 到右下角（16px 边距）画中画区 → 编码。
//
// 设计要点：
//   * gdigrab 是按需 BitBlt 拉模型，主循环 av_read_frame 在帧就绪时返回；
//   * dshow 摄像头是实时推流模型，独立线程读取避免阻塞主循环、并 drop-old
//     只留最新帧，使 PiP 帧率与桌面解耦（摄像头 15/30fps 都不影响桌面 30fps）；
//   * 暂停期间主循环 sleep（gdigrab 无缓冲堆积），摄像头线程持续读包丢弃防
//     rtbufsize 溢出（与 DShowVideoSource 同理）；
//   * PTS 与桌面帧同口径墙钟换算（共享零点 + 暂停扣减）；PiP 摄像头帧不打独立 PTS。
//   * 水印仅在桌面帧上叠加（与 GdiGrabVideoSource 回退路径一致），摄像头画面
//     不加水印。
class CompositeVideoSource : public IVideoSource {
public:
    CompositeVideoSource(VideoOptions options, Mp4Writer* writer);
    ~CompositeVideoSource() override;

    CompositeVideoSource(const CompositeVideoSource&) = delete;
    CompositeVideoSource& operator=(const CompositeVideoSource&) = delete;

    bool Open() override;
    bool Run(double duration_seconds,
             std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) override;
    void Close() override;

    // 暂停期间主循环 sleep；摄像头线程持续读包丢弃防溢出。继续后按
    // “真实经过时间 - 暂停总时长”换算 PTS，时间轴自然接续。
    void Pause() override { paused_.store(true); }
    void Resume() override {
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            total_paused_ += std::chrono::steady_clock::now() - pause_start_;
            pause_start_ = std::chrono::steady_clock::time_point{};
        }
        paused_.store(false);
    }

private:
    bool OpenMainInput();    // gdigrab 桌面输入。
    bool OpenCameraInput();  // dshow 摄像头输入。
    bool OpenEncoder();      // mpeg4 编码器 + 输出流。
    bool InitScalers();      // 主 sws（桌面→YUV）+ 副 sws（摄像头→小尺寸 YUV）。
    // 摄像头采集线程：持续 av_read_frame→解码→sws 缩放→更新最新帧槽（drop-old）。
    void CameraThread();
    // 把最新摄像头帧 blit 到主 YUV 帧右下角画中画区（YUV420P 平面拷贝）。
    void BlitPiP();

    // FFmpeg I/O 中断回调：停止标志置位后让阻塞采集立即返回。
    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    VideoOptions opts_;
    Mp4Writer*  writer_ = nullptr;  // 不持有所有权。

    // ---- 主源（gdigrab 桌面）----
    AVFormatContext* main_in_  = nullptr;
    AVCodecContext*  main_dec_ = nullptr;
    int              main_idx_ = -1;

    // ---- 副源（dshow 摄像头）----
    AVFormatContext* cam_in_  = nullptr;
    AVCodecContext*  cam_dec_ = nullptr;
    int              cam_idx_ = -1;
    SwsContext*      sws_cam_  = nullptr;  // 摄像头解码帧 → 小尺寸 YUV420P。
    AVFrame*         cam_small_ = nullptr;  // 最新摄像头小尺寸 YUV 帧。
    std::mutex       cam_mutex_;            // 保护 cam_small_ / cam_has_frame_。
    bool             cam_has_frame_ = false;
    std::thread      cam_thread_;
    std::atomic<bool> cam_stop_{false};

    // ---- 编码器 / 流 / 主缩放器 / 主 YUV 缓冲 ----
    AVCodecContext* enc_ctx_   = nullptr;
    AVStream*       stream_    = nullptr;
    SwsContext*     sws_main_  = nullptr;  // 桌面解码帧 → 主 YUV420P。
    AVFrame*        yuv_frame_ = nullptr;  // 合成输出帧（桌面 + PiP）。

    // ---- 画中画几何（主画面坐标系，像素）----
    int pip_x_ = 0, pip_y_ = 0, pip_w_ = 0, pip_h_ = 0;

    // ---- 暂停状态 ----
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_COMPOSITE_VIDEO_SOURCE_H
