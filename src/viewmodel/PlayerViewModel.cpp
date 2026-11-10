#include "PlayerViewModel.h"

namespace re {

PlayerViewModel::PlayerViewModel(QObject* parent)
    : QObject(parent) {
    decoder_ = new PlaybackDecoder(this);

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &PlayerViewModel::onTimer);
    // 默认帧间隔 = 1000/fps,后续按 fps_ 动态设置。
}

PlayerViewModel::~PlayerViewModel() {
    stop();
}

void PlayerViewModel::open(const QString& path) {
    stop();
    if (!decoder_->open(path)) {
        emit errorOccurred(QStringLiteral("无法打开视频文件: %1").arg(path));
        return;
    }

    source_ = path;

    // 更新视频信息
    video_w_ = decoder_->videoWidth();
    video_h_ = decoder_->videoHeight();
    duration_ms_ = decoder_->durationMs();
    position_ms_ = 0;
    emit videoInfoChanged();
    emit durationChanged();
    emit positionChanged();

    // 启动解码线程
    decoder_->start();

    // 尝试取第一帧显示(非阻塞:解码线程刚启动可能还没产出帧)
    DecodedFrame first = decoder_->tryTakeFrame();
    if (!first.image.isNull()) {
        current_frame_ = first.image;
        position_ms_ = first.pts_ms;
        emit positionChanged();
        emit frameReady();
    }
}

void PlayerViewModel::play() {
    if (source_.isEmpty()) return;
    if (finished_) {
        // 已自然播完后再次播放:从头重播(open 内部经 stop 复位 finished_)
        open(source_);
    }
    if (!decoder_->isRunning()) {
        // 线程已退出且不在"已播完重播"分支:
        //   - stop() 后播放(isEof=false)→ 重新打开;
        //   - EOF 时恰好处于暂停(isEof=true)→ 不 reopen:队列余量帧继续
        //     消费,由 onTimer 耗尽后走 EOF 分支正常结束(修复"末尾暂停再
        //     恢复会从头重播")。
        if (!decoder_->isEof()) open(source_);
    }
    // 设置音频倍速:1x 直通,>1 用 atempo 变速不变调
    // (速度未变化时内部为 no-op,不会停掉音频输出)
    decoder_->setAudioSpeed(speed_);
    // 恢复音频输出:pause() 路径停过音频;首次播放时为无害 no-op
    decoder_->resumeAudio();
    playing_ = true;
    emit playingChanged();
    // 启动墙钟播放时钟:start_pts_ 取当前 position(首帧时为 0,暂停后续播时为上次位置)
    // 音频可用时 onTimer 用音频时钟驱动视频帧;无音频时用墙钟估算
    start_pts_ = position_ms_;
    playback_clock_.start();
    clock_running_ = true;
    last_reported_ms_ = position_ms_;
    // 定时器间隔 = 帧间隔 / 倍速(用于驱动 onTimer 检查时钟)
    double fps = decoder_->fps();
    if (fps <= 0) fps = 30.0;
    int interval = static_cast<int>(1000.0 / fps / speed_);
    timer_->start(qMax(1, interval));
}

void PlayerViewModel::pause() {
    playing_ = false;
    clock_running_ = false;
    emit playingChanged();
    timer_->stop();
    // 暂停音频输出(保留已播样本计数,恢复时无缝续播);
    // 不要用 setAudioSpeed(1):速度相同时它是 no-op,
    // 且旧实现会误杀音频输出导致恢复后时钟冻结
    decoder_->pauseAudio();
}

void PlayerViewModel::stop() {
    playing_ = false;
    clock_running_ = false;
    finished_ = false;  // 手动停止:清除"已播完"标记
    timer_->stop();
    pending_frame_ = {};
    if (decoder_) {
        // 先停解码线程(内部联动停音频输出,解锁 write 等待)再 wait,
        // 避免音频继续播放而画面静止
        decoder_->stop();
        if (decoder_->isRunning()) decoder_->wait();
    }
    position_ms_ = 0;
    emit playingChanged();
    emit positionChanged();
}

void PlayerViewModel::setPlaying(bool p) {
    if (p) play();
    else pause();
}

void PlayerViewModel::setSpeed(int s) {
    if (s == speed_) return;
    speed_ = s;
    emit speedChanged();
    // 通知解码线程重建 atempo filter graph(1x 清空,>1 串联 atempo)
    // setAudioSpeed 内部完成时钟 rebase + prev_audio_speed_ 保存 +
    // beginSpeedChange(不清缓冲),靠旧数据边界无缝区分旧/新采样,
    // QAudioOutput 全程未中断,音频时钟不冻结
    if (playing_) decoder_->setAudioSpeed(speed_);
    // 倍速变化时重置墙钟基准,避免播放位置跳跃
    if (clock_running_) {
        start_pts_ = position_ms_;
        playback_clock_.restart();
    }
    if (playing_ && timer_->isActive()) {
        double fps = decoder_->fps();
        if (fps <= 0) fps = 30.0;
        int interval = static_cast<int>(1000.0 / fps / speed_);
        timer_->setInterval(qMax(1, interval));
    }
}

void PlayerViewModel::onTimer() {
    if (!clock_running_) return;

    // 目标 PTS:音频时钟可用时以它为权威(已按倍速换算到原始终轴);
    // 音频未启动或无音频时用墙钟估算。
    qint64 target_pts;
    qint64 audio_pts = -1;
    if (decoder_->hasAudio()) {
        audio_pts = decoder_->audioClockMs();
    }
    if (audio_pts >= 0) {
        // 音频时钟可用:取较大值防止抖动/回退
        target_pts = qMax(audio_pts, position_ms_);
    } else {
        // 无音频或音频尚未启动:墙钟估算
        target_pts = start_pts_ +
            static_cast<qint64>(playback_clock_.elapsed()) * speed_;
    }

    bool got_frame = false;

    // 1. 先检查 pending_frame_(上次未到时间的帧)
    //    如果它还没到时间,就不碰队列(队列里的帧 PTS 更高,更没到时间)。
    if (!pending_frame_.image.isNull()) {
        if (pending_frame_.pts_ms <= target_pts) {
            current_frame_ = pending_frame_.image;
            position_ms_ = pending_frame_.pts_ms;
            got_frame = true;
            pending_frame_ = {};
        } else {
            // pending 还没到时间,等下次 tick
            // 节流 positionChanged(即使没新帧也报告当前位置)
            if (position_ms_ - last_reported_ms_ >= 200) {
                last_reported_ms_ = position_ms_;
                emit positionChanged();
            }
            return;
        }
    }

    // 2. pending 被消费后(或原本没有 pending),从队列取帧
    while (pending_frame_.image.isNull() && decoder_->hasFrame()) {
        DecodedFrame f = decoder_->tryTakeFrame();
        if (f.image.isNull()) break;
        if (f.pts_ms <= target_pts) {
            current_frame_ = f.image;
            position_ms_ = f.pts_ms;
            got_frame = true;
        } else {
            // 帧还没到时间,缓存到 pending,下次 tick 优先处理
            pending_frame_ = f;
        }
    }

    // 3. 检查 EOF:队列空且解码线程已结束且无 pending
    if (!decoder_->isRunning() && !decoder_->hasFrame() &&
        pending_frame_.image.isNull()) {
        timer_->stop();
        playing_ = false;
        clock_running_ = false;
        finished_ = true;  // 自然播完:再次 play 时从头重播
        emit playingChanged();
        position_ms_ = duration_ms_;
        emit positionChanged();
        emit finished();
        return;
    }

    // 4. 节流 positionChanged:只在跨 200ms 时 emit,避免 burst 信号导致 QML 跳秒
    if (got_frame) {
        if (position_ms_ - last_reported_ms_ >= 200 ||
            position_ms_ >= duration_ms_) {
            last_reported_ms_ = position_ms_;
            emit positionChanged();
        }
        // 通知 View 有新帧可绘制
        emit frameReady();
    }
}

} // namespace re
