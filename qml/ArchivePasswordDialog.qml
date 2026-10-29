import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks for a package's password. Saved passwords (shown with their names, in plain text, as
// chosen in the design) can be picked; a typed one can be kept in the keyring under a name.
NookDialog {
    id: dialog
    objectName: "archivePasswordDialog"
    required property var app
    property string archiveName: ""
    property string suggestedName: ""
    property bool wrong: false
    property bool listing: false // the file list itself is encrypted
    readonly property var store: app.passwords
    signal submitted(string password, string saveName)
    signal declined()

    iconName: "lock"
    title: wrong ? "密码不正确" : "需要密码"
    message: wrong ? "请重新输入“" + archiveName + "”的密码。"
                   : listing ? "“" + archiveName + "”的文件列表已加密，输入密码后才能查看。"
                             : "“" + archiveName + "”已加密，请输入密码。"
    wrapAnywhere: true
    closePolicy: Popup.CloseOnEscape
    property bool answered: false
    function submit() {
        if (field.text === "") return
        answered = true
        submitted(field.text, keep.checked ? keepName.text.trim() : "")
        close()
    }
    onAboutToShow: {
        answered = false
        field.text = ""; field.revealed = false
        keep.checked = false; keepName.text = suggestedName
        store.load()
    }
    onOpened: field.forceActiveFocus()
    onAccepted: submit()
    onClosed: if (!answered) declined()

    ArchivePasswordField {
        id: field; objectName: "archivePasswordField"
        Layout.fillWidth: true
        placeholderText: "密码"
        onAccepted: dialog.submit()
    }
    // Saved passwords: one click fills the field.
    ColumnLayout {
        visible: dialog.store.entries.length > 0
        Layout.fillWidth: true; spacing: 2
        Text { text: "已保存的密码"; color: dialog.theme.muted; font.pixelSize: 12 }
        Flickable {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(savedColumn.implicitHeight, 132)
            contentHeight: savedColumn.implicitHeight; clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Column {
                id: savedColumn; width: parent.width
                Repeater {
                    model: dialog.store.entries
                    ItemDelegate {
                        id: saved
                        objectName: "archiveSavedPassword"
                        required property var modelData
                        width: savedColumn.width; implicitHeight: 32
                        hoverEnabled: true
                        background: Rectangle { radius: 7; color: saved.hovered ? dialog.theme.hover : "transparent" }
                        contentItem: RowLayout {
                            spacing: 10
                            Icon { name: "lock"; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                            Text { text: saved.modelData.name; color: dialog.theme.ink; font.pixelSize: 13; elide: Text.ElideRight; Layout.preferredWidth: 130 }
                            Text { text: saved.modelData.password; color: dialog.theme.muted; font.pixelSize: 13; font.family: "monospace"; elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                        onClicked: { field.text = modelData.password; field.forceActiveFocus() }
                        onDoubleClicked: { field.text = modelData.password; dialog.submit() }
                    }
                }
            }
        }
    }
    Text {
        visible: dialog.store.busy
        text: "正在读取密钥环…"; color: dialog.theme.muted; font.pixelSize: 12
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 8
        ArchiveCheck {
            id: keep; objectName: "archiveKeepPassword"
            text: "保存到密钥环"
            enabled: dialog.store.available
            detail: dialog.store.available ? "" : "没有可用的密钥环，无法保存"
        }
        ArchiveField {
            id: keepName
            visible: keep.checked
            Layout.fillWidth: true
            placeholderText: "名称，例如“工作资料”"
        }
    }
    actions: [
        ActionButton { text: "取消"; foreground: dialog.theme.ink; implicitHeight: 38; onClicked: dialog.close() },
        ActionButton { objectName: "archivePasswordOk"; text: "确定"; accent: true; implicitHeight: 38; enabled: field.text !== "" && (!keep.checked || keepName.text.trim() !== ""); onClicked: dialog.submit() }
    ]
}
