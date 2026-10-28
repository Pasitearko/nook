import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: panel
    required property var controller
    required property var host
    readonly property var details: controller.details
    readonly property bool dark: host.dark
    readonly property var theme: host.theme
    readonly property color ink: host.ink
    readonly property color muted: host.muted
    property int draftMode: 0
    objectName: "propertiesDialog"
    width: Math.min(590, host.layoutWidth - 36)
    height: Math.min(610, host.layoutHeight - 32)
    anchors.centerIn: parent
    modal: true; padding: 20; topPadding: 16
    header: Item {
        implicitHeight: 64
        Text { x: 22; anchors.verticalCenter: parent.verticalCenter; text: "属性"; color: panel.ink; font.pixelSize: 20; font.weight: Font.DemiBold }
        ActionButton { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; iconName: "close"; hint: "关闭"; onClicked: panel.close() }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: panel.theme.line }
    }
    background: Rectangle { radius: 16; color: panel.theme.raised; border.color: panel.theme.line }
    Overlay.modal: Rectangle { color: panel.theme.scrim }
    Connections { target: panel.controller; function onChanged() { panel.draftMode = Number(panel.details.permissions) || 0 } }
    component PropertyRow: RowLayout {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true; spacing: 14
        Text { text: label; color: panel.muted; font.pixelSize: 13; Layout.preferredWidth: 64 }
        TextEdit {
            id: valueText
            ContextMenu.menu: null
            TextMenuArea { target: valueText }
            selectionColor: panel.theme.selection; selectedTextColor: panel.theme.selectionText
            text: value; readOnly: true; selectByMouse: true; color: panel.ink; font.pixelSize: 13; wrapMode: TextEdit.WrapAnywhere; Layout.fillWidth: true }
    }
    contentItem: ScrollView {
        id: scroll
        // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
        Component.onCompleted: contentItem.acceptedButtons = Qt.NoButton
        contentWidth: availableWidth; clip: true
        ColumnLayout {
            width: scroll.availableWidth; spacing: 14
            BusyIndicator { running: panel.controller.busy; visible: running; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 30; Layout.preferredHeight: 30 }
            PropertyRow { label: "名称"; value: panel.details.name || "—" }
            PropertyRow { label: "位置"; value: panel.details.path || panel.details.uri || "—" }
            PropertyRow { label: "类型"; value: ({file: "文件", directory: "文件夹", symlink: "符号链接", special: "特殊文件"})[panel.details.type] || panel.details.type || "—" }
            PropertyRow { label: "大小"; value: panel.details.sizeText || "—" }
            PropertyRow { label: "修改时间"; value: panel.details.modifiedText || "—" }
            PropertyRow { visible: !!panel.details.createdText; label: "创建时间"; value: panel.details.createdText || "" }
            PropertyRow { label: "所有者"; value: panel.details.owner || "—" }
            PropertyRow { label: "用户组"; value: panel.details.group || "—" }
            Rectangle { height: 1; Layout.fillWidth: true; color: panel.theme.line }
            RowLayout {
                objectName: "hiddenAttributeRow"
                visible: panel.details.canHide === true
                Layout.fillWidth: true; spacing: 12
                CheckBox {
                    id: hiddenBox
                    objectName: "hiddenAttributeCheck"
                    padding: 0
                    checked: panel.details.hidden === true
                    enabled: !panel.controller.busy
                    onToggled: panel.controller.setHidden(checked)
                    indicator: Rectangle {
                        width: 20; height: 20; radius: 5
                        color: hiddenBox.checked ? panel.theme.accent : panel.theme.field
                        border.color: hiddenBox.checked ? panel.theme.accent : panel.theme.lineStrong
                        Text { anchors.centerIn: parent; text: "✓"; visible: hiddenBox.checked; color: panel.theme.accentInk; font.pixelSize: 14 }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 2
                    Text { text: "隐藏"; color: panel.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: panel.muted; font.pixelSize: 12
                        text: panel.details.hiddenByName
                              ? "名称以“.”开头，在 Linux 中总是隐藏；取消勾选将重命名为“" + String(panel.details.name || "").replace(/^\.+/, "") + "”"
                              : "记录在所在文件夹的 .hidden 列表中，不修改文件名；本应用、Nautilus、Dolphin 等均会隐藏它"
                    }
                }
            }
            Rectangle { visible: panel.details.canHide === true; height: 1; Layout.fillWidth: true; color: panel.theme.line }
            RowLayout {
                Layout.fillWidth: true
                Text { text: "权限"; color: panel.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true }
                Text { text: ("000" + panel.draftMode.toString(8)).slice(-3); color: panel.muted; font.pixelSize: 13 }
            }
            GridLayout {
                objectName: "permissionGrid"
                Layout.fillWidth: true; columns: 4; rowSpacing: 8; columnSpacing: 12
                Text { text: "" }
                Repeater { model: ["读取", "写入", "执行"]; Text { required property string modelData; text: modelData; color: panel.muted; font.pixelSize: 13; Layout.alignment: Qt.AlignHCenter } }
                Repeater {
                    model: 12
                    Item {
                        required property int index
                        Layout.fillWidth: true; implicitHeight: 30
                        Text { anchors.verticalCenter: parent.verticalCenter; visible: index % 4 === 0; text: ["所有者", "用户组", "其他人"][Math.floor(index / 4)]; color: panel.ink; font.pixelSize: 13 }
                        CheckBox {
                            objectName: "permissionBit" + index
                            visible: index % 4 !== 0
                            anchors.centerIn: parent
                            property int bit: index % 4 ? (1 << (8 - Math.floor(index / 4) * 3 - (index % 4 - 1))) : 0
                            enabled: panel.details.editable === true && !panel.controller.busy
                            checked: !!(panel.draftMode & bit)
                            onToggled: panel.draftMode = checked ? panel.draftMode | bit : panel.draftMode & ~bit
                            indicator: Rectangle {
                                width: 20; height: 20; radius: 5
                                color: parent.checked ? panel.theme.accent : panel.theme.field
                                border.color: parent.checked ? panel.theme.accent : panel.theme.lineStrong
                                Text { anchors.centerIn: parent; text: "✓"; visible: parent.parent.checked; color: panel.theme.accentInk; font.pixelSize: 14 }
                            }
                        }
                    }
                }
            }
            Text { visible: !!panel.details.permissionsError; text: panel.details.permissionsError || ""; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
            Text { visible: !!panel.details.error; text: panel.details.error || ""; color: panel.theme.danger; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }
    footer: Item {
        implicitHeight: 58
        RowLayout {
            anchors.right: parent.right; anchors.rightMargin: 20; anchors.verticalCenter: parent.verticalCenter; spacing: 8
            ActionButton { objectName: "closePropertiesButton"; text: "关闭"; onClicked: panel.close() }
            ActionButton { objectName: "applyPermissionsButton"; text: "应用权限"; accent: true; enabled: panel.details.editable === true && !panel.controller.busy && panel.draftMode !== Number(panel.details.permissions); onClicked: panel.controller.applyPermissions(panel.draftMode) }
        }
    }
}
