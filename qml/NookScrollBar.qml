import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

// Out of sight until it is useful (as in Pulse): a thin thumb shows while the view scrolls and
// fades out a second after it stops; the pointer on the bar, or dragging it, keeps it shown,
// widens the thumb and brings up its track.
ScrollBar {
    id: bar
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    readonly property bool hot: hovered || pressed
    readonly property bool scrollable: size < 1
    policy: ScrollBar.AsNeeded
    hoverEnabled: true
    padding: 2
    minimumSize: 0.06
    // Shown while scrolling or under the pointer, and for a second after.
    property bool lit: false
    onActiveChanged: if (active) lit = true; else hold.restart()
    onHotChanged: if (hot) lit = true; else hold.restart()
    Timer { id: hold; interval: 1000; onTriggered: bar.lit = bar.active || bar.hot }
    contentItem: Rectangle {
        implicitWidth: bar.hot ? 8 : 4
        implicitHeight: bar.hot ? 8 : 4
        radius: 4
        color: bar.pressed ? bar.theme.muted : bar.theme.faint
        opacity: bar.lit && bar.scrollable ? 1 : 0
        Behavior on opacity { enabled: bar.animated; NumberAnimation { duration: bar.lit ? 100 : 400; easing.type: Easing.OutCubic } }
        Behavior on implicitWidth { enabled: bar.animated; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        Behavior on implicitHeight { enabled: bar.animated; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
    }
    background: Rectangle {
        radius: 6
        color: bar.theme.track
        opacity: bar.hot && bar.scrollable ? 0.55 : 0
        Behavior on opacity { enabled: bar.animated; NumberAnimation { duration: 120 } }
    }
}
