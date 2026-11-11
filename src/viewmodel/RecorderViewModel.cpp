#include "RecorderViewModel.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QString>
#include <QUrl>

#include <utility>

#include "EnumerationService.h"

namespace re {

RecorderViewModel::RecorderViewModel(QObject* parent)
    : QObject(parent) {
    // 默认采集参数已在 RecordingService::StartFromUiState 里预置。
    // 高频 tick(200ms)驱动计时器文本刷新:Windows 默认时钟分辨率 ~15.6ms,
    // 1000ms 间隔的 QTimer 容易因主线程卡顿积压合并,导致秒数从 N 直接跳到 N+2
    // (跳过 N+1 没显示)——用户感知为"跳秒"。200ms 间隔下即使主线程卡了 1 秒,
    // 恢复后最多积压 5 个 tick,每次 elapsedText() 从 wall_clock_ 算真实值,
    // 靠 cached_elapsed_ 去抖只在秒数变化时 emit,最多延迟 200ms 显示不会跨秒。
    ticker_ = new QTimer(this);
    ticker_->setTimerType(Qt::PreciseTimer);  // Windows 上触发 timeBeginPeriod
    ticker_->setInterval(200);
    ticker_->setSingleShot(false);
    connect(ticker_, &QTimer::timeout, this,
            &RecorderViewModel::onTimerTick);
}

RecorderViewModel::~RecorderViewModel() {
    if (ticker_) ticker_->stop();
    recording_service_.Stop();  // Stop 内部会 reset ScreenRecorder
}

void RecorderViewModel::setOutputPath(const QString& path) {
    if (path == outputPath_) return;
    outputPath_ = path;
    emit outputPathChanged();
}

void RecorderViewModel::setSpeedMultiplier(int v) {
    // 校验值域：仅允许 1/2/4/8。越界回退 1（等同实时录制）。
    if (v != 1 && v != 2 && v != 4 && v != 8) v = 1;
    if (v == speedMultiplier_) return;
    speedMultiplier_ = v;
    emit speedMultiplierChanged();
}

void RecorderViewModel::setSegmentMinutes(int v) {
    // 校验值域：0（不分段）或正整数分钟（≥1）。负数回退 0。
    if (v < 0) v = 0;
    if (v == segmentMinutes_) return;
    segmentMinutes_ = v;
    emit segmentMinutesChanged();
}

// ---- BuildOptions ----
// 把所有 Q_PROPERTY 的 UI 状态一次性组装成 RecorderOptions。
// 包含输入校验:设备为空时报错返回带错误状态的 opts(调用方可查)。
void RecorderViewModel::start() {
    if (recording_) {
        setStatus(QStringLiteral("Already recording"));
        return;
    }

    // 组装 UI 状态 → 委托 RecordingService 做校验 + 参数组装 + 启动
    RecordingService::UiState s;
    s.output_path = outputPath_;
    s.speed_multiplier = speedMultiplier_;
    s.segment_minutes = segmentMinutes_;
    s.video_device = video_device_;
    s.secondary_video_device = secondary_video_device_;
    s.audio_device = audio_device_;
    s.primary_audio_device = primary_audio_device_;
    s.secondary_audio_device = secondary_audio_device_;
    s.record_mode = record_mode_;
    s.window_title = window_title_;
    s.region_offset_x = region_offset_x_;
    s.region_offset_y = region_offset_y_;
    s.region_width = region_width_;
    s.region_height = region_height_;

    const auto result = recording_service_.StartFromUiState(s);
    if (!result.ok) {
        setStatus(result.status);
        if (!result.error.isEmpty()) emit errorOccurred(result.error);
        return;
    }

    setRecording(true);
    setPaused(false);
    refreshStatus();

    // 重置计时器并启动:墙钟起点 = 现在,累计暂停 = 0,暂停段起点标记为 -1。
    wall_clock_.start();
    paused_total_ms_ = 0;
    pause_start_ms_ = -1;
    ticker_->start();
    onTimerTick();
}

void RecorderViewModel::pause() {
    if (!recording_ || paused_) return;
    if (recording_service_.Pause()) {
        setPaused(true);
        refreshStatus();
        // 暂停段起点锁定:后续 resume 时累计 (wall_clock - pause_start) 到
        // paused_total_ms_,从有效时间里扣除。
        pause_start_ms_ = wall_clock_.elapsed();
        onTimerTick();
    }
}

void RecorderViewModel::resume() {
    if (!paused_) return;
    if (recording_service_.Resume()) {
        setPaused(false);
        refreshStatus();
        if (pause_start_ms_ >= 0) {
            paused_total_ms_ += wall_clock_.elapsed() - pause_start_ms_;
            pause_start_ms_ = -1;
        }
        onTimerTick();
    }
}

void RecorderViewModel::stop() {
    if (!recording_ || !recording_service_.IsActive()) {
        setRecording(false);
        setPaused(false);
        return;
    }
    const bool stop_ok = recording_service_.Stop();

    // 补最后一段暂停 + 停 ticker + 归零计时状态。
    if (pause_start_ms_ >= 0) {
        paused_total_ms_ += wall_clock_.elapsed() - pause_start_ms_;
        pause_start_ms_ = -1;
    }
    ticker_->stop();
    setPaused(false);
    setRecording(false);
    wall_clock_.invalidate();
    paused_total_ms_ = 0;
    pause_start_ms_ = -1;
    cached_elapsed_.clear();
    emit elapsedTextChanged();

    refreshStatus();
    if (!stop_ok) {
        emit errorOccurred(QStringLiteral("Recording finished with errors"));
    }
}

void RecorderViewModel::playInBuiltInPlayer() {
    // 录制期间文件还在写,不能播放。
    if (recording_) {
        emit errorOccurred(QStringLiteral("录制中,请先停止再播放"));
        return;
    }
    // 解析绝对路径
    QString abs = outputPath_;
    if (!QFileInfo(abs).isAbsolute())
        abs = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(abs);
    if (!QFileInfo::exists(abs)) {
        emit errorOccurred(QStringLiteral("文件不存在: %1").arg(outputPath_));
        return;
    }
    // 先 emit 信号让 QML 把 videoSource 赋好(QML Connections 同步处理),
    // 再由 C++ show():Qt 5.12 下 QML 对 Window-in-Window 静态实例的
    // show() 会被静默吞掉,必须走 C++(同 RegionCaptureWindow/RecordingOverlay)。
    // PlayerWindow 的 onVisibleChanged 在 C++ show 后触发,内部完成 open+play。
    emit playInBuiltInPlayerRequested(abs);
    if (player_window_) {
        player_window_->show();
        player_window_->raise();
        player_window_->requestActivate();
    }
}

void RecorderViewModel::takeScreenshot(QString path) {
    QString saved;
    if (screenshot_service_.TakeFullscreenPng(path, /*draw_timestamp=*/true, &saved)) {
        setStatus(QStringLiteral("Screenshot saved: ") + saved);
    } else {
        setStatus(QStringLiteral("Screenshot failed"));
        emit errorOccurred(QStringLiteral("Failed to capture screenshot"));
    }
}

bool RecorderViewModel::startRegionScreenshot() {
    // 标记区域截图进行中,便于 restoreMainWindow 在异常路径也能恢复主窗。
    region_active_ = true;
    if (!screenshot_service_.StartRegionSession()) {
        region_active_ = false;
        emit errorOccurred(QStringLiteral("Failed to start region screenshot"));
        return false;
    }
    setStatus(QStringLiteral("Select region"));
    return true;
}

// 注入覆盖窗口（QML 静态实例）。显隐由 C++ 控制的原因见头文件注释：
// Qt 5.12 下 QML 对 Window-in-Window 实例的 visible=true 赋值被静默吞掉。
void RecorderViewModel::setMainWindow(QQuickWindow* w) {
    main_window_ = w;
    screenshot_service_.setMainWindow(w);
}

void RecorderViewModel::setImageProvider(ScreenshotImageProvider* p) {
    provider_ = p;
    screenshot_service_.setImageProvider(p);
}

void RecorderViewModel::setOverlayWindow(QObject* w) {
    overlay_window_ = qobject_cast<QQuickWindow*>(w);
    screenshot_service_.setOverlayWindow(overlay_window_);
}

// 注入录制悬浮窗(QML 静态实例 RecordingOverlay)。显隐由 C++ 在 setRecording
// 切换时控制,理由同 setOverlayWindow。
void RecorderViewModel::setRecordingOverlayWindow(QObject* w) {
    recording_overlay_ = qobject_cast<QQuickWindow*>(w);
}

// 注入视频播放器窗口(QML 静态实例 PlayerWindow)。显隐由 C++ 控制,
// 理由同 setOverlayWindow:Qt 5.12 下 QML 对 Window-in-Window 静态
// 实例的 show() 会被静默吞掉。
void RecorderViewModel::setPlayerWindow(QObject* w) {
    player_window_ = qobject_cast<QQuickWindow*>(w);
    if (player_window_) {
        // 进程退出兜底:aboutToQuit 在事件循环内发射,趁机关闭播放器窗口
        // (onClosing → videoPlayer.stop()),让解码线程停止与 QAudioOutput
        // suspend 都发生在事件循环存活期;exec() 返回后的析构路径上再操作
        // Qt Multimedia 后端有挂死风险(实测:播放中直接 quit 进程不退)。
        connect(qApp, &QCoreApplication::aboutToQuit,
                player_window_, &QQuickWindow::close,
                Qt::UniqueConnection);
    }
}

// 显示录制悬浮窗:定位到主屏右下角(避开任务栏,留 16px 边距)再 show()。
// availableSize 已扣除任务栏高度,贴右下角自然在任务栏上方。
void RecorderViewModel::ShowRecordingOverlay() {
    if (!recording_overlay_) return;
    QScreen* scr = QGuiApplication::primaryScreen();
    if (scr) {
        // availableSize 返回扣除任务栏后的可用区域(逻辑像素)。
        const QSize avail = scr->availableSize();
        const int margin = 16;
        const int w = recording_overlay_->width();
        const int h = recording_overlay_->height();
        const int x = avail.width() - w - margin;
        const int y = avail.height() - h - margin;
        recording_overlay_->setX(x);
        recording_overlay_->setY(y);
    }
    recording_overlay_->show();
}

// 隐藏录制悬浮窗。
void RecorderViewModel::HideRecordingOverlay() {
    if (recording_overlay_) recording_overlay_->hide();
}

// 隐藏覆盖窗口。供 QML confirm/cancel 调用；save/cancel 的 C++ 内部路径
// 也直接调用。
void RecorderViewModel::hideRegionOverlay() {
    if (overlay_window_) overlay_window_->hide();
}

void RecorderViewModel::copyRegionScreenshot(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        cancelRegionScreenshot();
        return;
    }
    if (screenshot_service_.CopyRegion(x, y, w, h)) {
        setStatus(QStringLiteral("Screenshot copied to clipboard"));
    } else {
        setStatus(QStringLiteral("Region screenshot failed"));
        emit errorOccurred(QStringLiteral("No captured image"));
    }
    region_active_ = false;
}

void RecorderViewModel::requestSave(int x, int y, int w, int h) {
    // 存选区坐标到 Service,供 completeSave 跨 FileDialog 异步间隙使用。
    screenshot_service_.BeginSaveSession(x, y, w, h);
    // 先隐藏覆盖窗+恢复主窗,让 FileDialog 不被全屏 overlay 挡住。
    // provider 的图不清——completeSave 还要用它裁剪。
    hideRegionOverlay();
    restoreMainWindow();
    // 通知 QML(MainWindow)打开 FileDialog。
    emit saveDialogRequested();
}

void RecorderViewModel::completeSave(QUrl url) {
    QString path = url.toLocalFile();
    if (!screenshot_service_.HasPendingSave()) {
        cancelRegionScreenshot();
        return;
    }
    if (screenshot_service_.CompleteSaveWithPending(path)) {
        setStatus(QStringLiteral("Screenshot saved: ") + path);
    } else {
        setStatus(QStringLiteral("Region screenshot failed"));
        emit errorOccurred(QStringLiteral("Failed to save region screenshot"));
    }
    region_active_ = false;
}

void RecorderViewModel::cancelSave() {
    // 用户取消 FileDialog:让 Service 清掉 pending 暂存(不触发 FinishSession,
    // 因为 overlay 和主窗已在 requestSave 里隐藏/恢复)。
    if (screenshot_service_.HasPendingSave()) {
        screenshot_service_.CancelSession();  // FinishSession 会连带清 pending_save_active_
    } else {
        // 极端异常路径:既没 pending 又没 session,手动清理残留。
        if (provider_) provider_->clear();
    }
    region_active_ = false;
    setStatus(QStringLiteral("Idle"));
}

void RecorderViewModel::cancelRegionScreenshot() {
    screenshot_service_.CancelSession();
    region_active_ = false;
    setStatus(QStringLiteral("Idle"));
}

void RecorderViewModel::restoreMainWindow() {
    if (!region_active_) return;
    region_active_ = false;
    if (main_window_) main_window_->show();
}

void RecorderViewModel::setRecording(bool v) {
    if (recording_ == v) return;
    recording_ = v;
    // 录制开始:隐藏主窗 + 显示右下角悬浮控制窗(暂停/继续/停止 + 计时)。
    // 录制结束:隐藏悬浮窗 + 恢复主窗。区域录制路径下主窗在选区前置流程里
    // 已隐藏,这里 hide 是幂等操作,不会闪现空白窗口。
    if (v) {
        if (main_window_) main_window_->hide();
        ShowRecordingOverlay();
    } else {
        HideRecordingOverlay();
        if (main_window_) main_window_->show();
    }
    emit recordingChanged();
}

void RecorderViewModel::setPaused(bool v) {
    if (paused_ == v) return;
    paused_ = v;
    emit pausedChanged();
}

void RecorderViewModel::setStatus(const QString& s) {
    if (status_ == s) return;
    status_ = s;
    emit statusChanged();
}

void RecorderViewModel::refreshStatus() {
    // 不直接覆盖 takeScreenshot 设置的"screenshot saved"提示：仅在状态切换时更新。
    if (!recording_) {
        setStatus(QStringLiteral("Idle"));
    } else if (paused_) {
        setStatus(QStringLiteral("Paused"));
    } else {
        setStatus(QStringLiteral("Recording"));
    }
}

QString RecorderViewModel::elapsedText() const {
    // 有效录制时间 = 墙钟经过时间 - 累计暂停时长。暂停期间用 pause_start_ms_
    // （暂停开始那一刻的墙钟位置）替代 wall_clock_.elapsed()，让计时停住。
    // wall_clock_ 未 start/invalidate 后直接返回 0，防御性兜底，不依赖
    // recording_ 的设置顺序。
    qint64 effective_ms = 0;
    if (wall_clock_.isValid()) {
        if (paused_ && pause_start_ms_ >= 0) {
            effective_ms = pause_start_ms_ - paused_total_ms_;
        } else {
            effective_ms = wall_clock_.elapsed() - paused_total_ms_;
        }
    }
    if (effective_ms < 0) effective_ms = 0;

    const qint64 total_sec = effective_ms / 1000;
    const qint64 h = total_sec / 3600;
    const qint64 m = (total_sec % 3600) / 60;
    const qint64 s = total_sec % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

void RecorderViewModel::onTimerTick() {
    // 缓存上次值，文本无变化时不 emit，避免 QML 无意义重绘。
    const QString now = elapsedText();
    if (now != cached_elapsed_) {
        cached_elapsed_ = now;
        emit elapsedTextChanged();
    }
}

// ---------------------------------------------------------------------------
// 录制范围模式切换
// ---------------------------------------------------------------------------

void RecorderViewModel::setRecordMode(int v) {
    if (v == record_mode_) return;
    // 仅允许 0/1/2 三种模式，越界回退 0（全屏）。
    if (v < 0 || v > 2) v = 0;
    record_mode_ = v;
}

void RecorderViewModel::setWindowTitle(QString t) {
    if (t == window_title_) return;
    window_title_ = std::move(t);
}

// ---------------------------------------------------------------------------
// 区域录制
// ---------------------------------------------------------------------------

// 区域录制前置流程:与 startRegionScreenshot 完全相同——隐藏主窗、150ms 后抓
// 帧、存 provider、显示覆盖窗。差异仅在覆盖窗里的工具栏:录屏模式显示 🎬 确认
// 按钮(调 startRegionRecording)而非 ✓ 复制/💾 保存。复用同一覆盖窗 + provider。
bool RecorderViewModel::startRegionRecordingCapture() {
    if (!provider_) {
        emit errorOccurred(QStringLiteral("Image provider not configured"));
        return false;
    }
    if (recording_) {
        emit errorOccurred(QStringLiteral("Already recording"));
        return false;
    }
    // 标记区域录制前置流程进行中(供 restoreMainWindow 在异常路径恢复主窗)。
    region_active_ = true;
    // 直接复用 ScreenshotService.StartRegionSession:它负责
    // hide main → 150ms → CaptureFullscreenFrame → provider → show overlay。
    // 后续用户画选区 → startRegionRecording 确认时调 EndSession 清标志。
    if (!screenshot_service_.StartRegionSession()) {
        region_active_ = false;
        emit errorOccurred(QStringLiteral("Failed to start region capture"));
        return false;
    }
    setStatus(QStringLiteral("Select region"));
    return true;
}

// 区域选完确认:把选区坐标写入 region_*_ 成员、EndSession 清 Service 状态、
// 隐藏覆盖窗、立即 start()。注意:这里不恢复主窗——主窗从选区前置流程起一直处于
// 隐藏态,若先 show 再由 setRecording(true) hide,主窗会闪现一帧未渲染的空白。
// 主窗保持隐藏,恢复交给 setRecording(false)(停止录制时)。
void RecorderViewModel::startRegionRecording(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        // 用户画了个空选区:走正常取消路径。
        screenshot_service_.CancelSession();  // FinishSession 清 provider + 恢复主窗
        region_active_ = false;
        setStatus(QStringLiteral("Region too small"));
        emit errorOccurred(QStringLiteral("Please select a valid region"));
        return;
    }
    // 先告诉 Service "会话结束但不碰窗口"——窗口显隐由我们自己控制。
    screenshot_service_.EndSession();

    // 高分屏坐标换算:QML 传的是覆盖窗逻辑坐标,gdigrab 需要物理像素坐标。
    if (overlay_window_ && overlay_window_->width() > 0
            && overlay_window_->height() > 0) {
        QScreen* scr = QGuiApplication::primaryScreen();
        const double dpr = scr ? scr->devicePixelRatio() : 1.0;
        if (dpr > 1.0) {
            x = qRound(x * dpr);
            y = qRound(y * dpr);
            w = qRound(w * dpr);
            h = qRound(h * dpr);
        }
    }
    region_offset_x_ = x;
    region_offset_y_ = y;
    region_width_ = w;
    region_height_ = h;
    // 清 provider(不影响什么,只是保持状态整洁)。
    if (provider_) provider_->clear();
    // 隐藏覆盖窗,立即 start()。只清 region_active_ 标志,不 show 主窗。
    hideRegionOverlay();
    region_active_ = false;
    start();
    // start() 失败时不会走到 setRecording(true),主窗仍处于隐藏态,
    // 补一次恢复,避免用户界面卡死(悬浮控制窗也不会出现)。
    if (!recording_ && main_window_) main_window_->show();
}

// ---------------------------------------------------------------------------
// 指定窗口 / DirectShow 设备枚举(已下放到 EnumerationService)
// ---------------------------------------------------------------------------

QVariantList RecorderViewModel::enumerateWindows() {
    return EnumerationService::EnumerateWindows();
}

QVariantList RecorderViewModel::enumerateVideoDevices() {
    return EnumerationService::EnumerateVideoDevices();
}

QVariantList RecorderViewModel::enumerateAudioDevices() {
    return EnumerationService::EnumerateAudioDevices();
}

void RecorderViewModel::setVideoDevice(QString t) {
    if (t == video_device_) return;
    video_device_ = std::move(t);
    emit videoDeviceChanged();
}

void RecorderViewModel::setSecondaryVideoDevice(QString t) {
    if (t == secondary_video_device_) return;
    secondary_video_device_ = std::move(t);
    emit secondaryVideoDeviceChanged();
}

void RecorderViewModel::setAudioDevice(QString t) {
    if (t == audio_device_) return;
    audio_device_ = std::move(t);
    emit audioDeviceChanged();
}

void RecorderViewModel::setPrimaryAudioDevice(QString t) {
    if (t == primary_audio_device_) return;
    primary_audio_device_ = std::move(t);
    emit primaryAudioDeviceChanged();
}

void RecorderViewModel::setSecondaryAudioDevice(QString t) {
    if (t == secondary_audio_device_) return;
    secondary_audio_device_ = std::move(t);
    emit secondaryAudioDeviceChanged();
}

}  // namespace re
