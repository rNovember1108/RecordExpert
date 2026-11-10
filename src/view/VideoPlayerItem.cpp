#include "VideoPlayerItem.h"
#include "PlayerViewModel.h"

#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

namespace re {

VideoPlayerItem::VideoPlayerItem(QQuickItem* parent)
    : QQuickItem(parent) {
    // 没有该标志,场景图不会调用 updatePaintNode。
    setFlag(QQuickItem::ItemHasContents, true);

    viewModel_ = new PlayerViewModel(this);

    // ViewModel 信号 → View 信号转发(QML 通过 VideoPlayerItem.onXxx 接收)
    connect(viewModel_, &PlayerViewModel::playingChanged,
            this, &VideoPlayerItem::playingChanged);
    connect(viewModel_, &PlayerViewModel::speedChanged,
            this, &VideoPlayerItem::speedChanged);
    connect(viewModel_, &PlayerViewModel::durationChanged,
            this, &VideoPlayerItem::durationChanged);
    connect(viewModel_, &PlayerViewModel::positionChanged,
            this, &VideoPlayerItem::positionChanged);
    connect(viewModel_, &PlayerViewModel::videoInfoChanged,
            this, &VideoPlayerItem::videoInfoChanged);
    connect(viewModel_, &PlayerViewModel::finished,
            this, &VideoPlayerItem::finished);
    connect(viewModel_, &PlayerViewModel::errorOccurred,
            this, &VideoPlayerItem::errorOccurred);

    // ViewModel 通知有新帧 → 快照并调度场景图重绘
    connect(viewModel_, &PlayerViewModel::frameReady,
            this, &VideoPlayerItem::onFrameReady);

    // 尺寸变化时重算等比缩放矩形(渲染线程需要重新拿到 updatePaintNode)
    connect(this, &QQuickItem::widthChanged, this, [this]() { update(); });
    connect(this, &QQuickItem::heightChanged, this, [this]() { update(); });
}

VideoPlayerItem::~VideoPlayerItem() = default;

void VideoPlayerItem::onFrameReady() {
    {
        QMutexLocker locker(&frame_mutex_);
        pending_frame_ = viewModel_->currentFrame();
        ++pending_seq_;
    }
    // update() 线程安全,可在任意线程调用。
    update();
}

QSGNode* VideoPlayerItem::updatePaintNode(QSGNode* old_node,
                                      UpdatePaintNodeData* /*data*/) {
    // 取 GUI 线程投递的最新帧(仅拷贝 QImage 句柄,像素隐式共享,无深拷贝)
    QImage frame;
    int seq = 0;
    {
        QMutexLocker locker(&frame_mutex_);
        seq = pending_seq_;
        if (seq != rendered_seq_ && !pending_frame_.isNull()) {
            frame = pending_frame_;
        }
    }

    // 还没有任何可显示的帧:返回空节点,黑色背景由 QML Rectangle 提供
    if (frame.isNull() && rendered_frame_.isNull()) {
        return nullptr;
    }

    QSGImageNode* node = static_cast<QSGImageNode*>(old_node);
    if (!node) {
        node = window()->createImageNode();
        if (!node) return nullptr;
        // 纹理由场景图节点接管:替换纹理或销毁节点时自动释放旧纹理
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
    }

    // Qt 5.12 的 QSGTexture 不支持原地更新像素,每帧新建纹理;
    // ownsTexture=true 保证旧纹理在 setTexture 时被回收。
    if (!frame.isNull()) {
        QSGTexture* tex = window()->createTextureFromImage(
            frame, QQuickWindow::TextureIsOpaque);
        node->setTexture(tex);
        rendered_frame_ = frame;
        rendered_seq_ = seq;
    }

    // 等比缩放 + 居中(GPU 采样完成缩放,item 坐标为逻辑像素,DPR 由场景图换算)
    const QSizeF item_size = size();
    const QSizeF frame_size = rendered_frame_.size();
    qreal w = item_size.width();
    qreal h = item_size.height();
    if (frame_size.width() > 0 && frame_size.height() > 0) {
        const qreal scale = qMin(w / frame_size.width(), h / frame_size.height());
        w = frame_size.width() * scale;
        h = frame_size.height() * scale;
    }
    node->setRect(QRectF((item_size.width() - w) / 2,
                         (item_size.height() - h) / 2, w, h));

    return node;
}

// ---- 命令转发到 ViewModel ----

void VideoPlayerItem::open(const QString& path) { viewModel_->open(path); }
void VideoPlayerItem::play()  { viewModel_->play(); }
void VideoPlayerItem::pause() { viewModel_->pause(); }
void VideoPlayerItem::stop()  { viewModel_->stop(); }

// ---- Q_PROPERTY getter 透传 ----

bool    VideoPlayerItem::playing()    const { return viewModel_->playing(); }
int     VideoPlayerItem::speed()      const { return viewModel_->speed(); }
qint64  VideoPlayerItem::duration()   const { return viewModel_->duration(); }
qint64  VideoPlayerItem::position()   const { return viewModel_->position(); }
int     VideoPlayerItem::videoWidth() const { return viewModel_->videoWidth(); }
int     VideoPlayerItem::videoHeight()const { return viewModel_->videoHeight(); }

void VideoPlayerItem::setPlaying(bool p) { viewModel_->setPlaying(p); }
void VideoPlayerItem::setSpeed(int s)    { viewModel_->setSpeed(s); }

} // namespace re
