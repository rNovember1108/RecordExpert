#ifndef RE_CAPTURE_DSHOW_VIDEO_SOURCE_H
#define RE_CAPTURE_DSHOW_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <string>

#include "i_video_source.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;

namespace re {

// dshow 单一视频设备（通常是摄像头 / 视频采集卡）的 producer。
// 与 GdiGrabVideoSource 同构：dshow 输入 → 解码 → sws → 水印 → encoder → queue。
//
// dshow 是推流设备，Run 里 av_read_frame 非阻塞 + 暂停期间持续读包丢弃（rtbufsize）。
class DShowVideoSource : public IVideoSource {
public:
    explicit DShowVideoSource(VideoOptions options);
    ~DShowVideoSource() override;

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
    bool OpenInput();
    bool OpenEncoder();
    bool InitScaler();

    bool EncodeFrameToQueue(AVFrame* frame);
    bool FlushEncoderToQueue();

    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    VideoOptions opts_;
    AVFifo* queue_ = nullptr;
    AVStream* stream_ = nullptr;
    std::atomic<bool>* flush_done_ = nullptr;

    AVFormatContext* in_ = nullptr;
    int              idx_ = -1;
    AVCodecContext*  dec_ = nullptr;
    AVCodecContext*  enc_ctx_ = nullptr;
    SwsContext*      sws_ctx_ = nullptr;
    AVFrame*         yuv_frame_ = nullptr;

    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};

    int64_t prev_pts_ = -1;
};

}  // namespace re

#endif  // RE_CAPTURE_DSHOW_VIDEO_SOURCE_H
