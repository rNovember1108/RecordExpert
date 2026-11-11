#ifndef RE_CAPTURE_I_AUDIO_SOURCE_H
#define RE_CAPTURE_I_AUDIO_SOURCE_H

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

// 控制音频采集的选项。
struct AudioOptions {
    std::string device = "麦克风阵列 (Realtek(R) Audio)";
    int sample_rate = 48000;
    int channels = 2;
    int64_t bit_rate = 128'000;
    int speed_multiplier = 1;
    std::string secondary_device;
};

// IAudioSource 在方案 C 里与 IVideoSource 同构：只做采集→解码→swr→混音→send_frame
// + receive_packet → push AVFifo。全程不碰 muxer，分段切换/PTS 偏移/WritePacket 全在
// muxer_consumer 侧完成。
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual void SetQueue(AVFifo* queue) = 0;
    virtual void SetStream(AVStream* stream) = 0;
    virtual void SetFlushDoneFlag(std::atomic<bool>* flag) = 0;

    virtual bool Open() = 0;
    virtual bool Run(double duration_seconds,
                     std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop,
                     const std::atomic<bool>& flush_requested) = 0;
    virtual void Close() = 0;

    virtual void Pause() = 0;
    virtual void Resume() = 0;

    virtual AVCodecContext* EncCtx() const = 0;
};

}  // namespace re

#endif  // RE_CAPTURE_I_AUDIO_SOURCE_H
