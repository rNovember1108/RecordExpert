cmake_minimum_required(VERSION 3.15)

project(RecordExpert LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Qt + MinGW 工具链配置
# 项目从 MSVC 切换到 MinGW 32-bit，以复用本机已安装的 Qt5.12.12 mingw73_32 套件
# （msvc2017_32 套件未安装）。FFmpeg 32-bit 库是 MSVC 格式 .lib + .def + .dll，
# MinGW ld 通过 -l:<name>.dll 直接链接 DLL 导出表，无需 .a 导入库。
# ---------------------------------------------------------------------------
set(QT_PREFIX "C:/Qt/Qt5.12.12/5.12.12/mingw73_32")
list(APPEND CMAKE_PREFIX_PATH ${QT_PREFIX})

set(CMAKE_AUTOMOC ON)  # 自动处理 Q_OBJECT 的 moc
set(CMAKE_AUTORCC ON)  # 自动处理 .qrc 的 rcc

find_package(Qt5 5.12 COMPONENTS Core Gui Qml Quick Multimedia REQUIRED)

# ---------------------------------------------------------------------------
# 捆绑的 FFmpeg (depends/ffmpeg) 是 32-bit (x86) 构建。MinGW Makefiles + mingw730_32
# 工具链天然产出 32-bit 二进制，无需额外平台校验。
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/depends/ffmpeg")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")

# 把 qml.qrc 编译成可链接的 .cpp 源文件路径。
qt5_add_resources(RES_QRC "${CMAKE_SOURCE_DIR}/qml.qrc")

add_executable(record_expert WIN32
    src/main.cpp
    src/model/ScreenRecorder.cpp
    src/model/Mp4Writer.cpp
    # 采集层：抽象接口（i_*_source）+ 具体后端（gdigrab/dshow）+ 运行时工厂。
    src/model/capture/video_source_factory.cpp
    src/model/capture/audio_source_factory.cpp
    src/model/capture/gdi_grab_video_source.cpp
    src/model/capture/dshow_video_source.cpp
    src/model/capture/dshow_audio_source.cpp
    src/model/capture/composite_video_source.cpp
    src/model/capture/mixed_audio_source.cpp
    src/model/Screenshot.cpp
    src/view/ScreenshotImageProvider.cpp
    src/model/Watermark.cpp
    src/model/AudioPlayer.cpp
    src/model/AudioPlayer.h
    # 播放器三层:PlaybackDecoder(Model 解码) / PlayerViewModel(状态机+时钟+帧调度) / VideoPlayerItem(View 绘制)
    src/model/PlaybackDecoder.h
    src/model/PlaybackDecoder.cpp
    src/viewmodel/PlayerViewModel.h
    src/viewmodel/PlayerViewModel.cpp
    src/view/VideoPlayerItem.cpp
    src/view/VideoPlayerItem.h
    src/viewmodel/RecorderViewModel.h
    src/viewmodel/RecorderViewModel.cpp
    # Service 层:录制生命周期/截图业务/枚举等独立逻辑。
    src/model/service/RecordingService.h
    src/model/service/RecordingService.cpp
    src/model/service/ScreenshotService.h
    src/model/service/ScreenshotService.cpp
    src/model/service/EnumerationService.h
    src/model/service/EnumerationService.cpp
    ${RES_QRC}
)

target_include_directories(record_expert PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/view"
    "${CMAKE_SOURCE_DIR}/src/viewmodel"
    "${CMAKE_SOURCE_DIR}/src/model"
    "${CMAKE_SOURCE_DIR}/src/model/service"
    "${CMAKE_SOURCE_DIR}/src/model/capture"
)

# MinGW ld 通过 -l:<name>.dll 直接链接 DLL，所以指向 bin 目录。
target_link_directories(record_expert PRIVATE "${FFMPEG_BIN_DIR}")

# LARGEADDRESSAWARE:把 32 位进程可用地址空间从 2GB 扩到 4GB。本应用是
# Qt Quick(QML 场景图/D3D 纹理映射)+ FFmpeg 双栈,单帧抓屏还要一次性分配
# 8MB 连续内存(1920x1080x4);32 位默认 2GB 空间被 DLL 加载区切割成碎块后,
# 大块分配会随机失败(gdigrab av_new_packet ENOMEM → 区域截图二次抓帧失败)。
# 对 32 位多媒体应用这是标准选项,对系统其他进程无影响。
target_link_options(record_expert PRIVATE -Wl,--large-address-aware)

target_link_libraries(record_expert PRIVATE
    # FFmpeg：用 -l:<name>.dll 让 MinGW ld 直接吃 DLL 导出表，
    # 绕过 MSVC 格式 .lib 导入库（MinGW ld 不一定能稳定读取 .lib COFF 索引）。
    -l:avformat-60.dll
    -l:avcodec-60.dll
    -l:avdevice-60.dll
    -l:avfilter-9.dll
    -l:avutil-58.dll
    -l:swscale-7.dll
    -l:swresample-4.dll
    Qt5::Multimedia  # QAudioOutput:VideoPlayerItem 音频输出(替代 Win32 WaveOut)
    Qt5::Core
    Qt5::Gui
    Qt5::Qml
    Qt5::Quick
)

# MinGW UTF-8 源码/执行字符集，替代 MSVC 的 /utf-8；NOMINMAX 与
# WIN32_LEAN_AND_MEAN 防 windows.h 宏（min/max）与 Qt 头冲突。
target_compile_options(record_expert PRIVATE
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
    -DNOMINMAX
    -DWIN32_LEAN_AND_MEAN
)

# 拷贝所有 FFmpeg 运行时 DLL 到输出目录（构建后立即从 build 树直接运行）。
add_custom_command(TARGET record_expert POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_BIN_DIR}"
        "$<TARGET_FILE_DIR:record_expert>"
    COMMENT "Copying FFmpeg runtime DLLs to output directory")

# 调用 windeployqt 扫描 qml/ 提取 QML 依赖，并把 Qt 运行时 DLL/插件/平台
# 拷贝到 exe 同目录，确保双击 exe 能加载 Qt + QML。Debug 构建部署调试版
# Qt DLL（Qt5Cored.dll 等），Release 构建部署发布版，按 $<CONFIG> 自动切换。
add_custom_command(TARGET record_expert POST_BUILD
    COMMAND "${QT_PREFIX}/bin/windeployqt.exe"
        --qmldir "${CMAKE_SOURCE_DIR}/qml"
        "$<IF:$<CONFIG:Debug>,--debug,--release>"
        "$<TARGET_FILE:record_expert>"
    COMMENT "Running windeployqt to bundle Qt runtime DLLs")
