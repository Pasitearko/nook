import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A password field with a show/hide button.
ArchiveField {
    id: field
    property alias revealed: reveal.checked
    Layout.preferredHeight: 38
    rightPadding: 44
    echoMode: reveal.checked ? TextInput.Normal : TextInput.Password
    ActionButton {
        id: reveal
        checkable: true
        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 3
        implicitWidth: 32; implicitHeight: 32; leftPadding: 7; rightPadding: 7
        iconName: "hidden"; active: checked; hint: checked ? "隐藏密码" : "显示密码"
    }
}
