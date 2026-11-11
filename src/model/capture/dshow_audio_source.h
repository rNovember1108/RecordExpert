#ifndef RE_CAPTURE_DSHOW_AUDIO_SOURCE_H
#define RE_CAPTURE_DSHOW_AUDIO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "i_audio_source.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwrContext;
struct AVAudioFifo;

namespace re {

// DShowAudioSource：dshow 麦克风采集 → swr → FIFO 缓冲 → AAC encoder → receive_packet → push AVFifo。
// 全程不碰 muxer，分段切换由 muxer_consumer 负责，此处 encoder 全程一个。
class DShowAudioSource : public IAudioSource {
public:
    explicit DShowAudioSource(AudioOptions options);
    ~DShowAudioSource() override;

    DShowAudioSource(const DShowAudioSource&) = delete;
    DShowAudioSource& operator=(const DShowAudioSource&) = delete;

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
    bool InitResampler();

    // DrainFifo 原来通过 writer_->EncodeAndWrite 送 muxer；现在改成 producer 封装：
    //   把 FIFO 排空 → avcodec_send_frame + receive_packet → rescale → push queue
    // flush=true 时编码最后补齐的残帧并 FlushEncoderToQueue。
    bool DrainFifo(bool flush);

    // DrainFifo 内部：send_frame + receive_packet → rescale → push queue
    bool EncodeFrameToQueue(AVFrame* frame);
    // Run 末尾 / segment 边界 flush encoder：send_frame(nullptr) → receive_packet
    // 循环 → push queue（内部 flush_buffers 退出 drain 模式）。
    bool FlushEncoderToQueue();

    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    AudioOptions opts_;

    AVFifo* queue_ = nullptr;
    AVStream* stream_ = nullptr;
    std::atomic<bool>* flush_done_ = nullptr;

    AVFormatContext* in_fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_    = nullptr;
    int              in_index_   = -1;

    AVCodecContext* enc_ctx_ = nullptr;
    SwrContext*     swr_ctx_ = nullptr;
    AVAudioFifo*    fifo_    = nullptr;

    std::chrono::steady_clock::time_point start_{};
    int64_t prev_pts_ = -1;

    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_DSHOW_AUDIO_SOURCE_H
