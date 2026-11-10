import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12

// 录屏子视图：开始/暂停/继续/结束 4 个按钮 + 录制范围模式 + 输出路径 + 倍率 + 状态。
// 录制范围:全屏 / 区域(选区) / 指定窗口(枚举窗口标题)。
// 浅色主题(Catppuccin Latte),与 MainWindow 统一色系。
Rectangle {
    color: "#eff1f5"

    // 枚举窗口列表供 ComboBox 绑定。按需加载:打开窗口模式时才调 enumerateWindows。
    property var windowList: []
    // 左侧标签统一列宽,保证右侧控件左边缘对齐。值 = 4 个中文字符(最大标签宽度)。
    property real labelColumnWidth: 64

    // 按关键词把音频设备分成"系统声音"和"麦克风"两组。
    // DShow 无法自动区分设备类型,只能靠设备名启发式判断。
    function categorizeAudioDevices(devices) {
        var sysKw = ["立体声混音", "stereo mix", "cable", "虚拟", "virtual",
                     "what u hear", "loopback", "wasapi"]
        var micKw = ["麦克风", "microphone", "mic", "headset", "usb audio"]
        var sys = [], mic = [], other = []
        for (var i = 0; i < devices.length; i++) {
            var d = devices[i].description.toLowerCase()
            var hit = false
            for (var j = 0; j < sysKw.length; j++) {
                if (d.indexOf(sysKw[j]) >= 0) { sys.push(devices[i]); hit = true; break }
            }
            if (hit) continue
            for (var k = 0; k < micKw.length; k++) {
                if (d.indexOf(micKw[k]) >= 0) { mic.push(devices[i]); hit = true; break }
            }
            if (!hit) other.push(devices[i])
        }
        return {sys: sys, mic: mic, other: other}
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // 标题行
        Label {
            text: "录屏"
            font.pixelSize: 18
            font.bold: true
            color: "#4c4f69"
        }

        // 录制范围模式：全屏 / 区域 / 指定窗口
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "录制范围"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: modeCombo
                model: ["全屏", "区域", "指定窗口"]
                currentIndex: 0
                enabled: !controller.isRecording
                onActivated: {
                    controller.recordMode = index
                    // 切到窗口模式时延迟加载窗口列表(耗时 < 10ms,按需即可)。
                    if (index === 2) {
                        windowList = controller.enumerateWindows()
                        windowCombo.model = windowList
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // ---- 全屏模式:无额外控件 ----

        // ---- 区域模式:"选择区域"按钮,点击后隐藏主窗、抓帧、显示覆盖窗 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: modeCombo.currentIndex === 1
            Label {
                text: "区域"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            Button {
                text: "选择区域"
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                enabled: !controller.isRecording
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "#ffffff" : "#bcc0cc"
                    font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? "#1753cc" : "#1e66f5") : "#e6e9ef"
                    radius: 6
                }
                onClicked: {
                    // 设置覆盖窗为录屏模式,然后调 C++ 前置流程。
                    // regionOverlay 是 MainWindow.qml 里的静态窗口,子文档沿
                    // context 链可引用父文档 id。
                    regionOverlay.captureMode = "record"
                    controller.startRegionRecordingCapture()
                }
            }
        }

        // ---- 指定窗口模式:枚举窗口列表供用户选 ----
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: modeCombo.currentIndex === 2
            Label {
                text: "窗口"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: windowCombo
                Layout.fillWidth: true
                model: windowList
                textRole: "title"
                enabled: !controller.isRecording && windowList.length > 0
                // QtQuick.Controls 2 ComboBox 没有 currentItem 属性;
                // 用 onActivated 的 index 参数从 model 取 title。
                onActivated: {
                    if (index >= 0 && index < windowList.length) {
                        controller.windowTitle = windowList[index].title
                    }
                }
                // 窗口列表为空时给友好提示。
                Label {
                    anchors.centerIn: parent
                    text: windowList.length === 0 && enabled
                          ? "未找到可见窗口" : ""
                    color: "#9ca0b0"
                    font.pixelSize: 12
                    z: 1
                }
            }
        }

        // 输出 MP4 路径。绑定到 controller.outputPath。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "输出路径"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            TextField {
                Layout.fillWidth: true
                text: controller.outputPath
                onTextChanged: controller.outputPath = text
                placeholderText: "output.mp4"
                color: "#4c4f69"
                font.pixelSize: 13
                background: Rectangle { color: "#ccd0da"; radius: 4 }
            }
        }

        // 倍率录制下拉框：1/2/4/8 倍。N>1 时 N 秒墙钟 → 1 秒 mp4（time-lapse）。
        // 录制开始后选项禁用，避免中途切换导致 PTS 错位。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "倍率"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: speedCombo
                model: ["1倍", "2倍", "4倍", "8倍"]
                currentIndex: 0
                enabled: !controller.isRecording
                onActivated: controller.speedMultiplier = [1, 2, 4, 8][index]
                Connections {
                    target: controller
                    onSpeedMultiplierChanged: {
                        var mapping = {1: 0, 2: 1, 4: 2, 8: 3}
                        var idx = mapping[controller.speedMultiplier]
                        if (idx !== undefined) speedCombo.currentIndex = idx
                    }
                }
            }
            Label {
                text: controller.speedMultiplier > 1
                      ? controller.speedMultiplier + " 秒墙钟→1 秒输出"
                      : "实时录制"
                color: "#9ca0b0"
                font.pixelSize: 12
            }
            Item { Layout.fillWidth: true }
        }

        // ---- 视频设备选择 ----
        // 桌面录屏硬编码为第一项(gdigrab),页面加载时枚举一次 dshow 摄像头,
        // 末尾追加"桌面+摄像头 (PiP)"混合项。次级摄像头下拉框按需枚举。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "视频"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: videoDeviceCombo
                Layout.fillWidth: true
                enabled: !controller.isRecording
                property string currentValue: controller.videoDevice
                model: []
                textRole: "description"
                onActivated: currentValue = model[index].name
                onCurrentValueChanged: controller.videoDevice = currentValue
                Component.onCompleted: {
                    var head = [{name: "desktop", description: "桌面 (gdigrab)"}]
                    var list = controller.enumerateVideoDevices()
                    var tail = [{name: "composite", description: "混合 (系统桌面+摄像头)"}]
                    model = head.concat(list).concat(tail)
                    // 默认选中第一项并同步到 controller,相当于用户手动选了它。
                    if (model.length > 0) { currentIndex = 0; currentValue = model[0].name }
                }
            }
        }

        // 画中画摄像头选择:仅当视频选"混合"时显示。
        // 首次显示时才枚举摄像头(避免启动时重复枚举)。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: videoDeviceCombo.currentValue === "composite"
            onVisibleChanged: if (visible && secondaryVideoCombo) secondaryVideoCombo.loadDevices()
            Label {
                text: "摄像头"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: secondaryVideoCombo
                Layout.fillWidth: true
                enabled: !controller.isRecording
                property string currentValue: controller.secondaryVideoDevice
                model: []
                textRole: "description"
                onActivated: currentValue = model[index].name
                onCurrentValueChanged: controller.secondaryVideoDevice = currentValue
                function loadDevices() {
                    if (model.length > 0) return
                    model = controller.enumerateVideoDevices()
                    if (model.length > 0) { currentIndex = 0; currentValue = model[0].name }
                }
            }
        }

        // ---- 音频设备选择 ----
        // 首项"系统声音+麦克风"为混合项;设备列表在页面加载时枚举一次。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "音频"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: audioDeviceCombo
                Layout.fillWidth: true
                enabled: !controller.isRecording
                property string currentValue: controller.audioDevice
                model: []
                textRole: "description"
                onActivated: currentValue = model[index].name
                onCurrentValueChanged: controller.audioDevice = currentValue
                Component.onCompleted: {
                    // 按系统声音→麦克风→混合排列,默认选中第一项(系统声音)。
                    var cat = categorizeAudioDevices(controller.enumerateAudioDevices())
                    var mixed = [{name: "mixed", description: "混合 (系统声音+麦克风)"}]
                    model = cat.sys.concat(cat.mic).concat(cat.other).concat(mixed)
                    if (model.length > 0) { currentIndex = 0; currentValue = model[0].name }
                }
            }
        }

        // 混音主源选择:仅当音频选"混合"时显示,用于挑选麦克风设备。
        // 首次显示时才枚举设备。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: audioDeviceCombo.currentValue === "mixed"
            onVisibleChanged: if (visible && primaryAudioCombo) primaryAudioCombo.loadDevices()
            Label {
                text: "麦克风"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: primaryAudioCombo
                Layout.fillWidth: true
                enabled: !controller.isRecording
                property string currentValue: controller.primaryAudioDevice
                model: []
                textRole: "description"
                onActivated: currentValue = model[index].name
                onCurrentValueChanged: controller.primaryAudioDevice = currentValue
                function loadDevices() {
                    if (model.length > 0) return
                    var cat = categorizeAudioDevices(controller.enumerateAudioDevices())
                    model = cat.mic.concat(cat.other).concat(cat.sys)
                    if (model.length > 0) { currentIndex = 0; currentValue = model[0].name }
                }
            }
        }

        // 混音副源选择:仅当音频选"混合"时显示,用于挑选立体声混音设备。
        // 首次显示时才枚举设备。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: audioDeviceCombo.currentValue === "mixed"
            onVisibleChanged: if (visible && secondaryAudioCombo) secondaryAudioCombo.loadDevices()
            Label {
                text: "系统声音"
                color: "#6c6f85"
                font.pixelSize: 13
                Layout.preferredWidth: labelColumnWidth
                Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                horizontalAlignment: Text.AlignLeft
            }
            ComboBox {
                id: secondaryAudioCombo
                Layout.fillWidth: true
                enabled: !controller.isRecording
                property string currentValue: controller.secondaryAudioDevice
                model: []
                textRole: "description"
                onActivated: currentValue = model[index].name
                onCurrentValueChanged: controller.secondaryAudioDevice = currentValue
                function loadDevices() {
                    if (model.length > 0) return
                    var cat = categorizeAudioDevices(controller.enumerateAudioDevices())
                    model = cat.sys.concat(cat.other).concat(cat.mic)
                    if (model.length > 0) { currentIndex = 0; currentValue = model[0].name }
                }
            }
        }

        // 混音提示:两个下拉框列出的是同一批音频设备,系统无自动区分能力,
        // 需用户看设备名判断。仅 mixed 模式显示。
        Label {
            Layout.fillWidth: true
            visible: audioDeviceCombo.currentValue === "mixed"
            color: "#9ca0b0"
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            text: qsTr("提示:系统声音设备通常叫\"立体声混音\"/\"Stereo Mix\"或虚拟声卡;麦克风设备通常含\"麦克风\"/\"Mic\"/\"Headset\"字样。若设备不存在,请在Windows声音设置中启用立体声混音或安装虚拟声卡。")
        }

        // "开始"/"播放"按钮行。开始按钮蓝色主按钮;播放按钮灰色次按钮,
        // 仅在非录制状态可点。播放使用内置播放器窗口打开 outputPath。
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Button {
                text: "开始"
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                enabled: !controller.isRecording && (modeCombo.currentIndex !== 1)
                highlighted: true
                contentItem: Label {
                    text: parent.text
                    color: "#ffffff"
                    font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? "#1753cc" : "#1e66f5") : "#e6e9ef"
                    radius: 6
                }
                onClicked: {
                    // 区域模式下按钮禁用,走"选择区域"→选完后 startRegionRecording 内部调 start()。
                    controller.start()
                }
            }
            Button {
                text: "播放"
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                enabled: !controller.isRecording
                highlighted: false
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "#ffffff" : "#bcc0cc"
                    font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: parent.enabled ? (parent.hovered ? "#5c6f8b" : "#6c7f9b") : "#e6e9ef"
                    radius: 6
                }
                onClicked: controller.playInBuiltInPlayer()
            }
        }

        // 状态显示(仅 Idle/错误文本,录制期间主窗已隐藏看不到)。
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
