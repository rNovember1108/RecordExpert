# 用 libavfilter 实现水印方案

> 历史方案文档（未采用）。最终水印实现为 `Watermark.{h,cpp}` 的 QPainter 像素绘制，详见 `ARCHITECTURE.md`。

## Context

用户要求将当前手动像素绘制的水印改为使用 FFmpeg 的 libavfilter 实现。当前项目已包含 libavfilter 库（avfilter.lib / avfilter-9.dll / 头文件），但 CMakeLists 未链接它。目标：用 filter graph 替代当前的 sws_scale + 手动水印，完成格式转换 + drawtext 水印。

## 核心改动

### 1. CMakeLists.txt — 链接 avfilter

在 `target_link_libraries` 中加入 `avfilter`（位于 [CMakeLists.txt:42-49](file:///d:/Work/RecordExpert/CMakeLists.txt#L42-L49)）。

### 2. video_track.h — 替换 sws 成员为 filter graph 成员（历史方案，现状见 ARCHITECTURE.md）

移除 `SwsContext* sws_ctx_` 和 `AVFrame* yuv_frame_`，新增 filter graph 相关成员：

```cpp
// 前置声明新增
struct AVFilterGraph;
struct AVFilterContext;

// 成员替换
AVFilterGraph*   filter_graph_ = nullptr;
AVFilterContext* filter_in_    = nullptr;   // buffer 源
AVFilterContext* filter_out_   = nullptr;   // buffersink
AVFrame*         filt_frame_   = nullptr;   // filter 输出帧
```

移除 `SwsContext` 前置声明，新增 `AVFilterGraph` / `AVFilterContext` 前置声明。

### 3. video_track.cpp — 用 filter graph 替换 InitScaler + 水印调用（历史方案，现状见 ARCHITECTURE.md）

#### 3a. 新增 `InitFilterGraph()` 替代 `InitScaler()`

构建如下 filter chain（一条 graph 串联多个 filter）：

```
buffer（输入：dec_ctx_ 的宽高/格式/帧率/时基）
  → drawtext: text="test", x=10, y=10, fontfile=arial.ttf, fontsize=24, fontcolor=white@0.8:shadowcolor=black:shadowx=1:shadowy=1
  → drawtext: text="%{localtime:%Y-%m-%d %H:%M:%S}", x=w-tw-10, y=h-th-10, fontfile=..., fontsize=24, 同上
  → format=yuv420p
  → buffersink
```

关键细节：
- `buffer` 滤镜参数用 `av_buffersrc_parameters_alloc` + `av_buffersrc_parameters_set` 设置，避免手拼字符串
- `buffersink` 设置 `pix_fmts` 为 YUV420P，替代 sws_scale 的格式转换功能
- 字体路径硬编码 `C:/Windows/Fonts/arial.ttf`（已确认存在）
- `drawtext` 的 `text` 参数用 `%{localtime}` 扩展实现动态时间戳
- 两个 drawtext 滤镜串联在一条 graph 里

#### 3b. Run() 循环改动

当前流程：
```
decode → make_writable → DrawLabelWatermark → DrawTimestampWatermark → sws_scale → PTS → encode
```

新流程：
```
decode → av_buffersrc_add_frame(filter_in, dec_frame)
       → av_buffersink_get_frame(filter_out, filt_frame)   // 输出已含水印的 YUV420P
       → PTS → encode
```

移除 `re::DrawLabelWatermark` / `re::DrawTimestampWatermark` 调用、`av_frame_make_writable(dec_frame)` 和 `sws_scale` 调用。

#### 3c. Close() 改动

替换 `sws_freeContext` 和 `av_frame_free(&yuv_frame_)` 为：
- `av_frame_free(&filt_frame_)`
- `avfilter_graph_free(&filter_graph_)`（会一并释放内部 filter context）

### 4. 保留 watermark.h/watermark.cpp（作为回退）

不删除手动绘制代码，作为 drawtext 不可用时的回退路径。CMakeLists.txt 保持 `src/watermark.cpp`。

## 回退策略（用户确认）

- `InitFilterGraph()` 中先调用 `avfilter_get_by_name("drawtext")` 检测滤镜是否可用
- 如果 drawtext 不存在（返回 null），打印告警并回退到 sws_scale + 手动水印方案（保留 watermark.h/watermark.cpp，不删除）
- 回退时走原有的 InitScaler() 路径，Run() 循环走原有的 make_writable + DrawLabelWatermark + DrawTimestampWatermark + sws_scale 流程
- 因此 video_track.h 需同时保留 sws 和 filter 两套成员，运行时根据 `use_filter_` 标志决定走哪条路径（历史方案，现状见 ARCHITECTURE.md）

## 验证方式

1. `cmake --build build` 编译通过
2. 运行 `record_expert.exe`，用 GUI 手动录制 5 秒（CLI 入口已删除）
3. 检查输出 MP4：左上角有 "test" 文字，右下角有实时时间戳
4. 确认帧率仍稳定 ~30fps（录制结束后的耗时统计）
