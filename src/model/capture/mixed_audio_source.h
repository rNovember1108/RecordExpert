#ifndef RE_CAPTURE_MIXED_AUDIO_SOURCE_H
#define RE_CAPTURE_MIXED_AUDIO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "i_audio_source.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwrContext;
struct AVAudioFifo;

namespace re {

// MixedAudioSource：双路 dshow 声卡 → 各自解码 → swr → 三路 AVAudioFifo（fifo_a/fifo_b/fifo_out）
// → MixOne 等权 0.5+0.5 混音 → DrainFifo → AAC encoder → receive_packet → push AVFifo。
// 全程不碰 muxer，分段切换由 muxer_consumer 负责，此处 encoder 全程一个。
class MixedAudioSource : public IAudioSource {
public:
    explicit MixedAudioSource(AudioOptions options);
    ~MixedAudioSource() override;

    MixedAudioSource(const MixedAudioSource&) = delete;
    MixedAudioSource& operator=(const MixedAudioSource&) = delete;

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
    bool OpenOneInput(const std::string& device_name,
                      AVFormatContext** fmt, AVCodecContext** dec,
                      int* idx, SwrContext** swr);
    bool OpenEncoder();
    bool DrainFifo(bool flush);
    int PumpOne(AVFormatContext* in, AVCodecContext* dec, int idx,
                SwrContext* swr, AVAudioFifo* fifo,
                AVPacket* pkt, AVFrame* dec_frame);
    int MixOne();

    bool EncodeFrameToQueue(AVFrame* frame);
    bool FlushEncoderToQueue();

    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    AudioOptions opts_;
    AVFifo* queue_ = nullptr;
    AVStream* stream_ = nullptr;
    std::atomic<bool>* flush_done_ = nullptr;

    AVFormatContext* in_a_ctx_ = nullptr;
    AVCodecContext*  dec_a_     = nullptr;
    int              idx_a_     = -1;
    SwrContext*      swr_a_     = nullptr;
    AVAudioFifo*     fifo_a_     = nullptr;

    AVFormatContext* in_b_ctx_ = nullptr;
    AVCodecContext*  dec_b_     = nullptr;
    int              idx_b_     = -1;
    SwrContext*      swr_b_     = nullptr;
    AVAudioFifo*     fifo_b_     = nullptr;

    AVCodecContext* enc_ctx_ = nullptr;
    AVAudioFifo*    fifo_out_ = nullptr;

    std::chrono::steady_clock::time_point start_{};
    int64_t prev_pts_ = -1;

    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_MIXED_AUDIO_SOURCE_H
