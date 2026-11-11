#ifndef RECORD_EXPERT_MP4_WRITER_H
#define RECORD_EXPERT_MP4_WRITER_H

#include <cstdint>
#include <vector>

// 前置声明 FFmpeg 类型，使调用方无需包含 FFmpeg 头文件。
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVPacket;

namespace re {

// Mp4Writer 在方案 C（"双生产者 + 单消费者"三线程架构）下，只被唯一的
// muxer_worker 线程调用——producer 线程完全不触碰它。因此内部不需要锁。
//
// 生命周期：
//   OpenMuxer(path, {enc_v, enc_a?})   // Alloc + NewStream × N + WriteHeader
//   [muxer 主循环: WritePacket(stream, offset, pkt)] × 多段
//   RotateMuxer(next_path, {enc_v, enc_a?})  // 旧 trailer → 新 Alloc + 新 NewStream × N + WriteHeader
//   ...
//   Close()  // trailer + avio_close + free_context
//
// 关键设计：
//   * encoder 全程一个（由 producer 持有），segment 边界不重建
//   * stream 在每次 RotateMuxer 后重建（因为 out_fmt_ctx_ 换了，旧 stream 指针失效）
//   * pts_offset 由 muxer_worker 手动传给 WritePacket，Mp4Writer 自己不知道 segment
//   * producer 只负责 avcodec_send_frame + receive_packet + 已 rescale 后的 packet 进 queue
class Mp4Writer {
public:
    Mp4Writer() = default;
    ~Mp4Writer();

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // ---- 一次性初始化 ----

    // 为 `output_path` 分配输出上下文，为每个编码器创建一个 AVStream，最后写文件头。
    // encs 的顺序决定 stream 的 index（0=第一个，1=第二个 …）。
    // 成功时，新 stream 指针按 encs 顺序存入 out_streams_out。
    // 失败返回 false。NeedsGlobalHeader() 已自动在调用此函数时处理。
    bool OpenMuxer(const char* output_path,
                   const std::vector<AVCodecContext*>& encs,
                   std::vector<AVStream*>& out_streams_out);

    // muxer_worker 初始化时判断输出格式是否要求全局头（编码器需已设置 GLOBAL_HEADER）。
    bool NeedsGlobalHeader() const;

    // ---- 写入（producer 已 rescale 好 stream time_base，WritePacket 只做 pts 减 offset）----

    // 把 pkt 写入 stream：pkt.pts -= pts_offset，pkt.dts -= pts_offset。
    // pkt 必须已经: rescale 到 stream->time_base（producer 负责）、stream_index 正确设置。
    // pts_offset=0 时 pkt 原样写入（不分段模式或新 segment 的 offset 初始值）。
    // 返回 av_interleaved_write_frame 的返回值，<0 表示致命错误。
    int WritePacket(AVStream* stream, int64_t pts_offset, AVPacket* pkt);

    // ---- 分段切换 ----

    // 收尾当前文件（写 trailer + avio_close + free_context），为新路径 Alloc +
    // 为相同编码器重新 NewStream × N + WriteHeader。
    // 新 stream 指针按 encs 顺序存入 out_streams_out（供 producer 重置 stream_ 指针）。
    bool RotateMuxer(const char* next_path,
                     const std::vector<AVCodecContext*>& encs,
                     std::vector<AVStream*>& out_streams_out);

    // ---- 收尾 ----

    // 写 trailer（仅一次）+ 关闭 IO + 释放输出上下文。幂等。
    void Close();

private:
    AVFormatContext* out_fmt_ctx_ = nullptr;
    bool             header_written_ = false;
};

}  // namespace re

#endif  // RECORD_EXPERT_MP4_WRITER_H
