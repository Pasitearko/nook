import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

ToolTip {
    id: tip
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    delay: 650
    timeout: 6000
    padding: 10
    leftPadding: 13
    rightPadding: 13
    implicitWidth: Math.min(420, contentItem.implicitWidth + leftPadding + rightPadding)
    contentItem: Text {
        text: tip.text
        color: tip.theme.tooltipText
        font.pixelSize: 12
        wrapMode: Text.Wrap
        maximumLineCount: 5
        elide: Text.ElideRight
    }
    background: Rectangle {
        radius: 9
        color: tip.theme.tooltip
        border.color: tip.theme.tooltip
        border.width: 1
    }
}
