import QtQuick
import QtQuick.Controls

// A thin vertical strip on a panel's edge that drags its width (the sidebar, the details pane),
// with the split view's look: a short accent bar while the pointer is on it or dragging.
// Widths are in the coordinates of `reference`, so the window's interface scale is respected.
Item {
    id: grip
    required property Item reference
    property real value: 0
    property real minimum: 0
    property real maximum: 10000
    // +1: dragging right widens (a panel on the left); -1: dragging left widens (one on the right).
    property int direction: 1
    readonly property bool dragging: area.pressed
    signal moved(real width)
    signal finished(real width)
    signal reset()
    width: 10
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.SplitHCursor
        preventStealing: true
        property real startX: 0
        property real startValue: 0
        onPressed: event => { startX = mapToItem(grip.reference, event.x, 0).x; startValue = grip.value }
        onPositionChanged: event => {
            if (!pressed) return
            const x = mapToItem(grip.reference, event.x, 0).x
            grip.moved(Math.max(grip.minimum, Math.min(grip.maximum, startValue + (x - startX) * grip.direction)))
        }
        onReleased: grip.finished(grip.value)
        // Double-click: back to the default width.
        onDoubleClicked: grip.reset()
    }
    Rectangle {
        anchors.centerIn: parent
        width: 2; height: 28; radius: 1
        readonly property var host: ApplicationWindow.window
        color: host && host.theme ? host.theme.accent : "#c96442"
        opacity: area.pressed || area.containsMouse ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
}
