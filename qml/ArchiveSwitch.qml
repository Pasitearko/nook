import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

// A setting that is on or off: title, an optional explanation, and nook's switch.
RowLayout {
    id: row
    property string title: ""
    property string detail: ""
    property alias checked: toggle.checked
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    signal toggled(bool value)
    Layout.fillWidth: true; spacing: 12
    ColumnLayout {
        Layout.fillWidth: true; spacing: 2
        Text { text: row.title; color: row.theme.ink; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.Wrap }
        Text { visible: row.detail !== ""; text: row.detail; color: row.theme.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
    }
    Switch {
        id: toggle
        padding: 0; implicitWidth: 42; implicitHeight: 26
        onToggled: row.toggled(checked)
        indicator: Rectangle {
            implicitWidth: 42; implicitHeight: 26; radius: 13
            color: toggle.checked ? row.theme.accent : row.theme.toggleOff
            Behavior on color { enabled: row.animated; ColorAnimation { duration: 130 } }
            Rectangle { x: toggle.checked ? 19 : 3; y: 3; width: 20; height: 20; radius: 10; color: row.theme.knob; Behavior on x { enabled: row.animated; NumberAnimation { duration: 130 } } }
        }
    }
}
