#ifndef RE_CAPTURE_I_VIDEO_SOURCE_H
#define RE_CAPTURE_I_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// FFmpeg 类型前置声明 —— 放在全局 extern "C" 里，避免 namespace re 里同名类型
// 遮盖 FFmpeg 头文件里的真实定义（FFmpeg 库用全局 C namespace）。
extern "C" {
struct AVCodecContext;
struct AVFifo;
struct AVPacket;
struct AVStream;
}

namespace re {

// 控制视频（桌面）采集的选项。各具体视频源按自身能力取用其中字段。
struct VideoOptions {
    int fps = 30;                 // 目标采集 / 输出帧率。
    int64_t bit_rate = 4'000'000; // 目标视频码率（比特/秒）。
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
    int speed_multiplier = 1;
    std::string window_title;
    std::string device;
    std::string secondary_device;
};

// IVideoSource 在方案 C 里只做"采集 → 解码 → sws → 水印 → send_frame/receive_packet
// → push 进 AVFifo"。它全程不触碰 muxer，不知道分段的存在。分段切换、PTS 偏移手动
// 减、WritePacket 全部在 muxer_consumer 侧完成。
//
// 接口生命周期：
//   Open 之前 SetQueue → Open 之后 SetStream / SetFlushDoneFlag
//   → Run（循环）→ Run 末尾 flush encoder → Close
//   Rotate 时 SetStream 刷新 stream 指针
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    // Open 之前注入输出队列（producer receive_packet 后 push 这里）。
    virtual void SetQueue(AVFifo* queue) = 0;

    // Open 之后注入流指针（Rotate 后也刷新）。
    virtual void SetStream(AVStream* stream) = 0;

    // Open 之后注入 flush_done 标志（muxer_worker 设 flush_requested，producer flush
    // 完后 store(true)，通知 muxer 可以 drain queue + Rotate）。
    virtual void SetFlushDoneFlag(std::atomic<bool>* flag) = 0;

    virtual bool Open() = 0;

    // Run 循环：采集 → 解码 → sws → 水印/BlitPiP → send_frame + receive_packet
    // → push queue。duration_seconds=0 表示无限循环（segment 边界由 muxer flush_requested 驱动）。
    virtual bool Run(double duration_seconds,
                     std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop,
                     const std::atomic<bool>& flush_requested) = 0;

    virtual void Close() = 0;

    virtual void Pause() = 0;
    virtual void Resume() = 0;

    // ScreenRecorder 在 OpenMuxer/RotateMuxer 时需要 encoder 上下文（MP4Writer
    // 内部创建 stream 时用 enc_ctx_->codecpar 拷贝）。
    virtual AVCodecContext* EncCtx() const = 0;
};

}  // namespace re

#endif  // RE_CAPTURE_I_VIDEO_SOURCE_H
