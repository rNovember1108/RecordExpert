import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12
import QtQuick.Dialogs 1.3
import Qt.labs.settings 1.0

// 主视图：顶栏两个 Tab（录屏、截图）切换子视图，主区域用 Loader 动态加载。
// 浅色主题扁平化设计,顶栏按钮等宽填满。
ApplicationWindow {
    id: window
    visible: true
    width: 536
    height: 640
    title: "RecordExpert"
    color: "#eff1f5"

    // 当前选中的 Tab 索引(0=录屏, 1=截图)
    property int currentTab: 0
    // controller.errorOccurred 的提示文本;空串时横幅隐藏。
    property string errorText: ""

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- 顶栏：两个等宽 Tab 按钮,填满整行 ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: "#e6e9ef"

            RowLayout {
                anchors.fill: parent
                spacing: 0

                // 录屏 Tab
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    property bool isActive: currentTab === 0
                    color: isActive ? "#eff1f5" : "transparent"
                    Label {
                        anchors.centerIn: parent
                        text: "录屏"
                        color: parent.isActive ? "#1e66f5" : "#9ca0b0"
                        font.pixelSize: 14
                        font.bold: parent.isActive
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { currentTab = 0; loader.source = "qrc:/qml/RecordPage.qml" }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 3
                        color: "#1e66f5"
                        visible: parent.isActive
                    }
                }

                // 截图 Tab
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    property bool isActive: currentTab === 1
                    color: isActive ? "#eff1f5" : "transparent"
                    Label {
                        anchors.centerIn: parent
                        text: "截图"
                        color: parent.isActive ? "#1e66f5" : "#9ca0b0"
                        font.pixelSize: 14
                        font.bold: parent.isActive
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { currentTab = 1; loader.source = "qrc:/qml/ScreenshotPage.qml" }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 3
                        color: "#1e66f5"
                        visible: parent.isActive
                    }
                }
            }
        }

        // 分隔线
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#ccd0da"
        }

        // 主区域：动态加载子视图。默认显示录屏视图。
        Loader {
            id: loader
            Layout.fillWidth: true
            Layout.fillHeight: true
            source: "qrc:/qml/RecordPage.qml"
        }

        // 错误提示横幅:RecorderViewModel 的业务错误统一在此展示,
        // 4 秒后自动消失,点击可立即关闭。
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 34 : 0
            visible: window.errorText !== ""
            color: "#f6d6d6"
            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: window.errorText
                color: "#b42318"
                font.pixelSize: 12
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: window.errorText = ""
            }
        }
    }

    // ---- 区域截图覆盖窗口(静态预创建) ----
    // 必须静态实例化、复用显隐:动态 createObject 创建的 QQuickWindow 在本应用
    // 中 scene graph 从不渲染首帧。注意:此实例的显隐由 C++ 控制(setOverlayWindow
    // 注入后走 show()/hide())——Qt 5.12 下 QML 对 Window-in-Window 实例的
    // visible 赋值会被静默吞掉(读回不变、onVisibleChanged 不触发),不要在
    // QML 里对它赋 visible。
    RegionCaptureWindow {
        id: regionOverlay
        visible: false
    }

    // ---- 录制期间悬浮控制窗(静态预创建) ----
    // 100x100 透明窗,显示状态行+录制计时+暂停/继续/停止。显隐由
    // C++ 在 setRecording
    // 切换时控制(同 regionOverlay 的理由:Qt 5.12 下 QML 对 Window-in-Window
    // 静态实例的 visible 赋值会被静默吞掉)。
    RecordingOverlay {
        id: recOverlay
        visible: false
    }

    // ---- 视频播放器窗口(静态预创建) ----
    // 由"播放"按钮唤起,显示 FFmpeg 解码的画面 + 倍速控制。
    PlayerWindow {
        id: playerView
        visible: false
    }

    // 启动时把覆盖窗口引用交给 controller,由 C++ 在抓帧完成后 show()。
    // 录制悬浮窗一并注入:setRecording(true) 时 show + 主窗 hide,
    // setRecording(false) 时 hide + 主窗 show。
    Component.onCompleted: {
        controller.setOverlayWindow(regionOverlay)
        controller.setRecordingOverlayWindow(recOverlay)
        // 播放器窗口一并注入:点"播放"后由 C++ show()(QML show 被吞)。
        controller.setPlayerWindow(playerView)
    }

    // ---- 区域截图保存文件对话框 ----
    // 流程:RegionCaptureWindow 点保存 → controller.requestSave() →
    // C++ 存坐标+隐藏 overlay+恢复主窗+emit saveDialogRequested →
    // 这里 open()。用户选完路径调 completeSave,取消调 cancelSave。
    // 坐标存在 C++ 的 pending_*_ 里,provider 的图也没被清,
    // completeSave 能正确裁剪。
    Connections {
        target: controller
        onSaveDialogRequested: {
            // open() 前恢复上次目录,确保对话框打开时已定位。
            if (saveDialogSettings.lastDir !== "")
                saveFileDialog.folder = saveDialogSettings.lastDir
            saveFileDialog.open()
        }
        onPlayInBuiltInPlayerRequested: {
            // 只赋 videoSource:窗口显隐由 C++ show() 控制(Qt 5.12 下 QML
            // 对 Window-in-Window 静态实例的 show() 会被静默吞掉)。
            // C++ show() 触发 PlayerWindow.onVisibleChanged → startPlayback。
            playerView.videoSource = path
        }
        onErrorOccurred: {
            window.errorText = message
            errorTimer.restart()
        }
    }

    Timer {
        id: errorTimer
        interval: 4000
        repeat: false
        onTriggered: window.errorText = ""
    }

    // 上次保存目录持久化:QSettings 后端依赖 gui_main 里设置的
    // organizationName/applicationName,缺了会初始化失败(AccessError)。
    Settings {
        id: saveDialogSettings
        category: "RegionScreenshotSave"
        property url lastDir: ""
    }

    FileDialog {
        id: saveFileDialog
        title: qsTr("Save Region Screenshot")
        selectExisting: false
        defaultSuffix: "png"
        nameFilters: [ "PNG (*.png)" ]
        onAccepted: {
            // folder 属性此时就是用户在对话框里最后浏览到的目录,
            // 直接回存,下次打开定位到同一位置。
            saveDialogSettings.lastDir = folder
            // fileUrl 本身就是 QUrl,C++ completeSave(QUrl) 内部用
            // toLocalFile() 处理前缀和百分号编码(含中文路径)。
            // 不要在 QML 里做字符串切割:substring(8) 遇到本地盘符之外的
            // file:///C%3A/... 形式或特殊字符会切错。
            controller.completeSave(saveFileDialog.fileUrl)
        }
        onRejected: {
            controller.cancelSave()
        }
    }
}
