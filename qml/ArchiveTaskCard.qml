import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

// One task: what it does, how far it is, and what can be done with it now.
Rectangle {
    id: card
    objectName: "archiveTaskCard"
    required property var app
    required property int taskId
    required property string kind
    required property string title
    required property string detail
    required property string status
    required property string statusText
    required property real fraction
    required property string progressText
    required property string current
    required property string message
    required property bool problem
    required property var warnings
    required property bool finished
    required property bool canMoveUp
    required property bool canMoveDown
    required property bool canDeleteCreated
    required property string target
    required property bool targetIsFolder
    property bool compact: false
    property bool showWarnings: false
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property var queue: app.queue
    readonly property bool waiting: status === "password" || status === "conflict"
    readonly property color tone: status === "failed" ? theme.danger : waiting || (finished && problem) ? theme.warning : status === "done" ? theme.success : theme.accent

    implicitHeight: column.implicitHeight + 24
    radius: 10
    color: compact ? "transparent" : theme.raised
    border.width: compact ? 0 : 1; border.color: theme.line

    ColumnLayout {
        id: column
        anchors.fill: parent; anchors.margins: 12; spacing: 6
        RowLayout {
            Layout.fillWidth: true; spacing: 10
            Rectangle {
                Layout.preferredWidth: 32; Layout.preferredHeight: 32; radius: 9
                color: Qt.alpha(card.tone, card.theme.dark ? 0.18 : 0.12)
                Icon { anchors.centerIn: parent; width: 18; height: 18; tint: card.tone; name: card.kind === "extract" ? "extract" : card.kind === "create" ? "compress" : "verify" }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Text { text: card.title; color: card.theme.ink; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                Text { text: card.detail; color: card.theme.muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
            }
            ActionButton { visible: card.canMoveUp; iconName: "up"; hint: "提前"; implicitWidth: 30; implicitHeight: 30; onClicked: card.queue.move(card.taskId, -1) }
            ActionButton { visible: card.canMoveDown; iconName: "up"; rotation: 180; hint: "推后"; implicitWidth: 30; implicitHeight: 30; onClicked: card.queue.move(card.taskId, 1) }
            ActionButton {
                objectName: "archiveTaskCancel"
                visible: !card.finished && card.status !== "cancelling"
                iconName: "close"; hint: "取消"; implicitWidth: 30; implicitHeight: 30
                onClicked: card.queue.cancel(card.taskId)
            }
            ActionButton {
                visible: card.finished && !card.compact
                iconName: "close"; hint: "从列表中移除"; implicitWidth: 30; implicitHeight: 30; foreground: card.theme.muted
                onClicked: card.queue.remove(card.taskId)
            }
        }
        ArchiveProgress {
            visible: !card.finished
            Layout.fillWidth: true; Layout.topMargin: 2
            value: card.fraction
            indeterminate: card.status === "running" && card.fraction <= 0
            tone: card.tone
            opacity: card.status === "queued" ? 0.4 : 1
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text {
                text: card.finished ? card.message : card.statusText
                color: card.status === "failed" ? card.theme.danger : card.waiting ? card.theme.warning : card.theme.inkSoft
                font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true
                maximumLineCount: 3; elide: Text.ElideRight
            }
            Text { visible: !card.finished && card.progressText !== ""; text: card.progressText; color: card.theme.muted; font.pixelSize: 12 }
        }
        Text {
            visible: !card.finished && card.current !== ""
            text: card.current; color: card.theme.muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true
        }
        RowLayout {
            visible: card.finished && (card.target !== "" && card.status === "done" && card.kind !== "test" || card.canDeleteCreated || card.warnings.length > 0)
            Layout.fillWidth: true; spacing: 4
            ActionButton {
                objectName: "archiveTaskReveal"
                visible: card.status === "done" && card.target !== "" && card.kind !== "test"
                text: "打开位置"; iconName: "folder"; implicitHeight: 30; foreground: card.theme.ink
                onClicked: card.app.reveal(card.target, card.targetIsFolder)
            }
            ActionButton {
                objectName: "archiveTaskDeleteCreated"
                visible: card.canDeleteCreated
                text: "删除本次已解压的内容"; iconName: "trash"; implicitHeight: 30; foreground: card.theme.ink
                hint: "把这次解压出的项目移到回收站"
                onClicked: card.queue.deleteCreated(card.taskId)
            }
            ActionButton {
                visible: card.warnings.length > 0
                text: card.showWarnings ? "收起详情" : "详情（" + card.warnings.length + "）"; implicitHeight: 30; foreground: card.theme.ink
                onClicked: card.showWarnings = !card.showWarnings
            }
            Item { Layout.fillWidth: true }
        }
        Rectangle {
            visible: card.showWarnings && card.warnings.length > 0
            Layout.fillWidth: true; Layout.preferredHeight: Math.min(140, warningText.implicitHeight + 16)
            radius: 8; color: card.theme.sunken; clip: true
            Flickable {
                anchors.fill: parent; anchors.margins: 8
                contentHeight: warningText.implicitHeight; boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Text { id: warningText; width: parent.width; text: card.warnings.join("\n"); color: card.theme.inkSoft; font.pixelSize: 12; wrapMode: Text.WrapAnywhere }
            }
        }
    }
}
