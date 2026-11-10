#ifndef RECORD_EXPERT_SCREEN_RECORDER_H
#define RECORD_EXPERT_SCREEN_RECORDER_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "capture/i_audio_source.h"
#include "capture/i_video_source.h"
#include "Mp4Writer.h"

namespace re {

// 控制一次录制会话的选项。由 GUI 控制器填写的公开旋钮集合；内部会拆分为
// VideoOptions / AudioOptions 供两条轨各自使用。
struct RecorderOptions {
    std::string output_path = "output.mp4";  // 目标 MP4 文件。

    // ---- 录制范围 ----
    // UI 层负责把用户选择归一化写入下列字段（window_title / offset / size），
    // ScreenRecorder 本身不感知录制范围模式。
    // 指定窗口录制：窗口标题（UTF-8 字符串）。非空时 gdigrab 走 title= 路径。
    std::string window_title;

    // 目标采集/输出帧率。fps 越高，对操作系统光标的采样越密，能减轻 gdigrab
    // 所绘鼠标的可见闪烁。
    int fps = 30;
    int64_t bit_rate = 4'000'000;            // 目标视频码率（比特/秒）。
    // 采集区域。offset_x/offset_y 是桌面上的左上角坐标。width/height <= 0
    // 表示"采集整个主屏幕"；区域录制时必须 >0。
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
    // 视频采集后端名（运行时参数，VideoSourceFactory 据此选择具体源）。
    // 当前已登记："gdigrab"（桌面）、"dshow"（DirectShow 摄像头/视频设备）、
    // "composite"（桌面+摄像头画中画）。
    std::string video_backend = "gdigrab";
    // 视频设备名。video_backend=="gdigrab" 时固定为 "desktop"（或指定窗口标题）；
    // video_backend=="dshow" 时为 avdevice_list_input_sources 返回的 device_name；
    // video_backend=="composite" 时主源为 "desktop"，副源见 video_secondary_device。
    std::string video_device = "desktop";
    // 混合录制（video_backend=="composite"）时的副源摄像头设备名。
    // 其他 video_backend 忽略。
    std::string video_secondary_device;

    // ---- 音频（通过 DirectShow 采集麦克风）----
    bool capture_audio = true;  // 把麦克风录进 MP4。
    // DirectShow 音频设备名（不带 "audio=" 前缀）。列出设备的命令：
    // ffmpeg -list_devices true -f dshow -i dummy
    // audio_backend=="mixed" 时为主源（麦克风）设备名。
    std::string audio_device;
    // 混合录制（audio_backend=="mixed"）时的副源音频设备名（系统声音/立体声混音）。
    // 其他 audio_backend 忽略。
    std::string audio_secondary_device;
    int audio_sample_rate = 48000;      // 输出采样率（Hz）。
    int audio_channels = 2;             // 输出声道数。
    int64_t audio_bit_rate = 128'000;   // AAC 码率（比特/秒）。
    // 音频采集后端名（运行时参数，AudioSourceFactory 据此选择具体源）。
    // 当前已登记："dshow"、"mixed"（麦克风+系统声音混音）。
    std::string audio_backend = "dshow";

    // 倍率录制（time-lapse）：1/2/4/8。N>1 时 N 秒墙钟时间 → 1 秒 mp4 输出
    // （快进 N 倍）。视频每 N 帧保留 1 帧，音频每 N 个样本保留 1 个（变调接受）。
    // 由 UI 倍率下拉框设置，校验值域 1/2/4/8，越界回退 1。
    int speed_multiplier = 1;
};

// ScreenRecorder 协调一个视频源（IVideoSource）和一个可选的音频源
// （IAudioSource），两者共享同一个 MP4 输出（Mp4Writer）。具体源由
// VideoSourceFactory / AudioSourceFactory 按 RecorderOptions 中的运行时后端名
// 创建；本类自身不持有任何采集/编码逻辑，只负责：
//   * 把 RecorderOptions 拆分为各轨的选项，
//   * 打开共享 muxer 与两条轨，然后写文件头，
//   * 从一个共同的墙钟零点出发，在各自的后台线程上运行每条轨（使两条媒体时间轴
//     保持对齐），
//   * 生命周期：Start / Stop 以及把文件收尾。
//
// 典型用法：
//
//     ScreenRecorder rec(opts);
//     if (!rec.Start(seconds)) { ... }   // 立即返回
//     ...                                // 调用方继续做其他事情
//     rec.Stop();                        // 提前停止并把文件收尾
//
// 所有公开方法都可从录制线程以外的线程安全调用。
class ScreenRecorder {
public:
    explicit ScreenRecorder(RecorderOptions options);
    ~ScreenRecorder();

    ScreenRecorder(const ScreenRecorder&) = delete;
    ScreenRecorder& operator=(const ScreenRecorder&) = delete;

    // 打开采集设备/编码器，并在后台线程上启动录制循环，随后立即返回。若
    // `duration_seconds <= 0`，录制会一直持续到调用 Stop()。若打开设备或编码器
    // 失败则返回 false（此时不会启动任何东西）。在已经在录制时调用 Start()
    // 会返回 false。
    bool Start(double duration_seconds);

    // 请求后台录制停止，然后等待其结束（flush 编码器并写好文件）。可安全地多次
    // 调用，即便从未启动过录制也可安全调用。返回录制的成功状态（若发生致命错误
    // 则为 false）。
    bool Stop();

    // 暂停/继续录制：转发到两个源的 Pause/Resume。仅在录制进行中有效，
    // 返回是否实际触发了状态切换。暂停期间视频源不抓帧（gdigrab 拉模型，
    // 不读即不抓）；音频源持续读出 dshow 推流的包并丢弃，避免其内部缓冲
    // 在长暂停时溢出。继续后两路 PTS 均按"墙钟 - 暂停总时长"换算，自然接续。
    bool Pause();
    bool Resume();

private:
    // 打开共享 muxer 与两条轨，然后写文件头。失败返回 false。
    bool Open();

    // Join 各 worker 线程（若在运行），把文件收尾并释放所有资源。幂等。
    void Close();

    RecorderOptions opts_;
    std::atomic<bool> stop_requested_{false};

    // 共享的输出文件，以及向它写入的两个采集源（实际类型由工厂按后端名创建）。
    Mp4Writer                        writer_;
    std::unique_ptr<IVideoSource>     video_;
    std::unique_ptr<IAudioSource>     audio_;

    // 后台录制线程与生命周期状态。
    std::thread       video_worker_;
    std::thread       audio_worker_;
    std::atomic<bool> recording_{false};   // 只要有 worker 循环在运行即为 true。
    std::atomic<int>  active_workers_{0};  // 正在运行的 worker 循环数量。
    std::atomic<bool> video_ok_{true};
    std::atomic<bool> audio_ok_{true};
    std::atomic<bool> paused_{false};  // 暂停标志，转发给两条轨。

    // 两条轨共享的共同墙钟零点。在 worker 线程启动前恰好捕获，使视频与音频的
    // 墙钟 PTS 从同一时刻起算。
    std::chrono::steady_clock::time_point capture_start_{};
};

}  // namespace re

#endif  // RECORD_EXPERT_SCREEN_RECORDER_H
