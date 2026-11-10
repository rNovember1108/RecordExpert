#ifndef RE_CAPTURE_DSHOW_VIDEO_SOURCE_H
#define RE_CAPTURE_DSHOW_VIDEO_SOURCE_H

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

// DShowVideoSource 是 IVideoSource 的 DirectShow 具体实现：通过 FFmpeg 的 dshow
// 设备采集摄像头/视频采集卡，用 libswscale 转换成 YUV420P，编码为 mpeg4（与
// GdiGrabVideoSource 同一编码器选型理由：GUI 模式 COM STA 下 h264_mf 必败），
// 再经共享的 Mp4Writer 写包。
//
// 与 gdigrab（按需 BitBlt，不抓即无帧）不同，dshow 摄像头是实时推流模型——
// 无论调用方是否读取，设备都持续向内部缓冲推帧。因此暂停策略与音频侧一致：
// 暂停期间继续 av_read_frame 读出包并立即丢弃，防止 rtbufsize 溢出丢包。
//
// 本类刻意对音频一无所知，只与音频侧共享 Mp4Writer（输出文件）和共同墙钟零点。
// 摄像头画面不加水印（水印是录屏场景的需求）。
class DShowVideoSource : public IVideoSource {
public:
    DShowVideoSource(VideoOptions options, Mp4Writer* writer);
    ~DShowVideoSource() override;

    DShowVideoSource(const DShowVideoSource&) = delete;
    DShowVideoSource& operator=(const DShowVideoSource&) = delete;

    bool Open() override;
    bool Run(double duration_seconds,
             std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) override;
    void Close() override;

    // 暂停期间持续读包丢弃（实时推流模型，防缓冲溢出）；继续后按
    // "真实经过时间 - 暂停总时长"换算 PTS，时间轴自然接续。
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
    bool InitScaler();

    // FFmpeg I/O 中断回调：停止标志置位后让阻塞在 av_read_frame 里的 dshow
    // 采集立即返回（否则实时推流下缓冲持续暴涨并丢帧）。
    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    VideoOptions opts_;
    Mp4Writer*  writer_ = nullptr;  // 不持有所有权。

    // ---- 输入（dshow 摄像头）----
    AVFormatContext* in_fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_    = nullptr;
    int              in_index_   = -1;

    // ---- 编码器 / 流 / 缩放器 ----
    AVCodecContext* enc_ctx_   = nullptr;
    AVStream*       stream_    = nullptr;
    SwsContext*     sws_ctx_   = nullptr;
    AVFrame*        yuv_frame_ = nullptr;    // sws 输出缓冲，循环复用。

    // ---- 暂停状态 ----
    // 暂停标志置位后，Run 循环持续读包丢弃；继续时累加暂停时长修正时长判断。
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_DSHOW_VIDEO_SOURCE_H
