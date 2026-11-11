#ifndef RECORD_EXPERT_SCREEN_RECORDER_H
#define RECORD_EXPERT_SCREEN_RECORDER_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <libavutil/fifo.h>   // AVFifo（单生产者-单消费者 lock-free）

#include "capture/i_audio_source.h"
#include "capture/i_video_source.h"
#include "Mp4Writer.h"

struct AVPacket;  // 前置声明
struct AVStream;

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

    // ---- 分段录制 ----
    // > 0 时，录制会话自动按 segment_duration_seconds 秒切分成多个 MP4 文件
    // （"output_001.mp4"、"output_002.mp4" …），中间不停止采集设备、不丢帧。
    // <= 0 时不分段，整段录制写一个 output_path 文件。
    double segment_duration_seconds = 0.0;
};

// ScreenRecorder 采用"双生产者 + 单消费者"三线程架构：
//
//   video_producer_worker  ──┐  send_frame + receive_packet
//                             │  → push AVFifo queue_v
//   audio_producer_worker  ──┤
//                             │  send_frame + receive_packet
//                             │  → push AVFifo queue_a
//                             ▼
//                     ┌────────────────────┐
//                     │  muxer_worker      │  唯一持有 AVFormatContext*
//                     │  queue_v / queue_a │  唯一调 av_interleaved_write_frame
//                     │  pts_offset 手动减  │  唯一做 segment 边界检测 + Rotate
//                     └────────────────────┘
//                             ▼
//                        MP4 文件
//
// 关键设计：
//   * encoder 全程一个（segment 边界不重建），由 pts_offset 手动减实现新 segment
//     从 PTS=0 起算；硬件编码器（NVENC/QSV/AMF）零 rebuild 开销
//   * AVFifo 单生产者-单消费者 lock-free，FFmpeg 内部用 C11 原子 head/tail 交换
//   * AVPacket 跨 FIFO 安全：AVBufferRef 原子引用计数，producer/unref 时只减自己那份
//   * segment 边界由 muxer_worker wall clock 检测，通知 producer flush encoder →
//     drain queue → Rotate → 更新 pts_offset → 继续下一段（encoder 不动）
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
    // 打开各轨输入设备 + 编码器，OpenMuxer 写首个文件头。失败返回 false。
    bool Open();

    // Join 各 worker 线程（若在运行），把文件收尾并释放所有资源。幂等。
    void Close();

    // 把原始 path 按 segment_index 生成 _NNN 后缀的分段路径。
    static std::string BuildSegmentPath(const std::string& original, int segment_index);

    RecorderOptions opts_;
    std::atomic<bool> stop_requested_{false};

    // ---- 各轨采集与编码（producer 侧） ----
    // 两条轨的 Run 循环：采集 → 解码 → sws/swr → 水印/混音 → send_frame + receive_packet
    // → push 进 AVFifo。encoder 全程一个，segment 边界不重建。
    std::unique_ptr<IVideoSource> video_;
    std::unique_ptr<IAudioSource> audio_;

    // ---- 唯一的 muxer（consumer 侧） ----
    // Mp4Writer 被 muxer_worker_ 单线程调用；producer 不触碰它。
    Mp4Writer writer_;

    // ---- lock-free 队列（AVFifo） ----
    // 单生产者(video_producer) + 单消费者(muxer_consumer)
    // 单生产者(audio_producer) + 单消费者(muxer_consumer)
    AVFifo* queue_v_ = nullptr;
    AVFifo* queue_a_ = nullptr;  // capture_audio=false 时保持 nullptr

    // ---- flush 同步 ----
    // muxer_worker 检测到 segment 边界时，设 flush_requested，producer 看到后
    // 把 encoder 内部残留帧 flush 出来 push queue，然后设 flush_done 通知 muxer
    // 可以 drain + Rotate。新段 pts_offset 由 muxer 依据"已写入的最大原始 pts"
    // 权威计算（DrainFifoAndWrite 返回值），不依赖 encoder flush 是否产出残包。
    std::atomic<bool> flush_requested_v_{false};
    std::atomic<bool> flush_done_v_{false};
    std::atomic<bool> flush_requested_a_{false};
    std::atomic<bool> flush_done_a_{false};

    // producer 退出标志：Run 返回后由 worker lambda 置 true（此时尾包已全部
    // push 进 queue）。muxer_worker 用它判断"可以安全做最终 drain + trailer"，
    // 避免Stop 时 muxer 抢先收尾把 producer 还没 flush 出来的尾包丢掉。
    std::atomic<bool> producers_done_v_{false};
    std::atomic<bool> producers_done_a_{false};

    // ---- segment 边界状态 ----
    int   segment_index_ = 0;   // 递增序号，仅用于分段文件命名（_001/_002…）
    int64_t pts_offset_v_ = 0;   // 每次 Rotate 后更新为"本轨已写入最大原始 pts + 1"
    int64_t pts_offset_a_ = 0;   // 同上，音频轨
    // 所有 producer 的墙钟 PTS 零点：Start 时置为 now，全录制期间不变。
    // 注意：分段边界不再重置它——新段从 0 起算由 pts_offset_ 手动减法实现；
    // 分段边界计时由 muxer_worker 内部的 seg_active（暂停冻结）负责。
    std::chrono::steady_clock::time_point capture_start_{};

    // muxer_worker 持有的当前 segment 的 stream 指针（Rotate 后刷新）。
    // producer 的 stream_ 也会通过 SetStream 同步更新（rescale 需要）。
    AVStream* current_stream_v_ = nullptr;
    AVStream* current_stream_a_ = nullptr;

    // ---- worker 线程 ----
    std::thread video_worker_;   // producer: 采集+编码+push queue_v_
    std::thread audio_worker_;   // producer: 采集+编码+push queue_a_（可选）
    std::thread muxer_worker_;   // consumer: pop queue → pts 减 offset → write_frame + Rotate

    // 生命周期状态
    std::atomic<bool> recording_{false};
    std::atomic<bool> video_ok_{true};
    std::atomic<bool> audio_ok_{true};
    std::atomic<bool> paused_{false};
};

}  // namespace re

#endif  // RECORD_EXPERT_SCREEN_RECORDER_H
