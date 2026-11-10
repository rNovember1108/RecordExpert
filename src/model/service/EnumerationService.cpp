// EnumerationService.cpp
// 从 RecorderViewModel 中抽出的窗口 / dshow 设备枚举实现。
#include "EnumerationService.h"

#include <windows.h>
#include <QVariantMap>
#include <string>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/avutil.h>
}

namespace {

// ---- 窗口枚举 ----

// EnumWindows 回调上下文:把每个可见顶层窗口的 {title, hwnd} 收集起来。
struct WindowEnumCtx {
    QVariantList list;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<WindowEnumCtx*>(lparam);
    // 跳过不可见/无子标题窗口,以及本进程自己的窗口(避免录自己)。
    if (!IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring wtitle(static_cast<size_t>(len + 1), L'\0');
    GetWindowTextW(hwnd, &wtitle[0], len + 1);
    QString title = QString::fromStdWString(wtitle.c_str()).trimmed();
    if (title.isEmpty()) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    QVariantMap item;
    item.insert(QStringLiteral("title"), title);
    item.insert(QStringLiteral("hwnd"), QString::number(reinterpret_cast<quintptr>(hwnd)));
    ctx->list.append(item);
    return TRUE;
}

// ---- dshow 设备枚举 ----
// avdevice_list_input_sources 返回所有输入源,按 media_types 过滤。
QVariantList EnumDShowDevices(bool want_video) {
    avdevice_register_all();
    const AVInputFormat* dshow = av_find_input_format("dshow");
    if (!dshow) return {};
    AVDeviceInfoList* dev_list = nullptr;
    const int ret = avdevice_list_input_sources(dshow, nullptr, nullptr, &dev_list);
    if (ret < 0 || !dev_list) return {};
    QVariantList result;
    for (int i = 0; i < dev_list->nb_devices; ++i) {
        AVDeviceInfo* dev = dev_list->devices[i];
        if (!dev) continue;
        if (!dev->media_types || dev->nb_media_types <= 0) continue;
        bool match = false;
        for (int j = 0; j < dev->nb_media_types; ++j) {
            const bool is_video = (dev->media_types[j] == AVMEDIA_TYPE_VIDEO);
            const bool is_audio = (dev->media_types[j] == AVMEDIA_TYPE_AUDIO);
            if ((want_video && is_video) || (!want_video && is_audio)) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        QVariantMap item;
        item.insert(QStringLiteral("name"),
                     QString::fromStdString(dev->device_name ? dev->device_name : ""));
        item.insert(QStringLiteral("description"),
                     QString::fromStdString(dev->device_description ? dev->device_description : ""));
        result.append(item);
    }
    avdevice_free_list_devices(&dev_list);
    return result;
}

}  // namespace

QVariantList EnumerationService::EnumerateWindows() {
    WindowEnumCtx ctx;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.list;
}

QVariantList EnumerationService::EnumerateVideoDevices() {
    return EnumDShowDevices(/*want_video=*/true);
}

QVariantList EnumerationService::EnumerateAudioDevices() {
    return EnumDShowDevices(/*want_video=*/false);
}
