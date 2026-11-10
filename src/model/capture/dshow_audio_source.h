#ifndef RE_CAPTURE_DSHOW_AUDIO_SOURCE_H
#define RE_CAPTURE_DSHOW_AUDIO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "i_audio_source.h"

// 前置声明 FFmpeg 类型，使调用方无需包含 FFmpeg 头文件。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct AVAudioFifo;

namespace re {

class Mp4Writer;

// DShowAudioSource 是 IAudioSource 的 DirectShow 具体实现：通过 FFmpeg 的 dshow
// 设备采集麦克风，用 SwrContext 重采样成 FLTP，借助 AVAudioFifo 把样本缓冲成固定
// 1024 采样的 AAC 帧，编码后经共享的 Mp4Writer 写包。
//
// 本类刻意对视频一无所知，只与视频侧共享唯一一样东西：Mp4Writer（输出文件）。
// 音频 PTS 与视频源同口径：共享零点 capture_start_，每帧按"此刻实际录制经过时间"
//（墙钟 - 暂停总时长）实时换算，突发连排撞槽时强制 +1 保严格单调。两条媒体时间轴
// 共用同一系统时钟，长录制不累积声卡晶振偏差导致的音画错位。
class DShowAudioSource : public IAudioSource {
public:
    DShowAudioSource(AudioOptions options, Mp4Writer* writer);
    ~DShowAudioSource() override;

    DShowAudioSource(const DShowAudioSource&) = delete;
    DShowAudioSource& operator=(const DShowAudioSource&) = delete;

    bool Open() override;
    bool Run(std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) override;
    void Close() override;

    // 暂停期间 Run 循环持续 av_read_frame 读出 dshow 推流的包并直接丢弃：dshow 是
    // 实时推流模型，不读会让其内部缓冲（rtbufsize=16MB ≈ 87 秒）持续堆积并在长暂停
    // 时溢出丢包，读出即丢等效于"输入暂停"且无溢出风险。暂停时长计入 total_paused_，
    // 音频 PTS（墙钟换算）在继续后自然接续，暂停段不产生时间轴空洞。
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
    bool OpenEncoder();  // 创建编码器，并创建输出音频流。
    bool InitResampler();
    // 把音频 FIFO 排空送入 AAC 编码器。若 `flush` 为 true，则编码最后一个（补齐的）
    // 残帧并 flush 编码器。
    bool DrainFifo(bool flush);

    // FFmpeg I/O 中断回调：停止标志置位后让阻塞在 av_read_frame 里的 dshow
    // 采集立即返回，从而能及时关闭设备（否则实时缓冲区持续暴涨并丢帧）。
    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    AudioOptions opts_;
    Mp4Writer*  writer_ = nullptr;  // 不持有所有权。

    // ---- 输入（dshow 麦克风）----
    AVFormatContext* in_fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_    = nullptr;
    int              in_index_   = -1;

    // ---- 编码器 / 流 / 重采样器 ----
    AVCodecContext* enc_ctx_ = nullptr;
    AVStream*       stream_  = nullptr;
    SwrContext*     swr_ctx_ = nullptr;
    AVAudioFifo*    fifo_    = nullptr;

    // ---- 墙钟 PTS 状态 ----
    // 共享零点（与视频源同一时刻），在 Run() 开始时设置。每帧 PTS = 此刻距零点的
    // 墙钟经过时间（扣除暂停）换算成样本数，与视频源同一口径，两条媒体时间轴共用
    // 同一系统时钟，长录制不累积音画错位。
    std::chrono::steady_clock::time_point start_{};
    // 上一帧 PTS：突发连续排空多帧时墙钟换算可能撞槽，强制 +1 保证编码器输入
    // 严格单调（误差 ≤1 样本，随后被墙钟自然吸收）。
    int64_t prev_pts_ = -1;

    // ---- 暂停状态（墙钟换算需扣除暂停总时长）----
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_DSHOW_AUDIO_SOURCE_H
