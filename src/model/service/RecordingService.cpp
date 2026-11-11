// RecordingService.cpp
// 封装 ScreenRecorder 完整生命周期 + 从 UI 状态到 RecorderOptions 的组装校验。
#include "RecordingService.h"

#include <QString>

namespace re {

bool RecordingService::Start(RecorderOptions opts) {
    // Stop() 会在 Start() 失败返回前被调用兜底,但这里显式保证从 nullptr 开始。
    recorder_.reset();
    recorder_ = std::make_unique<ScreenRecorder>(std::move(opts));
    if (!recorder_ || !recorder_->Start(0)) {
        recorder_.reset();
        return false;
    }
    return true;
}

bool RecordingService::Stop() {
    if (!recorder_) return true;
    const bool ok = recorder_->Stop();
    recorder_.reset();
    return ok;
}

bool RecordingService::Pause() {
    if (!recorder_) return false;
    return recorder_->Pause();
}

bool RecordingService::Resume() {
    if (!recorder_) return false;
    return recorder_->Resume();
}

bool RecordingService::IsActive() const {
    return recorder_ != nullptr;
}

// ---------------------------------------------------------------------------
// StartFromUiState: 校验 + 组装 RecorderOptions + 调用 Start()
// ---------------------------------------------------------------------------

namespace {

constexpr int kFullscreen = 0;
constexpr int kRegion     = 1;
constexpr int kWindow     = 2;

}  // namespace

RecordingService::StartResult RecordingService::StartFromUiState(const UiState& s) {
    StartResult r;

    // ---- 前置校验 ----
    if (s.video_device == QStringLiteral("composite")
            && s.secondary_video_device.isEmpty()) {
        r.status = QStringLiteral("No camera selected");
        r.error  = QStringLiteral("Please select a camera for PiP");
        return r;
    }
    if (s.audio_device == QStringLiteral("mixed")) {
        if (s.primary_audio_device.isEmpty()) {
            r.status = QStringLiteral("No microphone selected");
            r.error  = QStringLiteral("Please select a microphone for mixed audio");
            return r;
        }
        if (s.secondary_audio_device.isEmpty()) {
            r.status = QStringLiteral("No system sound device selected");
            r.error  = QStringLiteral("Please select a system sound device");
            return r;
        }
    }
    if (s.record_mode == kWindow && s.window_title.isEmpty()) {
        r.status = QStringLiteral("No window selected");
        r.error  = QStringLiteral("Please select a window");
        return r;
    }
    if (s.record_mode == kRegion
            && (s.region_width <= 0 || s.region_height <= 0)) {
        r.status = QStringLiteral("No region selected");
        r.error  = QStringLiteral("Please select a region first");
        return r;
    }

    // ---- 组装 RecorderOptions ----
    RecorderOptions opts;
    opts.fps = 30;
    opts.bit_rate = 4'000'000;
    opts.capture_audio = true;
    opts.audio_sample_rate = 48000;
    opts.audio_channels = 2;
    opts.audio_bit_rate = 128'000;

    opts.output_path = s.output_path.toStdString();
    opts.speed_multiplier = s.speed_multiplier;
    opts.segment_duration_seconds = (s.segment_minutes > 0)
                                        ? s.segment_minutes * 60.0
                                        : 0.0;

    // 视频后端选择
    opts.video_device = s.video_device.toStdString();
    opts.video_secondary_device = s.secondary_video_device.toStdString();
    if (s.video_device.isEmpty() || s.video_device == QStringLiteral("desktop")) {
        opts.video_backend = "gdigrab";
    } else if (s.video_device == QStringLiteral("composite")) {
        opts.video_backend = "composite";
        opts.video_device = "desktop";
    } else {
        opts.video_backend = "dshow";
    }

    // 音频后端选择
    opts.audio_device = s.audio_device.toStdString();
    opts.audio_secondary_device = s.secondary_audio_device.toStdString();
    if (s.audio_device == QStringLiteral("mixed")) {
        opts.audio_backend = "mixed";
        opts.audio_device = s.primary_audio_device.toStdString();
    } else {
        opts.audio_backend = "dshow";
    }

    // 录制范围
    switch (s.record_mode) {
    case kWindow:
        opts.window_title = s.window_title.toStdString();
        opts.offset_x = opts.offset_y = 0;
        opts.width = opts.height = 0;
        break;
    case kRegion:
        opts.offset_x = s.region_offset_x;
        opts.offset_y = s.region_offset_y;
        opts.width = s.region_width;
        opts.height = s.region_height;
        break;
    default:
        opts.window_title.clear();
        opts.offset_x = opts.offset_y = 0;
        opts.width = opts.height = 0;
        break;
    }

    // ---- 启动 ----
    if (!Start(std::move(opts))) {
        r.status = QStringLiteral("Start failed");
        r.error  = QStringLiteral("Failed to start recording");
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace re
