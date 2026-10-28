import QtQuick

// Fills along the bottom of a drop target while hovering is about to open it (spring-loading),
// so the wait is visible and can be abandoned by moving on.
Rectangle {
    id: bar
    property bool running: false
    property int duration: 700
    anchors.left: parent.left; anchors.bottom: parent.bottom
    anchors.leftMargin: 6; anchors.bottomMargin: 3
    height: 2; radius: 1
    width: 0
    visible: running
    onRunningChanged: { fill.stop(); width = 0; if (running) fill.start() }
    NumberAnimation { id: fill; target: bar; property: "width"; from: 0; to: bar.parent ? bar.parent.width - 12 : 0; duration: bar.duration }
}
