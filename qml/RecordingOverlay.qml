import QtQuick 2.12
import QtQuick.Window 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12

// 录制期间悬浮控制窗:桌面右下角 100x100 透明窗。
// 自上而下:状态行(绿/红闪烁点 + "录制中"/"暂停中")→ 录制计时(controller.elapsedText)
// → 暂停/继续 → 停止。
// 显隐由 C++ controller 在 setRecording(true/false) 时调用 show()/hide():
// Qt 5.12 下 QML 对 Window-in-Window 静态实例的 visible 赋值会被静默吞掉
// (详见 RegionCaptureWindow 注释),故统一由 C++ 控制显隐。
// 位置由 C++ 在 show 前 setPosition 到主屏右下角(避开任务栏)。
ApplicationWindow {
    id: window
    width: 100
    height: 100
    // Frameless + TopMost:无标题栏、置顶(不设置 Qt.Tool)。
    // color:"transparent" 让窗口本身透明,只有内部控件可见,适合任意桌面背景。
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"


    // 整体外框:半透明深色圆角矩形,承载状态行/计时器和按钮,确保在亮色桌面上可读。
    Rectangle {
        anchors.fill: parent
        color: "#CC1e1e2e"
        radius: 8

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 3

            // ---- 状态行:最前面的闪烁指示点 + 状态文字(位于计时之前) ----
            // 录制中:绿点闪烁 + "录制中";暂停中:红点闪烁 + "暂停中"。
            // 窗口宽仅 100px,状态行压缩到 14px。
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 14
                Row {
                    anchors.centerIn: parent
                    spacing: 4
                    // 闪烁指示点:录制中绿色,暂停中红色。动画 running 绑定
                    // isRecording——录制期间(含暂停)持续闪烁,窗口隐藏时随之停止。
                    Rectangle {
                        id: blinkDot
                        width: 8; height: 8; radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: controller.isPaused ? "#d20f39" : "#40a02b"
                        SequentialAnimation on opacity {
                            running: controller.isRecording
                            loops: Animation.Infinite
                            NumberAnimation { from: 1.0; to: 0.15; duration: 500 }
                            NumberAnimation { from: 0.15; to: 1.0; duration: 500 }
                        }
                    }
                    Label {
                        text: controller.isPaused ? "暂停中" : "录制中"
                        color: controller.isPaused ? "#fe640b" : "#40a02b"
                        font.pixelSize: 10
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            // 计时器 "HH:MM:SS"
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                color: "#80000000"
                radius: 4
                Label {
                    anchors.centerIn: parent
                    text: controller.elapsedText
                    // 暂停时橙色,录制时绿色。
                    color: controller.isPaused ? "#fe640b" : "#40a02b"
                    font.pixelSize: 13
                    font.bold: true
                    font.family: "Consolas"
                }
            }

            // 暂停/继续 切换按钮:同位置,根据 isPaused 互斥显示。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                color: pauseMouse.containsMouse ? "#bcc0cc" : "#ccd0da"
                radius: 4
                visible: controller.isRecording && !controller.isPaused
                Label {
                    anchors.centerIn: parent
                    text: "暂停"
                    color: "#4c4f69"
                    font.pixelSize: 11
                    font.bold: true
                }
                MouseArea {
                    id: pauseMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: controller.pause()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                color: resumeMouse.containsMouse ? "#bcc0cc" : "#ccd0da"
                radius: 4
                visible: controller.isPaused
                Label {
                    anchors.centerIn: parent
                    text: "继续"
                    color: "#4c4f69"
                    font.pixelSize: 11
                    font.bold: true
                }
                MouseArea {
                    id: resumeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: controller.resume()
                }
            }

            // 停止按钮:红底,录制中常驻。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                color: stopMouse.containsMouse ? "#d20f39" : "#e64553"
                radius: 4
                visible: controller.isRecording
                Label {
                    anchors.centerIn: parent
                    text: "停止"
                    color: "white"
                    font.pixelSize: 11
                    font.bold: true
                }
                MouseArea {
                    id: stopMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: controller.stop()
                }
            }
        }
    }
}
