#ifndef RE_CAPTURE_I_AUDIO_SOURCE_H
#define RE_CAPTURE_I_AUDIO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace re {

class Mp4Writer;

// 控制音频采集的选项。各具体音频源按自身能力取用其中字段。
struct AudioOptions {
    // 采集设备名（含义由具体源决定；dshow 源为不带 "audio=" 前缀的设备名）。
    // 列出 dshow 设备的命令：ffmpeg -list_devices true -f dshow -i dummy
    std::string device = "麦克风阵列 (Realtek(R) Audio)";
    int sample_rate = 48000;      // 输出采样率（Hz）。
    int channels = 2;             // 输出声道数。
    int64_t bit_rate = 128'000;   // AAC 码率（比特/秒）。
    // 倍率录制：1/2/4/8。N>1 时每 N 个样本保留 1 个，输出时长缩短为 1/N，
    // 频率提升 N 倍（变调，time-lapse 标准做法）。
    int speed_multiplier = 1;
    // 混合录制（mixed 后端）时的副源音频设备名（系统声音/立体声混音）。
    // mixed 后端：device 为主源（麦克风），secondary_device 为副源（系统声音）。
    // 其他后端忽略本字段。
    std::string secondary_device;
};

// IAudioSource 是音频采集源的抽象接口（策略模式中的抽象策略）。具体实现
// （如 DShowAudioSource）各自封装一种音频采集方案（dshow、wasapi 等），但对
// 协调器 ScreenRecorder 暴露与 IVideoSource 同构的生命周期：
//
//   Open()                —— 打开采集设备/编码器/重采样器/输出流（初始化）
//   Run()                 —— 在 worker 线程上开始采集编码循环
//   Pause()/Resume()      —— 暂停/继续
//   Close()               —— 结束并释放全部资源
//
// 具体源只负责"采到样本并编码"，编码后的包统一通过共享的 Mp4Writer 写入同一个
// MP4。新增一种音频后端只需新增一个本接口的实现类并在 AudioSourceFactory 登记。
class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // 打开采集输入、编码器、重采样器以及输出音频流。必须在 Run() 之前调用。
    // 失败返回 false。
    virtual bool Open() = 0;

    // 采集并编码音频，直到 `stop` 变为 true。`start` 是共享的墙钟零点（与视频源
    // 使用同一时刻），据此对齐两条媒体时间轴。发生致命错误返回 false。运行在
    // 调用方线程上（协调器在专用 worker 线程调用）。
    virtual bool Run(std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop) = 0;

    // flush 编码器并释放所有音频资源。幂等。
    virtual void Close() = 0;

    // 暂停/继续采集。各具体实现自行处理实时设备缓冲（如 dshow 暂停期间持续读包
    // 丢弃以防溢出）；继续后音频时间轴自然接续。
    virtual void Pause() = 0;
    virtual void Resume() = 0;
};

}  // namespace re

#endif  // RE_CAPTURE_I_AUDIO_SOURCE_H
