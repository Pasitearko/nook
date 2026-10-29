import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The task drawer: every task of the application, newest last, above the bottom-right corner.
Popup {
    id: drawer
    objectName: "archiveTasks"
    required property var app
    readonly property var queue: app.queue
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow.theme
    readonly property bool animated: hostWindow.animations === true
    width: Math.min(420, hostWindow.layoutWidth - 32)
    height: Math.min(hostWindow.layoutHeight - 90, header.implicitHeight + Math.max(110, list.contentHeight) + 36)
    x: hostWindow.layoutWidth - width - 16
    y: hostWindow.layoutHeight - height - 44
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: drawer.animated ? 140 : 0 }
            NumberAnimation { property: "y"; from: drawer.y + 12; to: drawer.y; duration: drawer.animated ? 180 : 0; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: drawer.animated ? 90 : 0 } }
    background: Item {
        Rectangle { anchors.fill: parent; anchors.topMargin: 6; anchors.margins: -2; radius: 16; color: drawer.theme.shadow }
        Rectangle { anchors.fill: parent; radius: 14; color: drawer.theme.window; border.width: 1; border.color: drawer.theme.lineStrong }
    }
    contentItem: ColumnLayout {
        spacing: 0
        RowLayout {
            id: header
            Layout.fillWidth: true; Layout.margins: 14; Layout.bottomMargin: 8; spacing: 8
            Text { text: "任务"; color: drawer.theme.ink; font.pixelSize: 16; font.weight: Font.DemiBold }
            Text {
                text: drawer.queue.activeCount > 0 ? drawer.queue.activeCount + " 个进行中" + (drawer.queue.concurrency > 1 ? "，同时 " + drawer.queue.concurrency + " 个" : "") : ""
                color: drawer.theme.muted; font.pixelSize: 12; Layout.fillWidth: true
            }
            ActionButton { visible: drawer.queue.finishedCount > 0; text: "清除已完成"; implicitHeight: 28; foreground: drawer.theme.muted; onClicked: drawer.queue.clearFinished() }
        }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.leftMargin: 10; Layout.rightMargin: 10; Layout.bottomMargin: 10
            clip: true; spacing: 8
            boundsBehavior: Flickable.StopAtBounds
            model: drawer.queue
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ArchiveTaskCard { width: list.width; app: drawer.app }
            Text {
                anchors.centerIn: parent; visible: list.count === 0
                text: "没有任务"; color: drawer.theme.muted; font.pixelSize: 13
            }
        }
    }
}
