import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

// A thin progress bar; indeterminate shows a sliding segment.
Rectangle {
    id: bar
    property real value: 0
    property bool indeterminate: false
    property color tone: theme.accent
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    implicitHeight: 6
    radius: height / 2
    color: theme.sunken
    clip: true
    Rectangle {
        visible: !bar.indeterminate
        height: parent.height; radius: parent.radius
        width: Math.max(parent.height, parent.width * Math.max(0, Math.min(1, bar.value)))
        color: bar.tone
        Behavior on width { enabled: bar.animated; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }
    Rectangle {
        id: slider
        visible: bar.indeterminate
        height: parent.height; radius: parent.radius
        width: parent.width * 0.3
        color: bar.tone
        NumberAnimation on x { running: bar.indeterminate && bar.visible; from: -slider.width; to: bar.width; duration: 1100; loops: Animation.Infinite }
    }
}
