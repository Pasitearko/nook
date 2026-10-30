import QtQuick
import QtQuick.Controls
import "palette.js" as Palette
import "motion.js" as Motion

// The file view's size: a short rail beside the list and grid buttons, a small square at one end
// and a large one at the other. Drag the dot and the files grow or shrink with it; the dot rests
// a moment at 100% on the way past, a click on the rail jumps there, the wheel steps by 5%, and a
// double-click goes back to 100%.
Item {
    id: slider
    property real value: 1            // what the view shows now
    property real from: 0.8
    property real to: 1.6
    property string label: "大小"
    // Live while dragging, then once more when let go (worth saving).
    signal moved(real value)
    signal committed(real value)
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    readonly property bool dragging: area.pressed
    readonly property bool hot: area.containsMouse || area.pressed
    implicitWidth: 120
    implicitHeight: 36
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: label + " " + Math.round(value * 100) + "%"

    readonly property real railLeft: 23
    readonly property real railWidth: Math.max(1, width - 48)
    function positionOf(v) { return Math.max(0, Math.min(1, (v - from) / (to - from))) }
    function valueAt(x) {
        const p = Math.max(0, Math.min(1, (x - railLeft) / railWidth))
        // A detent at 100%: within a few pixels the dot settles there.
        if (Math.abs(p - positionOf(1)) * railWidth < 4 && from < 1 && to > 1) return 1
        return Math.round((from + p * (to - from)) * 100) / 100
    }
    function step(delta) {
        const next = Math.max(from, Math.min(to, Math.round((value + delta) * 100) / 100))
        if (next !== value) committed(next)
    }
    Keys.onLeftPressed: step(-0.05)
    Keys.onRightPressed: step(0.05)
    Keys.onDownPressed: step(-0.05)
    Keys.onUpPressed: step(0.05)

    // The ends say which way is bigger.
    Rectangle {
        x: 4; anchors.verticalCenter: parent.verticalCenter
        width: 8; height: 8; radius: 2; color: "transparent"
        border.width: 1.5; border.color: slider.hot ? slider.theme.iconHover : slider.theme.icon
        Behavior on border.color { enabled: slider.animated; ColorAnimation { duration: 100 } }
    }
    Rectangle {
        anchors.right: parent.right; anchors.rightMargin: 2; anchors.verticalCenter: parent.verticalCenter
        width: 13; height: 13; radius: 3; color: "transparent"
        border.width: 1.5; border.color: slider.hot ? slider.theme.iconHover : slider.theme.icon
        Behavior on border.color { enabled: slider.animated; ColorAnimation { duration: 100 } }
    }
    Item {
        id: rail
        x: slider.railLeft; width: slider.railWidth; height: 4
        anchors.verticalCenter: parent.verticalCenter
        // Where the dot is drawn: it follows the pointer exactly while held and glides to a value
        // set any other way (the other view's size, the wheel, a double-click).
        property real position: slider.positionOf(slider.value)
        Behavior on position {
            enabled: slider.animated && !area.pressed
            NumberAnimation { duration: Motion.smooth.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.smooth.curve }
        }
        Rectangle { anchors.fill: parent; radius: 2; color: slider.theme.track }
        Rectangle { width: rail.position * parent.width; height: parent.height; radius: 2; color: slider.theme.accent; opacity: slider.hot ? 1 : 0.85 }
        // 100%.
        Rectangle {
            visible: slider.from < 1 && slider.to > 1
            x: slider.positionOf(1) * parent.width - 1; y: -3
            width: 2; height: 10; radius: 1
            color: rail.position * parent.width > x + 1 ? slider.theme.accentHover : slider.theme.lineStrong
        }
        Rectangle {
            id: knob
            objectName: "zoomSliderKnob"
            width: 14; height: 14; radius: 7
            x: rail.position * parent.width - width / 2; anchors.verticalCenter: parent.verticalCenter
            color: slider.theme.knob
            border.width: 2; border.color: slider.theme.accent
            scale: area.pressed ? 1.22 : area.containsMouse || slider.activeFocus ? 1.12 : 1
            Behavior on scale { enabled: slider.animated; NumberAnimation { duration: Motion.snappy.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.snappy.curve } }
            Rectangle { anchors.fill: parent; anchors.topMargin: 1.5; anchors.bottomMargin: -1.5; z: -1; radius: width / 2; color: slider.theme.shadow; opacity: 0.6 }
        }
    }
    MouseArea {
        id: area
        objectName: "zoomSliderArea"
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor
        // Held by the dot, it keeps the spot it was taken by; pressed on the rail, it jumps there.
        property real grab: 0
        property real live: 1
        onPressed: event => {
            const center = rail.x + rail.position * rail.width
            grab = Math.abs(event.x - center) <= 10 ? event.x - center : 0
            live = slider.valueAt(event.x - grab)
            slider.moved(live)
        }
        onPositionChanged: event => {
            if (!pressed) return
            const next = slider.valueAt(event.x - grab)
            if (next !== live) { live = next; slider.moved(live) }
        }
        onReleased: slider.committed(live)
        onCanceled: slider.committed(live)
        // The release that ends a double-click saves 100% too.
        onDoubleClicked: { live = 1; slider.moved(1); slider.committed(1) }
        onWheel: wheel => slider.step(wheel.angleDelta.y > 0 || wheel.angleDelta.x > 0 ? 0.05 : -0.05)
    }
    // The size shows at once while the dot is held, after a moment on hover.
    Timer { id: settle; interval: 450; running: area.containsMouse && !area.pressed }
    property bool hoverShown: false
    Connections { target: settle; function onTriggered() { slider.hoverShown = true } }
    Connections { target: area; function onContainsMouseChanged() { if (!area.containsMouse) slider.hoverShown = false } }
    NookToolTip {
        objectName: "zoomSliderTip"
        visible: area.pressed || (slider.hoverShown && area.containsMouse)
        delay: 0
        text: slider.label + " " + Math.round(slider.value * 100) + "%" + (area.pressed ? "" : " · 双击恢复 100%")
        x: rail.x + rail.position * rail.width - width / 2
        y: slider.height + 4
    }
}
