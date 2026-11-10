#ifndef RECORD_EXPERT_AUDIO_PLAYER_H
#define RECORD_EXPERT_AUDIO_PLAYER_H

#include <QtGlobal>
#include <QMutex>
#include <QWaitCondition>
#include <QAtomicInt>
#include <vector>

// Qt 类前向声明(全局命名空间)
class QAudioOutput;
class QAudioFormat;

namespace re {

// 前向声明:QAudioOutput/拉取设备都只在 .cpp 内使用,
// 头文件不引入 QtMultimedia 头,避免污染 VideoPlayerItem.h 编译环境。
class AudioPullDevice;

// 基于 QtMultimedia QAudioOutput 的音频输出封装(pull 模式)。
// 接收 S16LE 16bit interleaved PCM 数据,通过 QAudioOutput 播放。
// 提供“解码输入样本数 + 环形缓冲待消费样本数”用于音画同步。
//
// 线程模型(QAudioOutput 的硬约束:QObject 只能在主线程使用):
//   - QAudioOutput 在主线程创建/销毁(open/close 内部用
//     BlockingQueuedConnection 搬运,与 GUI 挂死路径无交集);
//   - 解码线程只写环形缓冲(write),主线程/解码线程都可通过
//     置位标志的方式 stop/resume(纯原子操作);
//   - QAudioOutput 音频线程回调 PullDevice::readData 消费环形缓冲,
//     与解码线程之间仅通过 mutex + cond 同步,无跨线程 QObject 调用。
class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    // 打开输出设备。sample_rate 典型 48000, channels 典型 2。
    // 失败返回 false(例如无音频设备);调用方应降级为静音播放。
    bool open(int sample_rate, int channels);
    void close();
    void stop();    // 停止播放+清缓冲(用于退出/切换文件)
    // 暂停播放(不清缓冲):保留旧数据供 resume 后无缝续播,
    // 避免清缓冲导致的空窗期卡顿
    void pause();
    // 恢复输出:清除暂停标志,QAudioOutput 恢复拉取,缓冲中旧数据
    // 立即被消费,音频时钟从暂停处无缝继续
    void resume();

    // 速度切换预处理:暂停 write()(解码线程立即解锁),但不清缓冲、
    // 不挂起 QAudioOutput。QAudioOutput 继续拉取旧数据,避免 underrun。
    // 速度切换完成后调 endSpeedChange() 恢复写入。
    void beginSpeedChange();
    void endSpeedChange();

    // 写入 PCM 数据(S16LE interleaved)。会阻塞直到环形缓冲有空间。
    // 返回 false 表示已 stop 或设备未打开。
    bool write(const uint8_t* data, int bytes);

    // 累加输入样本数(atempo 前的解码样本)。用于精确计算内容时间:
    // input_samples 不受 atempo 比例偏差影响,比 output_played*speed 更精确。
    void addInputSamples(qint64 samples);
    qint64 inputSamplesTotal() const;
    // 原子读取并清零输入样本计数(倍速切换 rebase 用):读与清零在同一把
    // 锁内完成,避免两者之间解码线程 addInputSamples 落入的样本既不进
    // rebase 也不进新轮次计数而丢失(每次切换约 1024 样本 ≈ 21ms)。
    qint64 takeInputSamples();
    // 环形缓冲中未消费的样本数(按旧/新数据分别返回,单次加锁)。
    // 用于从 decoded_position 减去未消费部分得到 played_position。
    void ringPendingSamples(qint64& old_samples, qint64& new_samples) const;

    // pullRead 供 AudioPullDevice::readData 转调(QAudioOutput 音频线程)。
    // 返回实际读出的字节数;0 表示无数据(stop 或缓冲空,underrun)。
    qint64 pullRead(char* data, qint64 maxlen);

private:
    // 在主线程创建/启动 QAudioOutput(open 从解码线程调用时搬运)
    bool openOnMainThread(const QAudioFormat& fmt);
    // 在主线程停止/销毁 QAudioOutput(close 同上)
    void closeOnMainThread();

    int channels_    = 0;

    // 环形缓冲(字节):write 侧(解码线程)写入,pullRead 侧(Qt 音频线程)
    // 消费。用累计位置 rd_/wr_ 表达,容量取 2 的幂。
    static constexpr int kRingBytes = 1 << 15;  // 32KB ≈ 170ms@48k/stereo
    std::vector<uint8_t> ring_;
    qint64 rd_ = 0;                 // 已消费位置(累计字节)
    qint64 wr_ = 0;                 // 已写入位置(累计字节)
    qint64 old_data_end_ = -1;       // 旧数据边界(累计 wr_ 快照),-1=无边界
    qint64 input_samples_total_ = 0;  // 累计输入样本数(atempo 前的解码样本,精确内容时间)

    mutable QMutex      mutex_;
    QWaitCondition      cond_;
    QAtomicInt          stop_{0};
    QAtomicInt          paused_{0};          // 暂停(不清缓冲):write() 立即返回 false
    QAtomicInt          speed_changing_{0};  // 速度切换中:write() 立即返回 false

    QAudioOutput*    qaudio_ = nullptr;  // 主线程创建/销毁
    AudioPullDevice* device_ = nullptr;  // 主线程创建/销毁
};

} // namespace re

#endif // RECORD_EXPERT_AUDIO_PLAYER_H
