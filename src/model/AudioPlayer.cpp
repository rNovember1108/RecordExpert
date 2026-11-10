#include "AudioPlayer.h"

#include <QCoreApplication>
#include <QThread>
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#include <QMetaObject>
#include <QDebug>
#include <cstring>

namespace re {

// ---------------------------------------------------------------------------
// AudioPullDevice:QAudioOutput pull 模式的数据源。
// readData() 在 QAudioOutput 的音频线程中被回调(不经过事件循环,
// 纯虚函数调用),直接转调 owner 的 pullRead。不用 Q_OBJECT:
// 不需要信号槽,避免 AUTOMOC 与嵌套类问题。
// ---------------------------------------------------------------------------
class AudioPullDevice : public QIODevice {
public:
    explicit AudioPullDevice(AudioPlayer* owner)
        : QIODevice(), owner_(owner) {}

    qint64 readData(char* data, qint64 maxlen) override {
        return owner_->pullRead(data, maxlen);
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    bool isSequential() const override { return true; }
    // 顺序设备不可 seek:显式覆写为静默失败,避免基类每次打印
    // "Cannot call seek on a sequential device" 告警(waveout 后端偶发调用)。
    bool seek(qint64) override { return false; }

private:
    AudioPlayer* owner_;
};

AudioPlayer::~AudioPlayer() {
    close();
}

bool AudioPlayer::open(int sample_rate, int channels) {
    close();

    channels_    = channels;
    {
        QMutexLocker lk(&mutex_);
        ring_.assign(kRingBytes, 0);
        rd_ = wr_ = 0;
        old_data_end_ = -1;
        input_samples_total_ = 0;
        stop_ = 0;
        paused_ = 0;
        speed_changing_ = 0;
    }

    // S16LE interleaved PCM 格式
    QAudioFormat fmt;
    fmt.setSampleRate(sample_rate);
    fmt.setChannelCount(channels);
    fmt.setSampleSize(16);
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setByteOrder(QAudioFormat::LittleEndian);
    fmt.setSampleType(QAudioFormat::SignedInt);

    // 无输出设备或格式不支持 → false,调用方降级为静音播放
    if (!QAudioDeviceInfo::defaultOutputDevice().isFormatSupported(fmt)) {
        qWarning() << "AudioPlayer: format not supported"
                   << sample_rate << "Hz" << channels << "ch";
        return false;
    }

    // QAudioOutput 是 QObject,只能在主线程创建/使用。open 由解码线程
    // 调用,此处搬运到主线程(BlockingQueuedConnection):此刻主线程在
    // 事件循环中(VideoPlayerItem::open 启动解码线程后立即返回),不会挂死。
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        return openOnMainThread(fmt);

    bool ok = false;
    QMetaObject::invokeMethod(QCoreApplication::instance(),
        [&]() { ok = openOnMainThread(fmt); },
        Qt::BlockingQueuedConnection);
    return ok;
}

bool AudioPlayer::openOnMainThread(const QAudioFormat& fmt) {
    device_ = new AudioPullDevice(this);
    // pull 设备必须先以 ReadOnly 打开(start(QIODevice*) 的前置条件);
    // Unbuffered 让 readData 直接拿到 maxlen,避免 Qt 再做一层缓冲。
    if (!device_->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        delete device_;
        device_ = nullptr;
        return false;
    }
    qaudio_ = new QAudioOutput(fmt);
    // 内部设备缓冲 ~40ms@48k/stereo:pullRead 的累计字节(时钟)会超前
    // 实际出声一个设备缓冲,缓冲越小,音画同步误差越小。
    qaudio_->setBufferSize(8192);
    qaudio_->start(device_);
    if (qaudio_->error() != QAudio::NoError) {
        qWarning() << "AudioPlayer: QAudioOutput start error"
                   << qaudio_->error();
        delete qaudio_;
        qaudio_ = nullptr;
        delete device_;
        device_ = nullptr;
        return false;
    }
    return true;
}

void AudioPlayer::close() {
    {
        QMutexLocker lk(&mutex_);
        stop_ = 1;
        paused_ = 0;
        cond_.wakeAll();  // 解锁可能阻塞在 write 空间等待的解码线程
    }
    if (qaudio_ || device_) {
        // QAudioOutput/AudioPullDevice 都属于主线程,搬运销毁。
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            closeOnMainThread();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(),
                [this]() { closeOnMainThread(); },
                Qt::BlockingQueuedConnection);
        }
    }
    QMutexLocker lk(&mutex_);
    ring_.clear();
    channels_    = 0;
    rd_ = wr_ = 0;
    old_data_end_ = -1;
    input_samples_total_ = 0;
}

void AudioPlayer::closeOnMainThread() {
    if (qaudio_) {
        qaudio_->stop();
        delete qaudio_;
        qaudio_ = nullptr;
    }
    if (device_) {
        device_->close();
        delete device_;
        device_ = nullptr;
    }
}

void AudioPlayer::stop() {
    {
        QMutexLocker lk(&mutex_);
        stop_ = 1;
        paused_ = 0;
        rd_ = wr_;  // 清缓冲
        old_data_end_ = -1;  // 清缓冲后旧数据边界无效
        cond_.wakeAll();
    }
    if (qaudio_)
        QMetaObject::invokeMethod(qaudio_, [this]() { qaudio_->suspend(); },
                                  Qt::AutoConnection);
}

void AudioPlayer::pause() {
    {
        QMutexLocker lk(&mutex_);
        paused_ = 1;
        // 不清缓冲:保留旧数据,resume 后 QAudioOutput 立即消费,
        // 音频时钟无缝继续,避免空窗期卡顿
        cond_.wakeAll();  // 解锁 write 缓冲满等待
    }
    if (qaudio_)
        QMetaObject::invokeMethod(qaudio_, [this]() { qaudio_->suspend(); },
                                  Qt::AutoConnection);
}

void AudioPlayer::resume() {
    if (!qaudio_) return;
    // 只清暂停标志:pause 不设 stop_,故不清 stop_。
    // 缓冲中保留的旧数据被 QAudioOutput 立即消费,音频时钟从暂停处无缝继续(无空窗期)。
    paused_ = 0;
    QMetaObject::invokeMethod(qaudio_, [this]() { qaudio_->resume(); },
                              Qt::AutoConnection);
    cond_.wakeAll();  // 唤醒 write 空间等待者
}

void AudioPlayer::beginSpeedChange() {
    // 置速度切换标志 + 记录旧数据边界(不清缓冲):
    // write() 立即返回 false 解锁解码线程,QAudioOutput 继续消费缓冲中
    // 的旧数据。pullRead 消费到 old_data_end_ 前用旧速率计时钟,
    // 之后用新速率,无需清缓冲即可精确计算时钟。
    // 注意:input_samples_total_ 的清零不在这里做——setAudioSpeed 已用
    // takeInputSamples() 在与读取同一原子点完成清零;此处若再清零,
    // 会丢掉 take 到 begin 之间解码线程新累加的样本。
    speed_changing_ = 1;
    {
        QMutexLocker lk(&mutex_);
        old_data_end_ = wr_;     // 旧数据边界 = 当前写入位置
        cond_.wakeAll();
    }
}

void AudioPlayer::endSpeedChange() {
    speed_changing_ = 0;
}

bool AudioPlayer::write(const uint8_t* data, int bytes) {
    if (!qaudio_ || stop_.load()) return false;
    if (paused_.load()) return false;   // 暂停:拒绝写入(数据丢弃,但避免缓冲溢出)
    if (speed_changing_.load()) return false;  // 速度切换中:拒绝写入
    if (bytes <= 0) return true;

    int written = 0;
    while (written < bytes) {
        QMutexLocker lk(&mutex_);
        while (wr_ - rd_ >= static_cast<qint64>(kRingBytes)) {
            if (stop_.load()) return false;   // 停止:解锁解码线程
            cond_.wait(&mutex_);              // 等 pullRead 消费
        }
        if (stop_.load()) return false;

        // 拷贝当前可用空间(环形,可能绕回)
        qint64 avail = static_cast<qint64>(kRingBytes) - (wr_ - rd_);
        int chunk = static_cast<int>(qMin<qint64>(avail, bytes - written));
        qint64 pos = wr_ % kRingBytes;
        qint64 first = qMin<qint64>(chunk, kRingBytes - pos);
        std::memcpy(ring_.data() + pos, data + written, first);
        if (chunk > first)
            std::memcpy(ring_.data(), data + written + first, chunk - first);
        wr_ += chunk;
        written += chunk;
        cond_.wakeAll();
    }
    return true;
}

qint64 AudioPlayer::pullRead(char* data, qint64 maxlen) {
    QMutexLocker lk(&mutex_);
    if (stop_.load()) return 0;   // stop:持续返回 0 → QAudioOutput 静音
    qint64 used = wr_ - rd_;
    if (used <= 0) {
        // 缓冲空且非 stop:返回静音而非 0,避免 QAudioOutput underrun →
        // IdleState(Qt 5.12 waveout 后端无法从 IdleState 恢复,拉取永久停止)。
        // 高倍速时多级 atempo 串联有启动延迟,首帧产出前缓冲持续空,
        // 若返回 0 必然 underrun → 音频时钟冻结 → 画面卡死。
        // 静音不累加输入样本:时钟冻结,等解码线程填入真实数据后恢复。
        if (maxlen > 0)
            std::memset(data, 0, static_cast<size_t>(maxlen));
        return maxlen;
    }

    qint64 n = qMin(used, maxlen);
    qint64 pos = rd_ % kRingBytes;
    qint64 first = qMin(n, static_cast<qint64>(kRingBytes) - pos);
    std::memcpy(data, ring_.data() + pos, first);
    if (n > first)
        std::memcpy(data + first, ring_.data(), n - first);
    rd_ += n;

    // 旧数据边界消费完后清除；ringPendingSamples 只需该边界划分未消费样本。
    if (old_data_end_ >= 0 && rd_ >= old_data_end_)
        old_data_end_ = -1;

    cond_.wakeAll();              // 唤醒 write 空间等待者
    return n;
}

void AudioPlayer::addInputSamples(qint64 samples) {
    QMutexLocker lk(&mutex_);
    input_samples_total_ += samples;
}

qint64 AudioPlayer::inputSamplesTotal() const {
    QMutexLocker lk(&mutex_);
    return input_samples_total_;
}

qint64 AudioPlayer::takeInputSamples() {
    QMutexLocker lk(&mutex_);
    qint64 v = input_samples_total_;
    input_samples_total_ = 0;
    return v;
}

// 环形缓冲中未消费的样本数(按旧/新数据分别返回)。
// 用于 audioClockMsDouble 计算:decoded_position - ring_pending = played_position。
// decoded_position = audio_start_pts + input_samples_total * 1000 / rate
// (input_samples 是 atempo 前的解码样本,精确等于内容时间)。
// played_position = decoded_position - ring_pending(按速率换算到原始终轴)。
// 误差来自 ring_pending 的换算,但 ring 缓冲容量固定(~170ms),误差有界不累积。
void AudioPlayer::ringPendingSamples(qint64& old_samples, qint64& new_samples) const {
    QMutexLocker lk(&mutex_);
    if (channels_ <= 0) {
        old_samples = 0;
        new_samples = 0;
        return;
    }
    int divisor = channels_ * 2;
    qint64 pending_bytes = wr_ - rd_;  // 缓冲中未消费字节总数
    if (pending_bytes <= 0) {
        old_samples = 0;
        new_samples = 0;
        return;
    }
    if (old_data_end_ >= 0) {
        // 有旧数据边界:未消费部分按边界拆分
        // 旧数据未消费 = old_data_end_ - rd_(若 rd_ 已超过边界则为 0)
        qint64 old_pending = (old_data_end_ > rd_) ? (old_data_end_ - rd_) : 0;
        if (old_pending > pending_bytes) old_pending = pending_bytes;
        qint64 new_pending = pending_bytes - old_pending;
        old_samples = old_pending / divisor;
        new_samples = new_pending / divisor;
    } else {
        // 无边界:全按新数据
        old_samples = 0;
        new_samples = pending_bytes / divisor;
    }
}

} // namespace re
