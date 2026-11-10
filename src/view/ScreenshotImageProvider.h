#ifndef RECORD_EXPERT_SCREENSHOT_IMAGE_PROVIDER_H
#define RECORD_EXPERT_SCREENSHOT_IMAGE_PROVIDER_H

#include <QImage>
#include <QQuickImageProvider>

namespace re {

// 把"被俘的整屏 QImage"暴露给 QML 的 Image 元素：
//   Image { source: "image://screenshot/desktop" }
// RecorderViewModel 在 startRegionScreenshot() 里抓整屏 → 转 QImage →
// setCapturedImage() 存入此 provider；QML 的 RegionCaptureWindow 用
// image://screenshot/desktop 把这张图作为背景显示，并在其上画选区遮罩。
//
// 本应用一次只服务一张被俘图，requestImage 忽略 id 直接返回内部 QImage。
class ScreenshotImageProvider : public QQuickImageProvider {
public:
    ScreenshotImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    // QQuickImageProvider 接口：QML Image 加载时由引擎调用。
    // 返回 captured_ 的副本（QML 端拿到的 QImage 由引擎管理，互不影响）。
    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requestedSize) override;

    // 由 RecorderViewModel 在抓帧后调用，把整屏 QImage 灌入。
    void setCapturedImage(const QImage& img);

    // 取出当前持有的整屏 QImage（RecorderViewModel 保存选区时调用，
    // 用于 .copy(rect) 裁剪）。
    QImage capturedImage() const { return captured_; }

    // 清空持有的图像（取消截图或退出选区窗口后调用）。
    void clear();

private:
    QImage captured_;
};

}  // namespace re

#endif  // RECORD_EXPERT_SCREENSHOT_IMAGE_PROVIDER_H
