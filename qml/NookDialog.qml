import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette
import "motion.js" as Motion

// A question in nook's own style (the look of the rename dialog): an icon badge, a title and
// message, optional content below them, and the answers on the right. Destructive questions
// (danger) use red instead of terracotta.
Popup {
    id: dialog
    property string title: ""
    property string message: ""
    property string iconName: "file"
    property bool danger: false
    property bool wrapAnywhere: false
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    readonly property real windowWidth: hostWindow && hostWindow.layoutWidth ? hostWindow.layoutWidth : 600
    readonly property color tone: danger ? theme.danger : theme.accent
    default property alias content: body.data
    property alias actions: actionRow.data
    // Enter answers with the main action, as on Windows.
    signal accepted()

    anchors.centerIn: parent
    width: Math.min(456, windowWidth - 40)
    modal: true; focus: true; padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: dialog.theme.scrim }
    // The card pops in, its icon badge pops from 0.6 and the text rises the last few pixels.
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: dialog.animated ? 150 : 0; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: dialog.animated ? Motion.pop.ms : 0; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.pop.curve }
            NumberAnimation { target: badge; property: "scale"; from: 0.6; to: 1; duration: dialog.animated ? Motion.pop.ms : 0; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.pop.curve }
            NumberAnimation { target: rise; property: "y"; from: 8; to: 0; duration: dialog.animated ? Motion.snappy.ms : 0; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.snappy.curve }
        }
    }
    exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: dialog.animated ? 90 : 0 } }
    background: Item {
        Rectangle { anchors.fill: parent; anchors.topMargin: 10; anchors.margins: -2; radius: 20; color: dialog.theme.shadow }
        Rectangle { anchors.fill: parent; anchors.topMargin: 4; radius: 18; color: dialog.theme.shadow }
        Rectangle {
            anchors.fill: parent; radius: 18
            color: dialog.theme.raised
            border.width: 1; border.color: dialog.theme.line
            Rectangle {
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                height: 76; radius: 17
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.alpha(dialog.tone, dialog.theme.dark ? 0.14 : 0.10) }
                    GradientStop { position: 1; color: "transparent" }
                }
            }
        }
    }
    contentItem: FocusScope {
        implicitWidth: column.implicitWidth
        implicitHeight: column.implicitHeight
        focus: true
        Keys.onReturnPressed: dialog.accepted()
        Keys.onEnterPressed: dialog.accepted()
        ColumnLayout {
            id: column
            anchors.fill: parent
            spacing: 0
            transform: Translate { id: rise }
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 22; Layout.bottomMargin: 16; spacing: 14
                Rectangle {
                    id: badge
                    Layout.preferredWidth: 46; Layout.preferredHeight: 46; Layout.alignment: Qt.AlignTop; radius: 13
                    color: dialog.danger ? dialog.theme.dangerSoft : dialog.theme.accentSoft
                    border.width: 1; border.color: Qt.alpha(dialog.tone, 0.35)
                    Icon { anchors.centerIn: parent; name: dialog.iconName; width: 24; height: 24; tint: dialog.danger ? dialog.theme.danger : dialog.theme.accentText }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 5
                    Text { text: dialog.title; color: dialog.theme.ink; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Text {
                        visible: dialog.message !== ""
                        text: dialog.message; color: dialog.theme.muted; font.pixelSize: 13; lineHeight: 1.15
                        Layout.fillWidth: true; wrapMode: dialog.wrapAnywhere ? Text.WrapAnywhere : Text.Wrap
                    }
                }
            }
            ColumnLayout {
                id: body
                visible: children.length > 0
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.leftMargin: 22; Layout.rightMargin: 22; Layout.bottomMargin: 4
                spacing: 10
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: 14; height: 1; color: dialog.theme.line; opacity: 0.8 }
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 14; Layout.leftMargin: 22; Layout.rightMargin: 16; spacing: 8
                Item { Layout.fillWidth: true }
                RowLayout { id: actionRow; spacing: 8 }
            }
        }
    }
}
