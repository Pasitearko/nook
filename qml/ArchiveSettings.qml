import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// nook解压缩's own preferences (appearance follows nook's settings): tasks, defaults for
// extraction, notifications and the saved passwords.
NookDialog {
    id: dialog
    objectName: "archiveSettings"
    required property var app
    readonly property var prefs: app.prefs
    readonly property var store: app.passwords
    iconName: "settings"
    title: "nook解压缩 设置"
    message: "外观、缩放和动画跟随 nook 的设置。"
    width: Math.min(560, windowWidth - 40)
    onAboutToShow: { store.load(); newName.text = ""; newPassword.text = "" }

    component Heading: Text { color: dialog.theme.ink; font.pixelSize: 14; font.weight: Font.DemiBold; Layout.topMargin: 6 }
    Flickable {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(body.implicitHeight, dialog.hostWindow.layoutHeight - 240)
        contentHeight: body.implicitHeight; clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ColumnLayout {
            id: body
            width: parent.width - 10; spacing: 10
            Heading { text: "任务" }
            RowLayout {
                Layout.fillWidth: true; spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Text { text: "同时进行的任务"; color: dialog.theme.ink; font.pixelSize: 14 }
                    Text { text: "其余的排队等候，可在任务列表里调整顺序"; color: dialog.theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
                NookComboBox {
                    objectName: "archiveConcurrency"
                    Layout.preferredWidth: 96
                    model: ["1 个", "2 个", "3 个", "4 个"]
                    currentIndex: Math.max(0, Math.min(3, dialog.prefs.concurrency - 1))
                    onActivated: dialog.app.setPref("concurrency", currentIndex + 1)
                }
            }
            ArchiveSwitch {
                objectName: "archiveNotify"
                title: "完成时发送通知"
                detail: "窗口不在前台或已关闭时，用系统通知告知结果"
                checked: dialog.prefs.notify === true
                onToggled: value => dialog.app.setPref("notify", value)
            }
            Heading { text: "密码" }
            ArchiveSwitch {
                objectName: "archiveAutoTry"
                title: "自动尝试已保存的密码"
                detail: "遇到加密的压缩包时先逐个试已保存的密码，都不对再询问"
                checked: dialog.prefs.autoTryPasswords === true
                onToggled: value => dialog.app.setPref("autoTryPasswords", value)
            }
            Text {
                visible: !dialog.store.available
                text: "没有可用的密钥环（Secret Service），无法保存密码。"
                color: dialog.theme.warning; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            Text { visible: dialog.store.busy; text: "正在读取密钥环…"; color: dialog.theme.muted; font.pixelSize: 12 }
            Repeater {
                model: dialog.store.entries
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true; spacing: 10
                    Icon { name: "lock"; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                    Text { text: modelData.name; color: dialog.theme.ink; font.pixelSize: 13; elide: Text.ElideRight; Layout.preferredWidth: 150 }
                    Text { text: modelData.password; color: dialog.theme.muted; font.pixelSize: 13; font.family: "monospace"; elide: Text.ElideRight; Layout.fillWidth: true }
                    ActionButton { iconName: "trash"; hint: "删除这个密码"; implicitWidth: 30; implicitHeight: 30; onClicked: dialog.store.remove(modelData.name) }
                }
            }
            Text {
                visible: dialog.store.loaded && dialog.store.available && dialog.store.entries.length === 0
                text: "还没有保存的密码。输入密码时勾选“保存到密钥环”，或在下面添加。"
                color: dialog.theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            RowLayout {
                visible: dialog.store.available
                Layout.fillWidth: true; spacing: 6
                ArchiveField { id: newName; objectName: "archiveNewPasswordName"; placeholderText: "名称"; Layout.preferredWidth: 140 }
                ArchivePasswordField { id: newPassword; objectName: "archiveNewPassword"; placeholderText: "密码"; Layout.fillWidth: true; Layout.preferredHeight: 34 }
                ActionButton {
                    objectName: "archiveAddPassword"
                    text: "添加"; iconName: "add"; implicitHeight: 34; foreground: dialog.theme.ink
                    enabled: newName.text.trim() !== "" && newPassword.text !== ""
                    onClicked: { dialog.store.save(newName.text, newPassword.text); newName.text = ""; newPassword.text = "" }
                }
            }
        }
    }
    actions: [
        ActionButton { text: "完成"; accent: true; implicitHeight: 38; onClicked: dialog.close() }
    ]
}
