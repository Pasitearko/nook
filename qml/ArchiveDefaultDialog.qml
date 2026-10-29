import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Both changes require a deliberate confirmation; merely opening this dialog writes nothing.
NookDialog {
    id: dialog
    objectName: "archiveDefaultDialog"
    required property var app
    readonly property var defaults: app.defaults
    property bool restoring: false
    property string errorMessage: ""
    signal completed(string message)

    iconName: "archive"
    title: restoring ? "恢复之前的默认程序？" : "设为默认压缩软件？"
    message: restoring
        ? "只恢复目前仍由 nook解压缩 打开的类型；你后来手动改给其他程序的类型会保留。"
        : "将下面 " + defaults.typeCount + " 种文件类型交给 nook解压缩 打开，包含 ISO、deb/rpm、jar/apk、dmg；不接管程序、办公文档或虚拟磁盘。"
    width: Math.min(600, windowWidth - 40)

    function openFor(restore) {
        restoring = restore === true
        errorMessage = ""
        defaults.refresh()
        open()
    }
    function apply() {
        if (restoring ? !defaults.canRestore : defaults.state === "unavailable") return
        errorMessage = restoring ? defaults.restore() : defaults.setAsDefault()
        if (errorMessage !== "") return
        completed(restoring ? "已恢复之前的默认程序" : "已设为默认压缩软件")
        close()
    }
    onAccepted: apply()

    Text {
        Layout.fillWidth: true
        text: dialog.restoring
            ? "不会整份覆盖 mimeapps.list，也不会修改无关文件类型。备份文件会保留。"
            : "更改前先备份现有的 " + dialog.defaults.mimeappsPath + "，并记录原默认程序。以后可在设置里恢复。"
        color: dialog.theme.muted; font.pixelSize: 12; wrapMode: Text.WrapAnywhere
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.max(70, Math.min(220, dialog.hostWindow.layoutHeight - 360))
        color: dialog.theme.field; radius: 8; border.color: dialog.theme.line
        ListView {
            objectName: "archiveDefaultTypes"
            anchors.fill: parent; anchors.margins: 8; clip: true
            model: dialog.defaults.associations
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width - 10; height: 32; spacing: 12
                Text {
                    text: modelData.type; color: dialog.theme.ink; font.pixelSize: 12
                    Layout.fillWidth: true; elide: Text.ElideRight
                }
                Text {
                    text: modelData.name; color: modelData.ours ? dialog.theme.accentText : dialog.theme.muted
                    font.pixelSize: 12; Layout.maximumWidth: 160; elide: Text.ElideRight
                }
            }
        }
    }
    Text {
        objectName: "archiveDefaultError"
        visible: text !== ""; text: dialog.errorMessage
        color: dialog.theme.danger; font.pixelSize: 12; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true
    }
    actions: [
        ActionButton {
            objectName: "archiveDefaultCancel"; text: "取消"; implicitHeight: 38
            foreground: dialog.theme.ink; onClicked: dialog.close()
        },
        ActionButton {
            objectName: "archiveDefaultConfirm"
            text: dialog.restoring ? "恢复之前的默认" : "设为默认"; accent: true; implicitHeight: 38
            enabled: dialog.restoring ? dialog.defaults.canRestore : dialog.defaults.state !== "unavailable"
            onClicked: dialog.apply()
        }
    ]
}
