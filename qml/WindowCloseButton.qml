import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

// The window's ✕ in the top-right corner: the same size as the toolbar buttons beside it,
// turning red under the pointer as on Windows.
AbstractButton {
    id: control
    property string hint: "关闭窗口"
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.controller.settings.values.animations : false
    implicitWidth: 40
    implicitHeight: 36
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.name: hint
    background: Rectangle {
        radius: 8
        color: control.hovered || control.down ? control.theme.closeHover : "transparent"
        opacity: control.down ? 0.82 : 1
        Behavior on color { enabled: control.animated; ColorAnimation { duration: 100 } }
    }
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: "close"; width: 18; height: 18
            tint: control.hovered || control.down ? "#ffffff" : control.theme.icon
        }
    }
    NookToolTip { visible: control.hovered && control.hint !== ""; text: control.hint; delay: 700 }
}
