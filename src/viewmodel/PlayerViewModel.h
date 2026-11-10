#ifndef RECORD_EXPERT_PLAYER_VIEW_MODEL_H
#define RECORD_EXPERT_PLAYER_VIEW_MODEL_H

#include <QObject>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>

#include "PlaybackDecoder.h"

namespace re {

// PlayerViewModel: 播放器 ViewModel。
// 职责: 播放状态机 + 播放时钟 + 帧调度 + 持有 PlaybackDecoder(Model)。
// 不依赖 View 层(QQuickItem 等),可独立单测。
// View (VideoPlayerItem) 通过 currentFrame() 取当前帧绘制,通过 frameReady 信号触发 update()。
class PlayerViewModel : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoInfoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoInfoChanged)

public:
    explicit PlayerViewModel(QObject* parent = nullptr);
    ~PlayerViewModel() override;

    Q_INVOKABLE void open(const QString& path);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();

    bool    playing()    const { return playing_; }
    int     speed()      const { return speed_; }
    qint64  duration()   const { return duration_ms_; }
    qint64  position()   const { return position_ms_; }
    int     videoWidth() const { return video_w_; }
    int     videoHeight()const { return video_h_; }
    // 当前应显示的帧(View 的 paint() 读取)
    QImage  currentFrame() const { return current_frame_; }

    void setPlaying(bool p);
    void setSpeed(int s);

signals:
    void playingChanged();
    void speedChanged();
    void durationChanged();
    void positionChanged();
    void videoInfoChanged();
    // 新帧已就绪:View 收到此信号后应调 update() 触发重绘
    void frameReady();
    void finished();
    void errorOccurred(const QString& msg);

private slots:
    void onTimer();

private:
    PlaybackDecoder* decoder_ = nullptr;
    QTimer*       timer_ = nullptr;
    QImage        current_frame_;
    QString       source_;              // 当前打开的源路径,用于重新打开
    bool          playing_ = false;
    int           speed_ = 1;
    qint64        duration_ms_ = 0;
    qint64        position_ms_ = 0;
    int           video_w_ = 0, video_h_ = 0;

    // 墙钟播放时钟:play 启动,pause 暂停,setSpeed 重置
    QElapsedTimer playback_clock_;
    qint64        start_pts_ = 0;
    bool          clock_running_ = false;
    // 自然播完(EOF)标记:play() 时从头重播;stop() 清除
    bool          finished_ = false;
    // 上次 emit positionChanged 时的 position,用于节流
    qint64        last_reported_ms_ = -1;
    // 上次取出但还没到时间的帧:下次 tick 优先检查
    DecodedFrame  pending_frame_;
};

} // namespace re

#endif // RECORD_EXPERT_PLAYER_VIEW_MODEL_H
