import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

// A text field in nook's look, with the shared text menu.
TextField {
    id: field
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    ContextMenu.menu: null
    TextMenuArea { target: field }
    Layout.preferredHeight: 34
    leftPadding: 11; rightPadding: 11
    color: theme.ink; font.pixelSize: 14; selectByMouse: true
    placeholderTextColor: theme.muted
    selectionColor: theme.selection; selectedTextColor: theme.selectionText
    background: Rectangle { radius: 8; color: field.enabled ? field.theme.field : field.theme.sunken; border.width: 1; border.color: field.activeFocus ? field.theme.accent : field.theme.line }
}
