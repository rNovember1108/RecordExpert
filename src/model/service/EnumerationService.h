// EnumerationService.h
// 纯静态设备/窗口枚举。不依赖 QObject,可独立单测。
#ifndef ENUMERATION_SERVICE_H
#define ENUMERATION_SERVICE_H

#include <QVariantList>

class EnumerationService {
public:
    // 返回 [{title, hwnd}]——顶层可见窗口,跳过本进程的。
    static QVariantList EnumerateWindows();
    // 返回 [{name, description}]——dshow 视频设备。
    static QVariantList EnumerateVideoDevices();
    // 返回 [{name, description}]——dshow 音频设备。
    static QVariantList EnumerateAudioDevices();
};

#endif  // ENUMERATION_SERVICE_H
