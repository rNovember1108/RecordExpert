#ifndef RE_CAPTURE_I_VIDEO_SOURCE_H
#define RE_CAPTURE_I_VIDEO_SOURCE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace re {

class Mp4Writer;

// 控制视频（桌面）采集的选项。各具体视频源按自身能力取用其中字段。
struct VideoOptions {
    int fps = 30;                 // 目标采集 / 输出帧率。
    int64_t bit_rate = 4'000'000; // 目标视频码率（比特/秒）。
    // 采集区域。offset_x/offset_y 是桌面上的左上角坐标；width/height <= 0 表示
    // "采集整个主屏"。当 window_title 非空时，这些区域坐标在 gdigrab 内部会被
    // 忽略，但保留字段以兼容其他后端。
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
    // 倍率录制：1/2/4/8。N>1 时每 N 帧保留 1 帧，输出 PTS 按"墙钟经过时间 / N"
    // 换算，从而把 N 秒墙钟内容压缩到 1 秒 mp4 输出。
    int speed_multiplier = 1;
    // 指定窗口录制：窗口标题（UTF-8 字符串）。空串或 "<desktop>" 表示全屏录制。
    // gdigrab 内部会把 title=<name> 传给 avformat_open_input 替代 "desktop"。
    // 注意：gdigrab 的 title= 模式按窗口初始矩形固定采集，窗口移动不会跟随——
    // 这是已知限制，可接受。
    std::string window_title;
    // dshow 视频设备名（摄像头/采集卡）。仅 dshow 后端使用；gdigrab 忽略。
    // 取值来自 avdevice_list_input_sources 返回的 device_name。
    std::string device;
    // 混合录制（composite 后端）时的副源摄像头设备名。composite 后端：device
    // 固定为 "desktop"（gdigrab 主源），secondary_device 为 dshow 摄像头名。
    // 其他后端忽略本字段。
    std::string secondary_device;
};

// IVideoSource 是视频采集源的抽象接口（策略模式中的抽象策略）。具体实现
// （如 GdiGrabVideoSource）各自封装一种桌面采集方案（gdigrab、dshow 等），
// 但对协调器 ScreenRecorder 暴露完全一致的生命周期：
//
//   Open()                —— 打开采集设备/编码器/输出流（初始化）
//   Run()                 —— 在 worker 线程上开始采集编码循环
//   Pause()/Resume()      —— 暂停/继续
//   Close()               —— 结束并释放全部资源
//
// 具体源只负责"采到帧并编码"，编码后的包统一通过共享的 Mp4Writer 写入同一个
// MP4；水印等公共能力由具体源自行复用公共模块（Watermark.h）。这样新增一种采集
// 后端只需新增一个本接口的实现类并在 VideoSourceFactory 登记，协调器无需改动。
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    // 打开采集输入、编码器以及输出视频流。必须在 Run() 之前调用。
    // 失败返回 false。
    virtual bool Open() = 0;

    // 采集并编码帧，直到 `duration_seconds` 耗尽（若 > 0）或 `stop` 变为 true。
    // 以 `start` 作为打真实时间 PTS 的共享零点（与音频源同一时刻）。发生致命
    // 错误返回 false。运行在调用方线程上（协调器在专用 worker 线程调用）。
    virtual bool Run(double duration_seconds,
                     std::chrono::steady_clock::time_point start,
                     const std::atomic<bool>& stop) = 0;

    // flush 编码器并释放所有视频资源。幂等。
    virtual void Close() = 0;

    // 暂停/继续采集。暂停期间不抓帧、不写包；继续后按"真实经过时间 - 暂停总时长"
    // 修正 PTS，使视频时间轴贴合墙钟。
    virtual void Pause() = 0;
    virtual void Resume() = 0;
};

}  // namespace re

#endif  // RE_CAPTURE_I_VIDEO_SOURCE_H
