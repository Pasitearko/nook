import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

ComboBox {
    id: control
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property color ink: theme.ink
    implicitWidth: 120
    implicitHeight: 34
    leftPadding: 12
    rightPadding: 29
    font.pixelSize: 13
    focusPolicy: Qt.TabFocus
    contentItem: Text {
        text: control.displayText; color: control.ink
        font: control.font
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    indicator: Text {
        text: "›"; rotation: control.popup.visible ? -90 : 90
        color: control.theme.muted
        font.pixelSize: 17
        x: control.width - width - 12; anchors.verticalCenter: parent.verticalCenter
        Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    }
    // Same surface as the address, search and filter fields.
    background: Rectangle {
        radius: 8
        color: control.down ? control.theme.sunken : control.theme.field
        border.width: 1
        border.color: control.activeFocus || control.popup.visible ? control.theme.accent : control.hovered ? control.theme.lineStrong : control.theme.line
    }
    popup: Popup {
        y: control.height + 5
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110; easing.type: Easing.OutCubic } }
        width: control.width
        padding: 5
        implicitHeight: contentItem.implicitHeight + 10
        background: Rectangle { radius: 10; color: control.theme.raised; border.color: control.theme.line }
        contentItem: ListView {
            clip: true; implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
    delegate: ItemDelegate {
        required property int index
        required property var modelData
        width: control.width - 10; height: 34
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: modelData; color: control.ink; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: 6; color: parent.highlighted ? control.theme.menuHighlight : "transparent" }
    }
}
