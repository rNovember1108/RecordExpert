import QtQuick 2.12
import QtQuick.Window 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12

// 微信风格拖动选框截图全屏覆盖窗口。
// 静态实例放在 MainWindow.qml,由 C++ setOverlayWindow 注入后 show()/hide()
// 控制显隐(Qt 5.12 下 QML 对 Window-in-Window 实例的 visible 赋值会被
// 静默吞掉)。
// 显示被俘整屏 QImage(image://screenshot/desktop) 作背景,鼠标拖动画选区,
// 选区内透明、外围 4 块半透明黑色遮罩(微信同款"镂空"效果),边框 + 尺寸
// 标签 + 工具栏跟随。截图模式下 ✓ 确认 → controller.copyRegionScreenshot;
// 💾 → controller.requestSave;Esc/✕ 取消 → controller.cancelRegionScreenshot。
// 三者都会关闭窗口。
ApplicationWindow {
    id: window
    // 屏幕逻辑尺寸由 controller 通过 setProperty("scrW"/"scrH") 直接注入
    // (在 performRegionCapture 里)。不要用 Screen 附加属性：Qt 5.12 下
    // 静态 ApplicationWindow 里 Screen.desktopWidth/Height 求值为 undefined
    // （实测日志 "Unable to assign [undefined] to int"），导致窗口拿不到
    // 尺寸、覆盖层无法全屏。
    property int scrW: 0
    property int scrH: 0
    // captureMode: "screenshot"（区域截图，✓复制/💾保存/✕取消）
    //              "record"    （区域录制，🎬开始录制/✕取消）
    // 由 MainWindow 在发起区域截图/区域录制前设置。
    property string captureMode: "screenshot"
    // 重取计数:静态实例在应用启动时就会创建(provider 此时还没有图,启动时
    // Image 加载失败警告无害),而 source 不变时 Image 不会重新请求 provider。
    // 每次显示前由 MainWindow 递增,URL 变化强制重新取图。
    property int reload: 0
    // 全屏靠显式几何(x/y/w/h 用传入的屏幕尺寸)+ frameless + TOPMOST flags。
    // 不要用 visibility: FullScreen + visible: true:实测 Qt 5.12 上该组合会让
    // 窗口进入诡异状态(visState 报 2=Maximized 而非 3=FullScreen),且窗口
    // scene graph 从不渲染首帧 → DWM surface 全透明,屏幕上看不到任何内容
    // (窗口存在、vis=True、TOPMOST、未 cloak,但 PrintWindow 只能抓到身后
    // 内容)。改为不设 visibility/visible,统一由 C++ 对静态实例调 show()/hide()
    // 一次性干净显示——覆盖层只需要几何上盖满屏幕,不需要独占式全屏模式
    // (微信截图同款做法)。
    x: 0
    y: 0
    width: scrW
    height: scrH
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    // 背景 Image 不透明覆盖全窗,窗口底色用黑色即可;不用 transparent 避免
    // Windows 上半透明分层窗口的合成开销与边缘伪影。
    color: "black"

    // 选区状态。初始 (0,0,0,0) 表示尚未拖出选区。dragging=true 表示
    // 鼠标按住正在画选区,期间工具栏隐藏,避免画选区时误点按钮。
    property rect selRect: Qt.rect(0, 0, 0, 0)
    property point dragStart
    property bool dragging: false

    // 选区已存在后的二次交互:调整大小(8 个手柄)或整体拖拽选区。
    // resizeHandle 取值:""/"n"/"s"/"e"/"w"/"ne"/"nw"/"se"/"sw";
    // dragMode 取值:""/"move"(整体拖拽选区)。
    property string resizeHandle: ""
    property string dragMode: ""
    property point resizeOrig      // 按下手柄时的原始鼠标位置
    property rect resizeOrigRect   // 按下手柄时的原始 selRect

    // 被俘整屏图作为背景,覆盖整窗。cache:false 确保每次创建窗口都向
    // provider 重新取图(provider 内的 QImage 在每次 startRegionScreenshot
    // 时被覆盖,默认 cache 会让 Image 拿到旧帧)。
    Image {
        id: bg
        // 窗口可见才加载:静态实例启动时 provider 尚无图,启动即加载只会报
        // "Failed to load image";C++ show() 触发 visible 变化后按最新 reload
        // 计数请求新帧。reload++ 使 URL 变化,强制重新向 provider 取图。
        source: window.visible ? ("image://screenshot/desktop?r=" + reload) : ""
        anchors.fill: parent
        fillMode: Image.Stretch  // 整屏图本身就是屏幕尺寸,无视觉缩放
        cache: false
    }

    // 拖动初始前全屏遮罩:还没开始拖动时整屏一层暗化,提示用户拖动鼠标。
    // 一旦开始拖动立刻 hide,让 4 块围选区的 Rectangle 接管暗化。
    Rectangle {
        anchors.fill: parent
        color: "#80000000"
        visible: !dragging && selRect.width === 0
    }

    // ---- 4 块外围遮罩:选区透明、外围暗化的镂空效果 ----
    // 选区上方一条
    Rectangle {
        color: "#80000000"
        x: 0; y: 0
        width: window.width
        height: selRect.y > 0 ? selRect.y : 0
        visible: selRect.height > 0
    }
    // 选区下方一条
    Rectangle {
        color: "#80000000"
        x: 0
        y: selRect.y + selRect.height
        width: window.width
        height: window.height - (selRect.y + selRect.height)
        visible: selRect.height > 0
    }
    // 选区左侧一条(上下被前两块覆盖,这里只补选区高度)
    Rectangle {
        color: "#80000000"
        x: 0
        y: selRect.y > 0 ? selRect.y : 0
        width: selRect.x > 0 ? selRect.x : 0
        height: selRect.height > 0 ? selRect.height : 0
        visible: selRect.height > 0
    }
    // 选区右侧一条
    Rectangle {
        color: "#80000000"
        x: selRect.x + selRect.width
        y: selRect.y > 0 ? selRect.y : 0
        width: window.width - (selRect.x + selRect.width)
        height: selRect.height > 0 ? selRect.height : 0
        visible: selRect.height > 0
    }

    // 选区边框:透明填充 + 蓝色 2px 边框
    Rectangle {
        id: selBorder
        color: "transparent"
        border.color: "#00aaff"
        border.width: 2
        x: selRect.x
        y: selRect.y
        width: selRect.width
        height: selRect.height
        visible: selRect.width > 0
    }

    // ---- 选区二次交互:8 个调整大小手柄 + 选区内部整体拖拽 ----
    // 全部 z:4 高于 toolbar(3)和遮罩(0)。
    // 手柄是 12×12 的半透明方块,居中贴选区的边/角。各 handle 的
    // onPositionChanged 调 window.resizeSel(handle,dx,dy) 统一计算。

    // 选区内部拖拽区:按住整体移动选区
    MouseArea {
        id: selDrag
        x: selRect.x; y: selRect.y
        width: selRect.width; height: selRect.height
        visible: selRect.width > 0 && !dragging
        z: 4
        cursorShape: Qt.SizeAllCursor
        onPressed: {
            // x/y 是 selRect 的位置(窗口坐标),mouseX/Y 是相对 selDrag 的
            // 局部坐标,两者相加 = 鼠标在窗口中的绝对位置。selDrag 随
            // selRect 移动后 mouseX 也会变,但 x+mouseX 始终等于真实位置。
            window.resizeOrig = Qt.point(x + mouseX, y + mouseY)
            window.resizeOrigRect = selRect
            dragMode = "move"
        }
        onPositionChanged: {
            if (dragMode !== "move") return
            var dx = (x + mouseX) - window.resizeOrig.x
            var dy = (y + mouseY) - window.resizeOrig.y
            var nx = window.resizeOrigRect.x + dx
            var ny = window.resizeOrigRect.y + dy
            // 夹在屏幕内
            if (nx + selRect.width > window.width) nx = window.width - selRect.width
            if (nx < 0) nx = 0
            if (ny + selRect.height > window.height) ny = window.height - selRect.height
            if (ny < 0) ny = 0
            selRect = Qt.rect(nx, ny, selRect.width, selRect.height)
        }
        onReleased: dragMode = ""
        onDoubleClicked: confirm()
    }

    // 8 个 resize handle:4 边 + 4 角。用 Repeater 避免重复代码。
    // Qt 5.12 不支持内联 component,故用 Repeater + ListModel。
    Repeater {
        model: ["n", "s", "e", "w", "nw", "ne", "sw", "se"]
        Rectangle {
            property string h: modelData
            width: 12; height: 12
            color: "#00aaff"
            border.color: "white"; border.width: 1
            z: 5
            visible: selRect.width > 0 && !dragging
            x: {
                if (h.indexOf("e") >= 0) return selRect.x + selRect.width - width/2
                if (h.indexOf("w") >= 0) return selRect.x - width/2
                return selRect.x + selRect.width/2 - width/2
            }
            y: {
                if (h.indexOf("s") >= 0) return selRect.y + selRect.height - height/2
                if (h.indexOf("n") >= 0) return selRect.y - height/2
                return selRect.y + selRect.height/2 - height/2
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: {
                    switch (parent.h) {
                    case "n": case "s": return Qt.SizeVerCursor
                    case "e": case "w": return Qt.SizeHorCursor
                    case "ne": case "sw": return Qt.SizeBDiagCursor
                    case "nw": case "se": return Qt.SizeFDiagCursor
                    }
                    return Qt.ArrowCursor
                }
                onPressed: {
                    // parent 是 handle Rectangle,其 x/y 是窗口坐标;
                    // mouseX/Y 是相对 MouseArea(=handle)的局部坐标。
                    // parent.x+mouseX 始终等于鼠标在窗口中的真实位置,
                    // 即使 handle 随 selRect 移动也不受影响。
                    window.resizeOrig = Qt.point(parent.x + mouseX, parent.y + mouseY)
                    window.resizeOrigRect = selRect
                    window.resizeHandle = parent.h
                }
                onPositionChanged: {
                    if (window.resizeHandle !== parent.h) return
                    var dx = (parent.x + mouseX) - window.resizeOrig.x
                    var dy = (parent.y + mouseY) - window.resizeOrig.y
                    window.applyResize(parent.h, dx, dy)
                }
                onReleased: window.resizeHandle = ""
            }
        }
    }

    // 尺寸标签:贴选区左上角外侧(避免与右下角工具栏争空间)
    Label {
        text: selRect.width + " × " + selRect.height
        color: "white"
        font.pixelSize: 12
        background: Rectangle { color: "#80000000" }
        padding: 4
        x: selBorder.x
        y: selBorder.y - height - 4
        visible: selRect.width > 0 && !dragging
    }

    // ---- 浮动工具栏:三模式自适应定位 ----
    // 固定尺寸常量(避免 toolbar.width/height 与 x/y 属性循环绑定):
    //   水平模式 hW=172 hH=30,竖直模式 vW=30 vH=82(三按钮 52+4+52+4+52 + pad×2)
    // 按钮用显式 x/y 而非 anchors,才能随方向切换布局。
    readonly property real hW: 172   // 水平模式总宽
    readonly property real hH: 30    // 水平模式总高
    readonly property real vW: 30    // 竖直模式总宽
    readonly property real vH: 82    // 竖直模式总高
    readonly property real btnW: 52
    readonly property real btnH: 24
    readonly property real gap: 4    // 按钮间距
    readonly property real pad: 4    // 容器内边距
    readonly property real margin: 6 // 选区与工具栏之间的外部间距
    readonly property real insideInset: 4  // inside 模式距选区边缘内缩

    // 模式判定(优先级 inside > side > outside):
    // inside:外部上下+左右都放不下 → 贴选区右下角内侧
    // side:  上下放不下但水平方向有空间 → 贴选区右/左侧竖直排列
    // outside:默认 → 贴选区下方水平排列,右侧对齐选区右边缘
    readonly property real botH: selRect.y + selRect.height
    readonly property real topFree: selRect.y - margin
    readonly property real botFree: window.height - botH - margin
    readonly property real rightFree: window.width - (selRect.x + selRect.width) - margin
    readonly property real leftFree: selRect.x - margin
    readonly property bool canOutside: topFree >= hH || botFree >= hH
    readonly property bool canSide: rightFree >= vW || leftFree >= vW
    readonly property bool canAnySide: (rightFree >= vW || leftFree >= vW) && (topFree >= vH && botFree >= vH)
    property string placeMode: !canOutside && !canAnySide ? "inside"
                           : (!canOutside && canSide ? "side"
                             : "outside")
    readonly property bool vertical: placeMode === "side"
    readonly property bool inside: placeMode === "inside"
    readonly property real toolbarW: vertical ? vW : hW
    readonly property real toolbarH: vertical ? vH : hH

    Rectangle {
        id: toolbar
        color: inside ? "#E0000000" : "#CC000000"  // inside 模式更深,避免被背景吞掉
        radius: 4
        width: window.toolbarW
        height: window.toolbarH
        visible: selRect.width > 0 && !dragging
        z: 3

        // x/y 按三种模式分别计算
        x: {
            if (inside) {
                // 选区右下角内侧,右侧对齐
                return selRect.x + selRect.width - width - window.insideInset
            }
            if (vertical) {
                // 贴右侧优先,空间不够翻左侧
                if (window.rightFree >= window.vW)
                    return selRect.x + selRect.width + window.margin
                return selRect.x - width - window.margin
            }
            // outside:右侧贴选区右边缘,空间不够翻左
            if (selRect.x + selRect.width - width < 0)
                return selRect.x
            return selRect.x + selRect.width - width
        }
        y: {
            if (inside) {
                // 选区右下角内侧
                return selRect.y + selRect.height - height - window.insideInset
            }
            if (vertical) {
                // 垂直居中于选区,夹在屏幕内
                var cy = selRect.y + selRect.height / 2 - height / 2
                if (cy < 0) cy = 0
                if (cy + height > window.height) cy = window.height - height
                return cy
            }
            // outside:选区下方优先,空间不够翻上方
            if (window.botFree >= window.hH)
                return selRect.y + selRect.height + window.margin
            return selRect.y - height - window.margin
        }

        // 确认按钮:截图模式=✓复制;录屏模式=🎬开始录制
        Rectangle {
            id: btnConfirm
            width: btnW; height: btnH
            color: confirmMouse.containsMouse ? "#40a0ff" : "#2080d0"
            radius: 3
            x: parent.vertical ? pad : parent.width - width - pad
            y: parent.vertical ? parent.height - height - pad : (parent.height - height) / 2
            Text {
                anchors.centerIn: parent
                text: captureMode === "record" ? "🎬" : "✓"
                color: "white"
                font.pixelSize: 13
            }
            MouseArea {
                id: confirmMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: confirm()
            }
        }
        // 保存按钮(打开文件对话框):仅截图模式显示,紧贴确认按钮左侧(水平)/上方(竖直)
        Rectangle {
            id: btnSave
            visible: captureMode === "screenshot"
            width: btnW; height: btnH
            color: saveMouse.containsMouse ? "#40b050" : "#209030"
            radius: 3
            x: parent.vertical ? pad : (btnConfirm.x - width - gap)
            y: parent.vertical ? (btnConfirm.y - height - gap) : (parent.height - height) / 2
            Text {
                anchors.centerIn: parent
                text: "💾"
                color: "white"
                font.pixelSize: 13
            }
            MouseArea {
                id: saveMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: saveToFile()
            }
        }
        // 取消按钮:紧贴保存按钮左侧(水平)/上方(竖直);录屏模式紧贴确认按钮
        Rectangle {
            id: btnCancel
            width: btnW; height: btnH
            color: cancelMouse.containsMouse ? "#ff6060" : "#a02020"
            radius: 3
            x: parent.vertical ? pad : (captureMode === "record" ? (btnConfirm.x - width - gap) : (btnSave.x - width - gap))
            y: parent.vertical ? ((captureMode === "record" ? btnConfirm.y : btnSave.y) - height - gap) : (parent.height - height) / 2
            Text {
                anchors.centerIn: parent
                text: "✕"
                color: "white"
                font.pixelSize: 13
            }
            MouseArea {
                id: cancelMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: cancel()
            }
        }
    }

    // ---- 自定义十字准星(已移除) ----
    // 还原系统鼠标指针,下面是 8 个调整大小手柄和选区整体拖拽区。

    // 全屏鼠标事件区:接收初始拖框 + 双击确认。选区已存在时点击外部
    // 开始新选区,覆盖旧的。
    MouseArea {
        id: ma
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        // 还原系统箭头光标,选区调整由各自的手柄/内部区域单独接管
        hoverEnabled: true
        z: 0
        onPressed: {
            // 选区已存在时点击外部 → 开始新选区(覆盖旧的)
            if (selRect.width > 0) selRect = Qt.rect(0, 0, 0, 0)
            dragStart = Qt.point(mouseX, mouseY)
            dragging = true
            selRect = Qt.rect(mouseX, mouseY, 0, 0)
        }
        onPositionChanged: {
            // 正在调大小/整体移动 → 跳过,下面 handle 自己处理
            if (resizeHandle !== "" || dragMode !== "" || !dragging) return
            var x1 = Math.min(dragStart.x, mouseX)
            var y1 = Math.min(dragStart.y, mouseY)
            var x2 = Math.max(dragStart.x, mouseX)
            var y2 = Math.max(dragStart.y, mouseY)
            selRect = Qt.rect(x1, y1, x2 - x1, y2 - y1)
        }
        onReleased: {
            dragging = false
        }
        onDoubleClicked: {
            confirm()
        }
    }

    // 键盘事件:Esc 取消,Enter/Return 确认
    Shortcut {
        sequence: "Esc"
        onActivated: cancel()
    }
    Shortcut {
        sequence: "Return"
        onActivated: confirm()
    }
    Shortcut {
        sequence: "Enter"
        onActivated: confirm()
    }

    // 根据手柄方向和鼠标位移重算 selRect。最小尺寸钳制 10px,避免选区
    // 退化成负宽/高。所有方向都从 resizeOrigRect 起始,每帧都以原始矩形
    // 为基准累加 dx/dy,这样不会累积浮点误差。
    function applyResize(h, dx, dy) {
        var MIN = 10
        var r = resizeOrigRect
        var x = r.x, y = r.y, w = r.width, hgt = r.height
        if (h.indexOf("e") >= 0) w = Math.max(MIN, r.width + dx)
        if (h.indexOf("w") >= 0) { w = Math.max(MIN, r.width - dx); x = r.x + r.width - w }
        if (h.indexOf("s") >= 0) hgt = Math.max(MIN, r.height + dy)
        if (h.indexOf("n") >= 0) { hgt = Math.max(MIN, r.height - dy); y = r.y + r.height - hgt }
        // 夹在屏幕边界内(避免拖出屏幕看不到)
        if (x < 0) { w += x; x = 0 }
        if (y < 0) { hgt += y; y = 0 }
        if (x + w > width) w = width - x
        if (y + hgt > height) hgt = height - y
        selRect = Qt.rect(x, y, w, hgt)
    }

    function confirm() {
        // 根据 captureMode 分流:
        //   截图模式 → copyRegionScreenshot(C++ 侧叠加水印后存剪贴板)
        //   录屏模式 → startRegionRecording(C++ 侧存坐标、隐藏 overlay、
        //              恢复主窗、立即 start() 进入录制态)
        // 传原始逻辑坐标即可:C++ 内部按覆盖窗尺寸与抓帧图尺寸的比例做高分屏
        // 换算,这里不要用 bg.sourceSize 换算——该属性未显式赋值时读回 (0,0)。
        if (selRect.width < 2 || selRect.height < 2) return
        var x = Math.round(selRect.x)
        var y = Math.round(selRect.y)
        var w = Math.round(selRect.width)
        var h = Math.round(selRect.height)
        if (captureMode === "record") {
            controller.startRegionRecording(x, y, w, h)
        } else {
            controller.copyRegionScreenshot(x, y, w, h)
        }
    }

    function saveToFile() {
        // 保存按钮:把选区原始坐标交给 C++ 的 requestSave。
        // C++ 会存坐标→隐藏 overlay→恢复主窗→emit saveDialogRequested,
        // 由 MainWindow.qml 里的 FileDialog 接管(不在 overlay 内打开)。
        if (selRect.width < 2 || selRect.height < 2) return
        controller.requestSave(Math.round(selRect.x), Math.round(selRect.y),
                               Math.round(selRect.width), Math.round(selRect.height))
    }

    function cancel() {
        // C++ 侧 hideRegionOverlay + restoreMainWindow + emit cancelled。
        controller.cancelRegionScreenshot()
    }

    // 显示时重置交互状态(上一轮的选区/拖动标志不能带进新会话)。
    // C++ show()/hide() 会触发本信号,而 QML 对该实例的 visible 赋值不会。
    onVisibleChanged: {
        if (visible) {
            selRect = Qt.rect(0, 0, 0, 0)
            dragging = false
            resizeHandle = ""
            dragMode = ""
        }
    }
}
