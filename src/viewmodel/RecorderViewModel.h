#ifndef RECORD_EXPERT_RECORDER_VIEW_MODEL_H
#define RECORD_EXPERT_RECORDER_VIEW_MODEL_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <memory>

#include "ScreenRecorder.h"
#include "ScreenshotImageProvider.h"
#include "RecordingService.h"
#include "ScreenshotService.h"

class QQuickWindow;

namespace re {

// RecorderViewModel（ViewModel 层）是 QML 与 Service 层之间的薄桥接。
//
// 它的职责严格限定为:
//   1. **QML 绑定**: 暴露 13 个 Q_PROPERTY + 27 个 Q_INVOKABLE，供 QML 属性绑定
//      与命令调用；发出 14 个状态变化信号。
//   2. **状态管理**: 维护录制状态机（Idle → Recording ⇄ Paused → Idle）、
//      计时器（wall_clock_ + paused_total_ms_ + ticker_）、区域录制暂存坐标。
//   3. **命令转发**: start() 组装 UiState → 委托 RecordingService.StartFromUiState;
//      截图委托 ScreenshotService;窗口显隐(4 个 QQuickWindow* 的 show/hide)
//      留在本类——Qt 5.12 下 QML 对 Window-in-Window 静态实例的 visible 赋值
//      会被静默吞掉,必须走 C++。
//
// 已下放到 Service 层、本类不再承担的职责:
//   - RecorderOptions 组装 + 前置校验 → RecordingService.StartFromUiState
//   - ScreenRecorder 生命周期 → RecordingService
//   - DirectShow 设备/窗口枚举 → EnumerationService
//   - 区域截图 pending 坐标暂存 → ScreenshotService(BeginSaveSession)
//
// 设计原则: Controller 不直接持有 ScreenRecorder,只通过 RecordingService
// 间接访问;所有耗时操作在 Service 内部完成,Controller 保持 GUI 线程响应式。
class RecorderViewModel : public QObject {
    Q_OBJECT
    // ---- 状态暴露 ----
    // 录制状态:true 表示后台 worker 正在跑(含暂停状态)。
    Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingChanged)
    // 暂停状态:仅当 isRecording 为 true 时有意义。
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY pausedChanged)
    // 录制输出 MP4 路径,可由 QML TextField 双向绑定。
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath NOTIFY outputPathChanged)
    // 状态/错误文本,用于在 QML Label 上显示提示。
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // 倍率录制档位(1/2/4/8)。N>1 时把 N 秒墙钟内容压缩为 1 秒 mp4 输出。
    Q_PROPERTY(int speedMultiplier READ speedMultiplier WRITE setSpeedMultiplier NOTIFY speedMultiplierChanged)
    // 分段录制档位(0=不分段, >0 按分钟切文件)。录制开始后禁用。
    Q_PROPERTY(int segmentMinutes READ segmentMinutes WRITE setSegmentMinutes NOTIFY segmentMinutesChanged)
    // 计时器文本 "HH:MM:SS",显示有效录制时长(扣除暂停区间)。
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY elapsedTextChanged)
    // 录制范围模式:0=全屏, 1=区域, 2=指定窗口。
    Q_PROPERTY(int recordMode READ recordMode WRITE setRecordMode)
    // 指定窗口录制的窗口标题(UTF-8)。recordMode==2 时生效。
    Q_PROPERTY(QString windowTitle READ windowTitle WRITE setWindowTitle)
    // 视频设备名。"desktop"=gdigrab 桌面;其他为 dshow 摄像头;"composite"=PiP。
    Q_PROPERTY(QString videoDevice READ videoDevice WRITE setVideoDevice NOTIFY videoDeviceChanged)
    // 画中画摄像头设备名(videoDevice=="composite" 时生效)。
    Q_PROPERTY(QString secondaryVideoDevice READ secondaryVideoDevice WRITE setSecondaryVideoDevice NOTIFY secondaryVideoDeviceChanged)
    // 音频设备名。"mixed"=麦克风+系统声音混音;其他为 dshow 单源。
    Q_PROPERTY(QString audioDevice READ audioDevice WRITE setAudioDevice NOTIFY audioDeviceChanged)
    // 混音主源设备名(audioDevice=="mixed" 时生效,麦克风)。
    Q_PROPERTY(QString primaryAudioDevice READ primaryAudioDevice WRITE setPrimaryAudioDevice NOTIFY primaryAudioDeviceChanged)
    // 混音副源设备名(audioDevice=="mixed" 时生效,系统声音/立体声混音)。
    Q_PROPERTY(QString secondaryAudioDevice READ secondaryAudioDevice WRITE setSecondaryAudioDevice NOTIFY secondaryAudioDeviceChanged)

public:
    explicit RecorderViewModel(QObject* parent = nullptr);
    ~RecorderViewModel() override;

    bool isRecording() const { return recording_; }
    bool isPaused() const { return paused_; }
    QString outputPath() const { return outputPath_; }
    QString status() const { return status_; }
    int speedMultiplier() const { return speedMultiplier_; }
    int segmentMinutes() const { return segmentMinutes_; }
    QString elapsedText() const;
    int recordMode() const { return record_mode_; }
    QString windowTitle() const { return window_title_; }
    QString videoDevice() const { return video_device_; }
    QString secondaryVideoDevice() const { return secondary_video_device_; }
    QString audioDevice() const { return audio_device_; }
    QString primaryAudioDevice() const { return primary_audio_device_; }
    QString secondaryAudioDevice() const { return secondary_audio_device_; }

    void setOutputPath(const QString& path);
    void setSpeedMultiplier(int v);
    void setSegmentMinutes(int v);
    void setRecordMode(int v);
    void setWindowTitle(QString t);
    Q_INVOKABLE void setVideoDevice(QString t);
    Q_INVOKABLE void setSecondaryVideoDevice(QString t);
    Q_INVOKABLE void setAudioDevice(QString t);
    Q_INVOKABLE void setPrimaryAudioDevice(QString t);
    Q_INVOKABLE void setSecondaryAudioDevice(QString t);

    // 由 main.cpp 在 engine.load 之后注入:
    //   setMainWindow: 顶层 QQuickWindow,区域截图/录制时 hide。
    //   setImageProvider: 同截图 provider,不持有所有权(engine 持有)。
    // 同时转发给 ScreenshotService(截图会话需要这些依赖)。
    void setMainWindow(QQuickWindow* w);
    void setImageProvider(ScreenshotImageProvider* p);

public slots:
    // ---- 录制命令 ----
    // 组装 UI 状态 → 调用 RecordingService.StartFromUiState → 处理结果。
    Q_INVOKABLE void start();
    // 暂停录制:转发 recordingService_.Pause()。仅当录制中且未暂停时有效。
    Q_INVOKABLE void pause();
    // 继续录制:转发 recordingService_.Resume()。仅当暂停时有效。
    Q_INVOKABLE void resume();
    // 结束录制:recordingService_.Stop() 收尾 MP4,销毁 ScreenRecorder。
    Q_INVOKABLE void stop();
    // 截一张 PNG 到 path(未指定或空时用默认文件名)。
    Q_INVOKABLE void takeScreenshot(QString path);
    // 打开内置视频播放器播放 outputPath。
    Q_INVOKABLE void playInBuiltInPlayer();

    // ---- 窗口注入(QML 静态实例,Qt 5.12 下必须 C++ show/hide) ----
    Q_INVOKABLE void setOverlayWindow(QObject* w);
    Q_INVOKABLE void setRecordingOverlayWindow(QObject* w);
    Q_INVOKABLE void setPlayerWindow(QObject* w);
    Q_INVOKABLE void hideRegionOverlay();
    Q_INVOKABLE void restoreMainWindow();

    // ---- 区域截图 ----
    Q_INVOKABLE bool startRegionScreenshot();
    Q_INVOKABLE void copyRegionScreenshot(int x, int y, int w, int h);
    Q_INVOKABLE void requestSave(int x, int y, int w, int h);
    Q_INVOKABLE void completeSave(QUrl url);
    Q_INVOKABLE void cancelSave();
    Q_INVOKABLE void cancelRegionScreenshot();

    // ---- 区域录制 ----
    Q_INVOKABLE bool startRegionRecordingCapture();
    Q_INVOKABLE void startRegionRecording(int x, int y, int w, int h);

    // ---- 指定窗口 / DirectShow 设备枚举 ----
    Q_INVOKABLE QVariantList enumerateWindows();
    Q_INVOKABLE QVariantList enumerateVideoDevices();
    Q_INVOKABLE QVariantList enumerateAudioDevices();

signals:
    void recordingChanged();
    void pausedChanged();
    void outputPathChanged();
    void statusChanged();
    void speedMultiplierChanged();
    void segmentMinutesChanged();
    void elapsedTextChanged();
    void videoDeviceChanged();
    void secondaryVideoDeviceChanged();
    void audioDeviceChanged();
    void primaryAudioDeviceChanged();
    void secondaryAudioDeviceChanged();
    // 业务事件信号,供 QML 弹出提示或刷新界面。
    void saveDialogRequested();
    void playInBuiltInPlayerRequested(const QString& path);
    void errorOccurred(const QString& message);

private:
    void setRecording(bool v);
    void setPaused(bool v);
    void setStatus(const QString& s);
    // 把状态机三个 bool 状态合并刷新:Recording / Paused / status 文本。
    void refreshStatus();
    // 每秒触发的计时 tick:更新 elapsedText 并发出信号。
    void onTimerTick();
    // 悬浮窗显隐:定位到主屏右下角(避开任务栏)再 show()。
    void ShowRecordingOverlay();
    void HideRecordingOverlay();

    // ---- Service 层 ----
    RecordingService recording_service_;  // ScreenRecorder 生命周期代理
    ScreenshotService screenshot_service_{this};  // 截图业务代理(parent=this 自动析构)

    // ---- UI 状态 ----
    bool recording_ = false;
    bool paused_ = false;
    QString outputPath_ = QStringLiteral("output.mp4");
    QString status_ = QStringLiteral("Idle");
    int speedMultiplier_ = 1;
    int segmentMinutes_ = 0;  // 0=不分段, >0 按分钟切文件。
    int record_mode_ = 0;  // 0=全屏, 1=区域, 2=指定窗口。
    QString window_title_;
    QString video_device_ = QStringLiteral("desktop");
    QString secondary_video_device_;  // PiP 摄像头名(video_device_=="composite")。
    QString audio_device_;
    QString primary_audio_device_;    // 混音主源(麦克风,audio_device_=="mixed")。
    QString secondary_audio_device_;  // 混音副源(系统声音,audio_device_=="mixed")。

    // ---- 计时器 ----
    QTimer*          ticker_ = nullptr;       // 200ms 间隔,驱动 UI 计时刷新。
    QElapsedTimer    wall_clock_;             // 墙钟起点,start() 时 start()。
    qint64           paused_total_ms_ = 0;    // 累计暂停毫秒数。
    qint64           pause_start_ms_ = -1;    // 当前暂停段起点(暂停中有效,否则 -1)。
    mutable QString   cached_elapsed_;          // 缓存上次发出的文本,避免无变化时 emit。

    // ---- 窗口引用(QQuickWindow,非 owned) ----
    ScreenshotImageProvider* provider_            = nullptr;
    QQuickWindow*            main_window_         = nullptr;
    QQuickWindow*            overlay_window_      = nullptr;
    QQuickWindow*            recording_overlay_   = nullptr;
    QQuickWindow*            player_window_       = nullptr;
    bool                     region_active_        = false;
    // 区域录制过程中暂存的物理像素坐标(startRegionRecording 写入,start() 读取)。
    int region_offset_x_ = 0, region_offset_y_ = 0;
    int region_width_ = 0, region_height_ = 0;
};

}  // namespace re

#endif  // RECORD_EXPERT_RECORDER_VIEW_MODEL_H
