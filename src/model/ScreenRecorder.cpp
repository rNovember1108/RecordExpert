#include "ScreenRecorder.h"

#include <utility>

#include "capture/audio_source_factory.h"
#include "capture/video_source_factory.h"

namespace re {

ScreenRecorder::ScreenRecorder(RecorderOptions options)
    : opts_(std::move(options)) {}

ScreenRecorder::~ScreenRecorder() {
    // 拆除任何东西之前，先确保后台线程都已结束；Stop() 会 join 各 worker，
    // Close() 释放资源。
    Stop();
    Close();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool ScreenRecorder::Open() {
    // 先分配共享的输出上下文，好让各轨能针对它创建自己的流。
    if (!writer_.Alloc(opts_.output_path.c_str()))
        return false;

    // 从视频相关选项构建视频源；具体类型由工厂按运行时后端名决定。
    VideoOptions vopts;
    vopts.fps = opts_.fps;
    vopts.bit_rate = opts_.bit_rate;
    vopts.offset_x = opts_.offset_x;
    vopts.offset_y = opts_.offset_y;
    vopts.width = opts_.width;
    vopts.height = opts_.height;
    vopts.speed_multiplier = opts_.speed_multiplier;
    vopts.window_title = opts_.window_title;
    vopts.device = opts_.video_device;
    vopts.secondary_device = opts_.video_secondary_device;
    video_ = VideoSourceFactory::Create(opts_.video_backend, vopts, &writer_);
    if (!video_ || !video_->Open()) return false;

    if (opts_.capture_audio) {
        AudioOptions aopts;
        aopts.device = opts_.audio_device;
        aopts.secondary_device = opts_.audio_secondary_device;
        aopts.sample_rate = opts_.audio_sample_rate;
        aopts.channels = opts_.audio_channels;
        aopts.bit_rate = opts_.audio_bit_rate;
        aopts.speed_multiplier = opts_.speed_multiplier;
        // 具体音频源同样由工厂按运行时后端名决定。
        audio_ = AudioSourceFactory::Create(opts_.audio_backend, aopts, &writer_);
        if (!audio_ || !audio_->Open()) {
            // 后端未知或音频初始化失败：仅录视频继续进行，从而在麦克风
            // 缺失/占用或后端不可用时不至于中断整个录制。
            audio_.reset();
            opts_.capture_audio = false;
        }
    }

    // 只有在所有流（视频 + 可选的音频）都创建完毕后才写文件头。
    if (!writer_.WriteHeader()) return false;
    return true;
}

bool ScreenRecorder::Start(double duration_seconds) {
    if (recording_.load() || video_worker_.joinable() ||
        audio_worker_.joinable()) {
        return false;
    }

    // 在调用方线程上打开设备/编码器，从而让失败能同步报告，出错时也不会已经
    // 启动了后台线程。
    if (!Open()) {
        Close();  // 释放此前部分打开的资源。
        return false;
    }

    stop_requested_.store(false);
    video_ok_.store(true);
    audio_ok_.store(true);
    recording_.store(true);
    active_workers_.store(opts_.capture_audio ? 2 : 1);

    // 在 worker 开始读取*之前*，为两条时间轴确立共享零点，使视频与音频的
    // 墙钟 PTS 从同一时刻起算。
    capture_start_ = std::chrono::steady_clock::now();

    video_worker_ = std::thread([this, duration_seconds]() {
        video_ok_.store(
            video_->Run(duration_seconds, capture_start_, stop_requested_));
        // 当其中一个循环结束（例如视频时长走完）时，请求另一个也停下，让两个
        // 线程一起结束。
        stop_requested_.store(true);
        if (--active_workers_ == 0) recording_.store(false);
    });

    if (opts_.capture_audio) {
        audio_worker_ = std::thread([this]() {
            audio_ok_.store(audio_->Run(capture_start_, stop_requested_));
            stop_requested_.store(true);
            if (--active_workers_ == 0) recording_.store(false);
        });
    }
    return true;
}

bool ScreenRecorder::Stop() {
    stop_requested_.store(true);
    if (video_worker_.joinable()) video_worker_.join();
    if (audio_worker_.joinable()) audio_worker_.join();
    // 两个循环都已结束；把文件收尾（幂等）。
    Close();
    return video_ok_.load() && audio_ok_.load();
}

// ---------------------------------------------------------------------------
// 暂停 / 继续
// ---------------------------------------------------------------------------

bool ScreenRecorder::Pause() {
    // 仅在录制中且未暂停时切换为暂停；转发给两条轨，让 worker 在循环顶部
    // sleep + continue，不再采集也不写包。
    if (!recording_.load() || paused_.load()) return false;
    paused_.store(true);
    if (video_) video_->Pause();
    if (audio_) audio_->Pause();
    return true;
}

bool ScreenRecorder::Resume() {
    if (!paused_.load()) return false;
    if (video_) video_->Resume();
    if (audio_) audio_->Resume();
    paused_.store(false);
    return true;
}

// ---------------------------------------------------------------------------
// 拆除
// ---------------------------------------------------------------------------

void ScreenRecorder::Close() {
    // 顺序很重要：先 flush/关闭各轨（它们会通过 EncodeAndWrite 引用 muxer），
    // 再让 muxer 写 trailer 并释放持有各轨的流的输出上下文。
    if (video_) video_->Close();
    if (audio_) audio_->Close();
    writer_.Close();
}

}  // namespace re
