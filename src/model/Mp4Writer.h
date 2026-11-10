#ifndef RECORD_EXPERT_MP4_WRITER_H
#define RECORD_EXPERT_MP4_WRITER_H

#include <mutex>

// 前置声明 FFmpeg 类型，使调用方无需包含 FFmpeg 头文件。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;

namespace re {

// Mp4Writer 持有视频、音频两路共享的唯一一个 MP4 输出上下文，并对所有写入操作
// 串行化。两路各自在自己的线程上编码，但把包交织写入同一个文件，因此对 muxer 的
// 调用必须用互斥量保护。
//
// 该上下文刻意做到"媒体无关"：它只知道如何创建输出流、如何编码并写包，而对
// gdigrab、dshow、缩放、重采样一无所知。这样 video_track / audio_track 彼此不必
// 关心对方，只共享唯一一样东西——输出文件。
class Mp4Writer {
public:
    Mp4Writer() = default;
    ~Mp4Writer();

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // 为 `output_path` 分配输出上下文。失败返回 false。
    bool Alloc(const char* output_path);

    // 按 `enc` 的参数创建一个新的输出流。失败返回 nullptr。需在编码器已打开、且在
    // WriteHeader() 之前调用。
    AVStream* NewStream(AVCodecContext* enc);

    // 打开输出文件（如有需要）并写入容器头。必须在所有流都创建完毕后调用。
    // 失败返回 false。
    bool WriteHeader();

    // 把 `frame`（或传 nullptr 以 flush）送入 `enc`，再把编码得到的包写入 `stream`。
    // muxer 写入由内部互斥量串行化，从而视频与音频线程可以安全共享这一个输出
    // 上下文。发生致命错误返回 false。
    bool EncodeAndWrite(AVCodecContext* enc, AVStream* stream, AVFrame* frame);

    // 写入 trailer（仅一次）并释放输出上下文。幂等。
    void Close();

    // 当输出格式使用全局头（编码器须设置 AV_CODEC_FLAG_GLOBAL_HEADER）时返回 true。
    // 在 Alloc() 之后有效。
    bool NeedsGlobalHeader() const;

private:
    AVFormatContext* out_fmt_ctx_ = nullptr;
    std::mutex       write_mutex_;   // 串行化 av_interleaved_write_frame。
    bool             header_written_ = false;
};

}  // namespace re

#endif  // RECORD_EXPERT_MP4_WRITER_H
