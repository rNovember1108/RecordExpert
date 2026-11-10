import QtQuick 2.12
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.12
import QtQuick.Window 2.12
import RecordExpert 1.0

// 视频播放器窗口:用 FFmpeg 解码,QSGImageNode 渲染(场景图纹理绘制)。
// 支持 1x/2x/4x 倍速播放。由 RecordPage 的"播放"按钮唤起。
Window {
    id: playerWindow
    title: "视频播放"
    width: 960
    height: 600
    color: "#1a1a1f"

    // 视频文件路径,由调用方传入。
    property string videoSource: ""

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0

        // 视频画面区域
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000000"

            VideoPlayerItem {
                id: videoPlayer
                anchors.fill: parent
                // 不绑定 videoSource:窗口显示时 startPlayback() 统一 open,
                // 避免绑定赋值 + startPlayback 双重 open 重启解码。

                onPositionChanged: {
                    var ms = videoPlayer.position
                    var s = Math.floor(ms / 1000)
                    var m = Math.floor(s / 60)
                    s = s % 60
                    timeLabel.text = (m < 10 ? "0" + m : m) + ":" +
                                     (s < 10 ? "0" + s : s) + " / " +
                                     formatDuration(videoPlayer.duration)
                    progressBar.value = videoPlayer.duration > 0
                        ? ms / videoPlayer.duration : 0
                }
                onFinished: {
                    playButton.text = "播放"
                }
                onErrorOccurred: {
                    errorLabel.text = msg
                    errorLabel.visible = true
                }
                onVideoInfoChanged: {
                    var w = videoPlayer.videoWidth
                    var h = videoPlayer.videoHeight
                    if (w > 0 && h > 0) {
                        // 根据视频宽高比调整窗口大小
                        var maxW = 1280
                        var maxH = 720
                        var scaleW = maxW / w
                        var scaleH = maxH / h
                        var scale = Math.min(scaleW, scaleH, 1.0)
                        var newW = Math.round(w * scale)
                        var newH = Math.round(h * scale)
                        // 加上控制栏高度
                        playerWindow.width = newW
                        playerWindow.height = newH + 60
                    }
                }
            }

            // 错误提示
            Label {
                id: errorLabel
                anchors.centerIn: parent
                color: "#ff6b6b"
                font.pixelSize: 14
                visible: false
            }
        }

        // 控制栏
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: "#2a2a31"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                // 播放/暂停按钮
                Button {
                    id: playButton
                    text: "播放"
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 34
                    contentItem: Label {
                        text: parent.text
                        color: "#ffffff"
                        font.pixelSize: 13; font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? "#1753cc" : "#1e66f5"
                        radius: 4
                    }
                    onClicked: {
                        if (videoPlayer.playing) {
                            videoPlayer.pause()
                            playButton.text = "播放"
                        } else {
                            videoPlayer.play()
                            playButton.text = "暂停"
                        }
                    }
                }

                // 进度条
                Slider {
                    id: progressBar
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: 0
                    enabled: false  // 暂不支持拖动 seek
                }

                // 时间显示
                Label {
                    id: timeLabel
                    color: "#9ca0b0"
                    font.pixelSize: 12
                    font.family: "Consolas"
                    text: "00:00 / 00:00"
                }

                // 分隔线
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 24
                    color: "#4a4a55"
                }

                // 倍速按钮组
                Repeater {
                    model: [1, 2, 4]
                    Button {
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 28
                        text: modelData + "x"
                        highlighted: videoPlayer.speed === modelData
                        contentItem: Label {
                            text: parent.text
                            color: parent.highlighted ? "#ffffff" : "#9ca0b0"
                            font.pixelSize: 12; font.bold: parent.highlighted
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: parent.highlighted
                                ? "#1e66f5"
                                : (parent.hovered ? "#3a3a45" : "transparent")
                            border.color: parent.highlighted
                                ? "#1e66f5" : "#4a4a55"
                            border.width: 1
                            radius: 4
                        }
                        onClicked: {
                            videoPlayer.speed = modelData
                        }
                    }
                }

                // 分隔线
                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 24
                    color: "#4a4a55"
                }

                // 停止按钮
                Button {
                    text: "停止"
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 28
                    contentItem: Label {
                        text: parent.text
                        color: "#ff6b6b"
                        font.pixelSize: 12; font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? "#3a1515" : "transparent"
                        border.color: "#6a2a2a"
                        border.width: 1
                        radius: 4
                    }
                    onClicked: {
                        videoPlayer.stop()
                        playButton.text = "播放"
                        timeLabel.text = "00:00 / " + formatDuration(videoPlayer.duration)
                        progressBar.value = 0
                    }
                }
            }
        }
    }

    // 工具函数:毫秒 → "MM:SS" 格式
    function formatDuration(ms) {
        var s = Math.floor(ms / 1000)
        var m = Math.floor(s / 60)
        s = s % 60
        return (m < 10 ? "0" + m : m) + ":" + (s < 10 ? "0" + s : s)
    }

    // 窗口打开后自动开始播放。
    // 静态 Window 由 C++ show() 触发 onVisibleChanged 进入播放流程。
    // videoSource 由 MainWindow 在 show() 前赋值。
    function startPlayback() {
        if (videoSource.length > 0) {
            videoPlayer.open(videoSource)
            videoPlayer.play()
            playButton.text = "暂停"
        }
    }
    onVisibleChanged: {
        if (visible) startPlayback()
    }

    // 关闭窗口时停止播放
    onClosing: {
        videoPlayer.stop()
    }
}
