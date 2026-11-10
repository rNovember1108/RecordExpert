import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12

// 截图子视图：输出路径输入 + 截图按钮 + 区域截图按钮 + 状态显示。
// "区域截图"按下后,主窗隐藏 → controller 抓整屏 → 存 provider → C++ show()
// 静态覆盖窗口 regionOverlay(显隐统一由 MainWindow 管理,这里只负责触发入口)。
// 浅色主题(Catppuccin Latte),与 MainWindow 统一色系。
Rectangle {
    color: "#eff1f5"

    // 截图输出路径独立于录屏输出路径,默认 PNG 文件名(避免复用 output.mp4
    // 导致实际落盘为 output.mp4.png)。
    property string screenshotPath: "screenshot.png"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // 标题行
        Label {
            text: "截图"
            font.pixelSize: 18
            font.bold: true
            color: "#4c4f69"
        }

        // 输出 PNG 路径(截图专用,不带 .png 后缀时 C++ 侧会自动补)。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "输出路径"
                color: "#6c6f85"
                font.pixelSize: 13
            }
            TextField {
                Layout.fillWidth: true
                text: screenshotPath
                onTextChanged: screenshotPath = text
                placeholderText: "screenshot.png"
                color: "#4c4f69"
                font.pixelSize: 13
                background: Rectangle { color: "#ccd0da"; radius: 4 }
            }
        }

        // 两个截图按钮：左为整屏截图,右为微信风格拖动选区截图。
        // Qt 5.12 不支持内联 component,按钮样式直接内联写。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: "截图"
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                highlighted: true
                contentItem: Label {
                    text: parent.text
                    color: "#ffffff"
                    font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.hovered ? "#1753cc" : "#1e66f5"
                    radius: 6
                }
                onClicked: controller.takeScreenshot(screenshotPath)
            }
            Button {
                text: "区域截图"
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                // 覆盖窗口打开期间禁用,避免重复触发(主窗已被 controller 隐藏)。
                // regionOverlay 是 MainWindow.qml 里的静态窗口,子文档可沿
                // context 链引用父文档 id。
                enabled: !regionOverlay.visible
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "#cdd6f4" : "#45475a"
                    font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.enabled
                        ? (parent.hovered ? "#45475a" : "#313244")
                        : "#181825"
                    radius: 6
                }
                onClicked: {
                    // 截图模式:captureMode="screenshot",工具栏显示 ✓/💾/✕。
                    regionOverlay.captureMode = "screenshot"
                    controller.startRegionScreenshot()
                }
            }
        }

        // 状态显示
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: "#e6e9ef"
            radius: 6

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                Label {
                    text: "状态"
                    color: "#9ca0b0"
                    font.pixelSize: 12
                }
                Label {
                    text: controller.status
                    color: "#6c6f85"
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
