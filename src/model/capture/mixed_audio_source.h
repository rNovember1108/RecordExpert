#ifndef RE_CAPTURE_MIXED_AUDIO_SOURCE_H
#define RE_CAPTURE_MIXED_AUDIO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
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

// MixedAudioSource 是 IAudioSource 的“麦克风 + 系统声音”混音具体实现：同时
// 打开两路 DirectShow 音频设备（主源=麦克风 opts_.device，副源=系统声音/
// 立体声混音 opts_.secondary_device），各自解码并用 SwrContext 重采样成统一的
// FLTP/48k/2ch，在两个独立 AVAudioFifo 中缓冲；按两路较短一侧的样本数对齐后
// 等权相加（0.5+0.5，clamp 到 [-1,1]）写入输出 FIFO，再交给同一个 AAC 编码器
// 编码、经共享 Mp4Writer 写包。
//
// 设计要点：
//   * 两路都是实时推流（dshow），主 Run 循环用非阻塞 av_read_frame（EAGAIN 即
//     跳过）逐包泵入各自的输入 FIFO；
//   * 两个输入 FIFO 在稳态下近似等速增长（同为 48k 实时），混音取 min(a,b) 不会
//     产生持续累积；为防止一路设备停推/慢推导致另一路无限堆积，对超过阈值
//     （~0.5s）的一侧做丢弃式 resync；
//   * 暂停期间两路都持续读包丢弃（与 DShowAudioSource 同理，防 rtbufsize 溢出）；
//   * PTS 与 DShowAudioSource 同策略：共享零点墙钟换算（扣除暂停），撞槽强制
//     +1 保单调；两条媒体时间轴共用同一系统时钟，长录制无晶振偏差累积。
//
// 本类与视频侧只共享 Mp4Writer（输出文件）与共同墙钟零点，对视频一无所知。
class MixedAudioSource : public IAudioSource {
public:
    MixedAudioSource(AudioOptions options, Mp4Writer* writer);
    ~MixedAudioSource() override;

    MixedAudioSource(const MixedAudioSource&) = delete;
    MixedAudioSource& operator=(const MixedAudioSource&) = delete;

    bool Open() override;
    bool Run(std::chrono::steady_clock::time_point start,
             const std::atomic<bool>& stop) override;
    void Close() override;

    // 暂停期间两路 dshow 推流都持续读包丢弃，防 rtbufsize 溢出；暂停时长计入
    // total_paused_，音频 PTS（墙钟换算）在继续后自然接续，不产生时间轴空洞。
    void Pause() override { paused_.store(true); }
    void Resume() override {
        if (pause_start_ != std::chrono::steady_clock::time_point{}) {
            total_paused_ += std::chrono::steady_clock::now() - pause_start_;
            pause_start_ = std::chrono::steady_clock::time_point{};
        }
        paused_.store(false);
    }

private:
    // 打开单路 dshow 音频输入（设备名 device_name），写入传入的 fmt/dec/idx/swr。
    // 成功返回 true。两路复用同一套打开逻辑。
    bool OpenOneInput(const std::string& device_name,
                      AVFormatContext** fmt, AVCodecContext** dec,
                      int* idx, SwrContext** swr);
    bool OpenEncoder();
    // 把输出 FIFO 排空送入 AAC 编码器。flush=true 时编码最后一个（补齐的）残帧
    // 并 flush 编码器。
    bool DrainFifo(bool flush);
    // 从一路输入非阻塞读一个包并解码+重采样写入对应输入 FIFO。返回值：
    //   1 = 泵入成功（或 EAGAIN 无数据，可继续）
    //   0 = 发生硬错误（设备被拔/AVERROR_EXIT 等），调用方应结束录制。
    int PumpOne(AVFormatContext* in, AVCodecContext* dec, int idx,
                SwrContext* swr, AVAudioFifo* fifo,
                AVPacket* pkt, AVFrame* dec_frame);
    // 把两个输入 FIFO 中较短一侧的样本等权混音写入输出 FIFO。返回混入的样本数。
    int MixOne();

    // FFmpeg I/O 中断回调：停止标志置位后让阻塞在 av_read_frame 里的 dshow
    // 采集立即返回。
    static int InterruptCallback(void* opaque);
    const std::atomic<bool>* stop_flag_ = nullptr;

    AudioOptions opts_;
    Mp4Writer*  writer_ = nullptr;  // 不持有所有权。

    // ---- 主源（麦克风）输入 ----
    AVFormatContext* in_a_ctx_ = nullptr;
    AVCodecContext*  dec_a_     = nullptr;
    int              idx_a_     = -1;
    SwrContext*      swr_a_     = nullptr;
    AVAudioFifo*     fifo_a_     = nullptr;

    // ---- 副源（系统声音/立体声混音）输入 ----
    AVFormatContext* in_b_ctx_ = nullptr;
    AVCodecContext*  dec_b_     = nullptr;
    int              idx_b_     = -1;
    SwrContext*      swr_b_     = nullptr;
    AVAudioFifo*     fifo_b_     = nullptr;

    // ---- 编码器 / 流 / 输出 FIFO ----
    AVCodecContext* enc_ctx_ = nullptr;
    AVStream*       stream_  = nullptr;
    AVAudioFifo*    fifo_out_ = nullptr;  // 混音后的样本，供 AAC 编码取 1024 帧。

    // ---- PTS（与 DShowAudioSource 同策略：墙钟换算 + 撞槽强制单调）----
    std::chrono::steady_clock::time_point start_{};
    int64_t prev_pts_ = -1;

    // ---- 暂停状态 ----
    std::atomic<bool> paused_{false};
    std::chrono::steady_clock::time_point pause_start_{};
    std::chrono::steady_clock::duration total_paused_{};
};

}  // namespace re

#endif  // RE_CAPTURE_MIXED_AUDIO_SOURCE_H
