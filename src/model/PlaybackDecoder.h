#ifndef RECORD_EXPERT_PLAYBACK_DECODER_H
#define RECORD_EXPERT_PLAYBACK_DECODER_H

#include <QImage>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QAtomicInt>
#include <atomic>

// FFmpeg C 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace re {

class AudioPlayer;  // 前向声明,实现在 AudioPlayer.h

// 解码帧:图片 + PTS(毫秒)
struct DecodedFrame {
    QImage image;
    qint64 pts_ms = 0;
};

// 解码线程:av_read_frame → avcodec_send/receive → sws_scale → RGB24 → 入队
// 纯 Model 层:不依赖 QML/View 层,可独立单测。
class PlaybackDecoder : public QThread {
    Q_OBJECT
public:
    PlaybackDecoder(QObject* parent = nullptr);
    ~PlaybackDecoder() override;

    // 打开视频文件,返回 true 表示成功。
    bool open(const QString& path);
    void close();
    void stop();  // 设置停止标志并唤醒线程

    // 帧队列接口(线程安全)
    DecodedFrame tryTakeFrame(); // 非阻塞取一帧,队空时返回空 image
    bool hasFrame() const;
    // 解码线程是否已自然读完文件(EOF)。用于 play() 区分:
    // EOF 暂停后恢复 → 续播队列余量;stop() 后播放 → 重新打开。
    bool isEof() const { return eof_.load(); }

    // 视频信息
    int videoWidth() const { return width_; }
    int videoHeight() const { return height_; }
    double fps() const { return fps_; }
    qint64 durationMs() const { return duration_ms_; }

    // 音频信息与音画同步时钟
    bool   hasAudio() const { return audio_stream_ >= 0 && audio_player_ != nullptr; }
    // 当前音频播放位置(ms),已按倍速换算到原始时间轴。无音频返回 -1。
    qint64 audioClockMs() const;
    // 双精度版本:rebase 时用,避免整数截断累积
    double audioClockMsDouble() const;
    // 设置音频倍速(1/2/4/8):1x 直通,>1 用 atempo 变速不变调。
    // 速度与当前一致时为 no-op(不会停掉音频输出)。
    void   setAudioSpeed(int speed);
    // 暂停音频输出(保留时钟计数);恢复音频输出(从暂停处无缝续播)
    void   pauseAudio();
    void   resumeAudio();

protected:
    void run() override;

private:
    // 初始化 atempo 音频 filter graph(speed>1 时用),失败返回 false
    bool initAudioFilterGraph(int speed);
    void freeAudioFilterGraph();

    // 视频流:FFmpeg 上下文
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext*  codec_ctx_ = nullptr;
    SwsContext*      sws_ctx_ = nullptr;
    int video_stream_ = -1;
    int width_ = 0, height_ = 0;
    double fps_ = 30.0;
    qint64 duration_ms_ = 0;

    // 音频流:解码器 + 重采样到 S16LE + AudioPlayer(QAudioOutput)输出
    AVCodecContext* audio_codec_ctx_ = nullptr;
    SwrContext*     swr_ctx_ = nullptr;
    AudioPlayer*    audio_player_ = nullptr;  // 解码线程拥有,run() 中写入
    int  audio_stream_      = -1;
    int  audio_sample_rate_ = 0;
    int  audio_channels_   = 0;
    // 跨线程成员(音频时钟):audio_start_pts_ms_ 由解码线程写首帧、GUI 线程
    // 在 setAudioSpeed rebase;32 位下 double 非原子有撕裂风险,故用
    // std::atomic<double>。audio_clock_started_/audio_speed_ 同理跨线程。
    std::atomic<double> audio_start_pts_ms_{-1.0};  // 第一帧音频 PTS(ms)
    QAtomicInt audio_clock_started_{0};  // 是否已捕获首帧音频 PTS(区分未初始化与负 PTS)
    QAtomicInt audio_speed_{1};         // 当前音频倍速(1/2/4)
    int  prev_audio_speed_ = 1;         // 倍速切换前的速度(仅 GUI 线程访问)

    // atempo 音频 filter graph(倍速>1 时用,1x 时为空)
    AVFilterGraph*   afilter_graph_   = nullptr;
    AVFilterContext* abuffersrc_ctx_  = nullptr;
    AVFilterContext* abuffersink_ctx_ = nullptr;

    // 帧队列(线程安全,有界)
    mutable QMutex mutex_;
    QWaitCondition cond_;
    QQueue<DecodedFrame> queue_;
    static constexpr int kMaxQueue = 60;  // 最多缓冲 60 帧

    QAtomicInt stop_{0};
    QAtomicInt eof_{0};
    QAtomicInt paused_{0};  // 暂停标志:解码线程在 paused_=1 时阻塞,恢复后继续
    QAtomicInt pending_audio_speed_{1};  // 待应用的音频倍速:GUI 线程写,解码线程读(BUG3 修复)
};

} // namespace re

#endif // RECORD_EXPERT_PLAYBACK_DECODER_H
