#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QtQml>

#include "RecorderViewModel.h"
#include "ScreenshotImageProvider.h"
#include "VideoPlayerItem.h"

// GUI 入口：启动 QGuiApplication + QQmlApplicationEngine 加载 QML 主视图。
int main(int argc, char** argv) {
    // QSettings 依赖:必须在 QGuiApplication 构造前设置,否则 QML Settings
    // (对话框尺寸/收藏夹/上次保存目录)初始化失败(AccessError)。
    QGuiApplication::setOrganizationName(QStringLiteral("RecordExpert"));
    QGuiApplication::setApplicationName(QStringLiteral("RecordExpert"));

    // 高 DPI 自适应缩放，避免 QML 在高分屏上字符过小。
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    // app 与 engine 均堆分配并故意在退出时不 delete(由 OS 随进程回收):
    // 它们的析构(QPA 平台清理 / QML 对象树销毁 + threaded render loop
    // 渲染线程同步握手)依赖 GUI 事件循环存活;exec() 返回后事件循环已停,
    // 若退出瞬间渲染/音频仍活跃(如正在播放视频),析构会永久挂死——
    // 实测播放中 quit 时 exec 正常返回 rc=0,进程却挂在析构里不退。
    // controller 保持栈对象正常析构:~RecorderViewModel 负责录制中退出时
    // 的 MP4 finalize(stop 写 trailer),不可泄漏。
    auto* app = new QGuiApplication(argc, argv);

    // 把 RecorderViewModel 作为 context property 暴露给 QML，
    // QML 内可写成 controller.start() / controller.pause() 等。
    re::RecorderViewModel controller;

    auto* engine = new QQmlApplicationEngine;

    // 注册 VideoPlayerItem QML 类型,供 PlayerWindow.qml 使用:
    //   import RecordExpert 1.0
    //   VideoPlayerItem { ... }
    qmlRegisterType<re::VideoPlayerItem>("RecordExpert", 1, 0, "VideoPlayerItem");

    // 注册 image provider：QML 的 RegionCaptureWindow 通过
    //   Image { source: "image://screenshot/desktop" }
    // 拿到 controller 在 startRegionScreenshot() 里抓的整屏 QImage 作为
    // 选区背景。provider 所有权归 engine，controller 只持裸指针。
    auto* screenshot_provider = new re::ScreenshotImageProvider;
    engine->addImageProvider(QStringLiteral("screenshot"),
                             screenshot_provider);
    controller.setImageProvider(screenshot_provider);

    engine->rootContext()->setContextProperty("controller", &controller);
    engine->load(QUrl(QStringLiteral("qrc:/qml/MainWindow.qml")));
    if (engine->rootObjects().isEmpty()) return -1;

    // 注入主窗指针：区域截图开始时 hide()，确认/取消后 show()。
    // rootObjects()[0] 是 MainWindow.qml 的 ApplicationWindow 实例。
    if (auto* win = qobject_cast<QQuickWindow*>(
            engine->rootObjects().value(0))) {
        controller.setMainWindow(win);
    }

    return app->exec();
}
