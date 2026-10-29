import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

// A check box in nook's look; detail is a quieter second line.
CheckBox {
    id: check
    property string detail: ""
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    padding: 2
    spacing: 9
    readonly property real textOffset: indicator.width + spacing
    // The natural width of the text, so layouts do not size the box from its own wrapping.
    implicitWidth: leftPadding + rightPadding + textOffset + Math.max(label.implicitWidth, note.visible ? note.implicitWidth : 0)
    implicitHeight: topPadding + bottomPadding + Math.max(indicator.height + 2, column.implicitHeight)
    Layout.fillWidth: true
    opacity: enabled ? 1 : 0.45
    contentItem: Column {
        id: column
        leftPadding: check.textOffset
        spacing: 2
        Text { id: label; text: check.text; color: check.theme.ink; font.pixelSize: 14; width: Math.max(0, check.availableWidth - check.textOffset); wrapMode: Text.Wrap }
        Text { id: note; visible: check.detail !== ""; text: check.detail; color: check.theme.muted; font.pixelSize: 12; width: Math.max(0, check.availableWidth - check.textOffset); wrapMode: Text.Wrap }
    }
    indicator: Rectangle {
        implicitWidth: 18; implicitHeight: 18; radius: 5
        x: check.leftPadding; y: check.topPadding + 1
        color: check.checked ? check.theme.accent : check.theme.field
        border.color: check.checked ? check.theme.accent : check.theme.lineStrong
        Text { anchors.centerIn: parent; visible: check.checked; text: "✓"; color: check.theme.accentInk; font.pixelSize: 12; font.bold: true }
    }
}
