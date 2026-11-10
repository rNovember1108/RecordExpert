#ifndef RECORD_EXPERT_VIDEO_PLAYER_ITEM_H
#define RECORD_EXPERT_VIDEO_PLAYER_ITEM_H

#include <QImage>
#include <QMutex>
#include <QQuickItem>

class QSGNode;

namespace re {

class PlayerViewModel;

// VideoPlayerItem: 播放器 View 层(QQuickItem + QSGImageNode)。
// 职责极简:
//   1. updatePaintNode() 把当前帧作为纹理交给场景图(GPU 缩放居中)
//   2. Q_PROPERTY / Q_INVOKABLE / 信号全部透传到 PlayerViewModel
// 播放状态机、时钟、帧调度全在 PlayerViewModel 里,可独立单测。
class VideoPlayerItem : public QQuickItem {
    Q_OBJECT

    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoInfoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoInfoChanged)

public:
    explicit VideoPlayerItem(QQuickItem* parent = nullptr);
    ~VideoPlayerItem() override;

    // 场景图渲染线程:创建/复用 QSGImageNode,把最新帧作为纹理绘制。
    QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;

    Q_INVOKABLE void open(const QString& path);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();

    bool    playing()    const;
    int     speed()      const;
    qint64  duration()   const;
    qint64  position()   const;
    int     videoWidth() const;
    int     videoHeight()const;

    void setPlaying(bool p);
    void setSpeed(int s);

signals:
    void playingChanged();
    void speedChanged();
    void durationChanged();
    void positionChanged();
    void videoInfoChanged();
    void finished();
    void errorOccurred(const QString& msg);

private:
    // GUI 线程收到新帧通知:加锁快照帧,再调度渲染线程重绘。
    void onFrameReady();

    PlayerViewModel* viewModel_ = nullptr;

    // ---- GUI 线程 → 渲染线程的帧交接 ----
    // PlayerViewModel::currentFrame() 在 GUI 线程被写,不能在渲染线程直接读,
    // 故在此加锁快照,由 updatePaintNode 取走。
    QMutex frame_mutex_;
    QImage pending_frame_;      // 待渲染帧(GUI 线程写)
    int    pending_seq_ = 0;    // 待渲染帧序号(GUI 线程递增)

    // ---- 以下成员仅渲染线程(updatePaintNode)访问 ----
    int    rendered_seq_ = 0;   // 已上传纹理的帧序号
    QImage rendered_frame_;     // 最近上传的帧,尺寸变化时用于重算绘制矩形
};

} // namespace re

#endif // RECORD_EXPERT_VIDEO_PLAYER_ITEM_H
