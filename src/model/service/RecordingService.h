// RecordingService.h
// RecordingService: 封装 ScreenRecorder 完整生命周期的纯 C++ 服务。
// 不继承 QObject,可独立于 Qt 编译单元单测。
#ifndef RECORDING_SERVICE_H
#define RECORDING_SERVICE_H

#include <memory>
#include <QString>

#include "ScreenRecorder.h"   // RecorderOptions / ScreenRecorder 定义在这(namespace re)

namespace re {

class RecordingService {
public:
    RecordingService() = default;
    ~RecordingService() = default;

    // ---- 底层启动(直接给 RecorderOptions) ----
    bool Start(RecorderOptions opts);
    bool Stop();
    bool Pause();
    bool Resume();
    bool IsActive() const;

    // ---- 高层 UI 状态启动 ----
    // 把 QML 选择的各种状态组装成 RecorderOptions 并做前置校验。
    // 返回值: {ok, status_text, error_text}
    // status_text 用于 setStatus, error_text 用于 emit errorOccurred。
    struct StartResult {
        bool ok = false;
        QString status;   // Controller.setStatus 用
        QString error;    // Controller.emit errorOccurred 用(可为空)
    };

    // UI 端的输入状态:Controller 只传这些,不再自己做校验/组装。
    struct UiState {
        QString output_path;               // Q_PROPERTY
        int speed_multiplier = 1;          // Q_PROPERTY
        int segment_minutes = 0;           // Q_PROPERTY, 0=不分段, >0 按分钟切文件
        QString video_device;              // Q_PROPERTY ("desktop"/设备名/"composite")
        QString secondary_video_device;    // Q_PROPERTY
        QString audio_device;              // Q_PROPERTY ("mixed"/设备名)
        QString primary_audio_device;      // Q_PROPERTY
        QString secondary_audio_device;    // Q_PROPERTY
        int record_mode = 0;               // Q_PROPERTY (0=全屏 1=区域 2=窗口)
        QString window_title;              // Q_PROPERTY
        int region_offset_x = 0;           // 物理像素(Controller 已做 DPR 换算)
        int region_offset_y = 0;
        int region_width = 0;
        int region_height = 0;
    };

    StartResult StartFromUiState(const UiState& state);

private:
    std::unique_ptr<ScreenRecorder> recorder_;
};

}  // namespace re

#endif  // RECORDING_SERVICE_H
