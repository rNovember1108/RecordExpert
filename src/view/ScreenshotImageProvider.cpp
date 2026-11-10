#include "ScreenshotImageProvider.h"

namespace re {

QImage ScreenshotImageProvider::requestImage(const QString& id, QSize* size,
                                              const QSize& requestedSize) {
    // 忽略 id（本 provider 一次只持有一张被俘图）与 requestedSize
    // （RegionCaptureWindow 用 anchors.fill: parent 自动按窗口尺寸缩放，
    // 不需要 provider 再做缩放）。返回 captured_ 的浅拷贝引用计数副本，
    // 让 QML 端使用期间即便 setCapturedImage/clear 被调用也不受影响。
    QImage out = captured_;
    if (size) *size = out.size();
    return out;
}

void ScreenshotImageProvider::setCapturedImage(const QImage& img) {
    captured_ = img;
}

void ScreenshotImageProvider::clear() {
    captured_ = QImage();
}

}  // namespace re
