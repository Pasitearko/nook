import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

MenuItem {
    id: control
    property string iconName: ""
    property string shortcutText: ""
    property bool danger: false
    property var host: ApplicationWindow.window
    readonly property var theme: host && host.theme ? host.theme : Palette.fallback()
    implicitWidth: 264
    implicitHeight: visible ? 39 : 0
    height: implicitHeight
    leftPadding: 11; rightPadding: 11
    topPadding: 5; bottomPadding: 5
    opacity: (enabled ? 1 : 0.38) * reveal
    // As the menu opens, rows fade in one after another: 20 ms, then 9 ms a row (at most 8
    // rows apart), 120 ms each.
    property real reveal: 1
    Connections {
        target: control.menu
        function onAboutToShow() {
            const menu = control.menu
            appear.stop()
            if (!menu || menu.animated !== true) { control.reveal = 1; return }
            let row = 0
            while (row < menu.count && menu.itemAt(row) !== control) ++row
            appear.delay = 20 + 9 * Math.min(row, 8)
            control.reveal = 0
            appear.start()
        }
    }
    SequentialAnimation {
        id: appear
        property int delay: 20
        PauseAnimation { duration: appear.delay }
        NumberAnimation { target: control; property: "reveal"; to: 1; duration: 120; easing.type: Easing.OutCubic }
    }
    hoverEnabled: true
    // The highlight itself is drawn (and animated) once by NookMenu.
    background: Rectangle {
        radius: 7
        color: control.down ? control.theme.pressed : "transparent"
    }
    contentItem: RowLayout {
        spacing: 10
        Icon { name: control.iconName; visible: control.iconName !== ""; tint: control.danger ? control.theme.danger : control.theme.icon; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
        Text { text: control.text; color: control.danger ? control.theme.danger : control.theme.ink; font.pixelSize: 14; Layout.fillWidth: true; elide: Text.ElideRight }
        Text { text: control.shortcutText; color: control.theme.muted; font.pixelSize: 11 }
        Item { visible: control.subMenu !== null; Layout.preferredWidth: 12; Layout.preferredHeight: 16 }
    }
    arrow: Text { x: control.width - width - 12; y: (control.height - height) / 2; visible: control.subMenu !== null; text: "›"; color: control.theme.muted; font.pixelSize: 22 }
}
