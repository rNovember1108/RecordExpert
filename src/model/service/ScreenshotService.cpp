#include "ScreenshotService.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QQuickWindow>
#include <QTimer>

#include "Screenshot.h"
#include "ScreenshotImageProvider.h"
#include "Watermark.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace re {

// ---- 静态辅助 ----

namespace {

QString TimestampForFilename() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
}

QString EnsurePngExtension(QString path) {
    const QString lower = path.toLower();
    if (!lower.endsWith(QStringLiteral(".png"))) {
        path += QStringLiteral(".png");
    }
    return path;
}

}  // namespace

// ===========================================================================
// 静态纯函数层:与 Controller/窗口完全解耦
// ===========================================================================

bool ScreenshotService::TakeFullscreenPng(const QString& path,
                                          bool draw_timestamp,
                                          QString* saved_path) {
    QString png_path = path.isEmpty()
        ? QStringLiteral("screenshot_%1.png").arg(TimestampForFilename())
        : EnsurePngExtension(path);

    ScreenshotOptions opts;
    opts.draw_timestamp = draw_timestamp;
    const bool ok = CapturePngScreenshot(png_path.toStdString(), opts);
    if (ok && saved_path)
        *saved_path = png_path;
    return ok;
}

QImage ScreenshotService::CaptureFullscreenFrame() {
    // 默认 ScreenshotOptions:fullscreen (offset/size = 0), draw_timestamp = false
    // (不加水印,供后续按需裁剪/叠加)。
    AVFrame* frame = nullptr;
    ScreenshotOptions opts;
    opts.draw_timestamp = false;

    if (!CaptureDesktopFrame(&frame, opts)) {
        return QImage();
    }
    QImage img = FrameToQImage(frame);
    av_frame_free(&frame);
    return img;
}

QImage ScreenshotService::CropRegion(const QImage& full, int x, int y, int w, int h,
                                     int ref_w, int ref_h,
                                     bool draw_watermark) {
    if (full.isNull() || w <= 0 || h <= 0) return QImage();

    // 高分屏换算:选区坐标是覆盖窗逻辑坐标,抓帧图是物理像素。
    if (ref_w > 0 && ref_h > 0) {
        const double sx = double(full.width()) / ref_w;
        const double sy = double(full.height()) / ref_h;
        x = qRound(x * sx);
        y = qRound(y * sy);
        w = qRound(w * sx);
        h = qRound(h * sy);
    }
    // 夹到图像边界内。
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w > full.width() - x) w = full.width() - x;
    if (h > full.height() - y) h = full.height() - y;
    if (w <= 0 || h <= 0) return QImage();

    QImage crop = full.copy(x, y, w, h);
    if (draw_watermark) {
        DrawLabelWatermark(&crop, kWatermarkLabel);
        DrawTimestampWatermark(&crop);
    }
    return crop;
}

bool ScreenshotService::SavePng(const QImage& img, const QString& path) {
    if (img.isNull() || path.isEmpty()) return false;
    return img.save(path, "PNG");
}

// ===========================================================================
// 区域截图会话:实例方法,依赖注入的窗口/Provider
// ===========================================================================

bool ScreenshotService::StartRegionSession() {
    if (!provider_ || !overlay_window_) return false;

    // 标记会话中,供 FinishSession / RestoreMainWindow 区分异常路径。
    session_active_ = true;
    // 隐藏主窗(避免自身出现在被俘桌面帧里,微信截图同款体验)。
    // OS 重绘需要时间,150ms 经验值足够让桌面无主窗。
    if (main_window_) main_window_->hide();

    // 150ms 后抓整屏、存 provider、显示覆盖窗。
    // 用单成员 QTimer::singleShot 避免 QObject 生命周期问题。
    QTimer::singleShot(150, [this]() {
        QImage full = CaptureFullscreenFrame();
        if (full.isNull()) {
            // 抓帧失败,恢复主窗、复位状态。
            FinishSession();
            return;
        }
        provider_->setCapturedImage(full);

        // 主屏逻辑尺寸(QML Screen 附加属性不可用,由 C++ 传入)。
        QScreen* scr = QGuiApplication::primaryScreen();
        const QSize sz = scr ? scr->size() : QSize(0, 0);

        // 覆盖窗显示前重置交互状态。
        overlay_window_->setProperty("scrW", sz.width());
        overlay_window_->setProperty("scrH", sz.height());
        // 让 QML Image 重新向 provider 取图。
        const int cur = overlay_window_->property("reload").toInt();
        overlay_window_->setProperty("reload", cur + 1);
        overlay_window_->setProperty("selRect", QRectF());
        overlay_window_->setProperty("dragging", false);

        overlay_window_->show();
    });
    return true;
}

bool ScreenshotService::CompleteSave(int x, int y, int w, int h,
                                      const QString& file_path) {
    if (!session_active_ || !provider_) return false;

    // provider 存的是 startRegionSession 时抓的整屏原图。
    QImage full = provider_->capturedImage();
    if (full.isNull()) {
        FinishSession();
        return false;
    }

    // 用覆盖窗逻辑尺寸作为参考,让 CropRegion 按比例换算高分屏坐标。
    const int ref_w = overlay_window_ ? overlay_window_->width() : 0;
    const int ref_h = overlay_window_ ? overlay_window_->height() : 0;
    QImage crop = CropRegion(full, x, y, w, h, ref_w, ref_h, true);

    // 清 provider(不影响已保存的图,保持会话干净)。
    if (provider_) provider_->clear();
    FinishSession();

    if (crop.isNull()) return false;

    QString path = file_path.isEmpty()
        ? QStringLiteral("screenshot_%1.png").arg(TimestampForFilename())
        : EnsurePngExtension(file_path);
    return SavePng(crop, path);
}

bool ScreenshotService::CopyRegion(int x, int y, int w, int h) {
    if (!session_active_ || !provider_) return false;

    QImage full = provider_->capturedImage();
    if (full.isNull()) {
        FinishSession();
        return false;
    }
    const int ref_w = overlay_window_ ? overlay_window_->width() : 0;
    const int ref_h = overlay_window_ ? overlay_window_->height() : 0;
    QImage crop = CropRegion(full, x, y, w, h, ref_w, ref_h, true);

    if (provider_) provider_->clear();
    FinishSession();

    if (crop.isNull()) return false;
    QGuiApplication::clipboard()->setImage(crop);
    return true;
}

void ScreenshotService::CancelSession() {
    FinishSession();
}

void ScreenshotService::EndSession() {
    // 只清标志,不碰窗口。窗口显隐由调用方自己处理。
    session_active_ = false;
    pending_save_active_ = false;
}

void ScreenshotService::FinishSession() {
    if (overlay_window_) overlay_window_->hide();
    if (main_window_) main_window_->show();
    if (provider_) provider_->clear();
    session_active_ = false;
    pending_save_active_ = false;  // 连带清掉保存暂存
}

// ===========================================================================
// 保存暂存会话:跨 QML FileDialog 异步间隙记住选区坐标
// ===========================================================================

void ScreenshotService::BeginSaveSession(int x, int y, int w, int h) {
    pending_x_ = x;
    pending_y_ = y;
    pending_w_ = w;
    pending_h_ = h;
    pending_save_active_ = true;
}

bool ScreenshotService::CompleteSaveWithPending(const QString& file_path) {
    if (!pending_save_active_) return false;
    const int x = pending_x_, y = pending_y_, w = pending_w_, h = pending_h_;
    pending_save_active_ = false;

    // 复用 CompleteSave 的核心逻辑
    return CompleteSave(x, y, w, h, file_path);
}

}  // namespace re
