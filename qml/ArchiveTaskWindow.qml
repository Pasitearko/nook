import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A small window for one task when no package window is there for it: it asks the task's
// questions, shows its progress, and closes once the task has succeeded. Opened for a task
// that already ended (from a notification), it shows the outcome and details.
ArchiveWindow {
    id: window
    objectName: "archiveTaskWindow"
    required property int taskId
    readonly property var queue: app.queue
    property bool wasRunning: false

    // Room for a question dialog over the task.
    width: 540; height: 360
    minimumWidth: 460; minimumHeight: 300
    title: "任务 · nook解压缩"

    Component.onCompleted: {
        wasRunning = !queue.task(taskId).finished
        queue.adopt(taskId, window)
        prompts.offer(taskId) // its question may already be waiting
    }
    Connections {
        target: window.queue
        function onTaskFinished(id) {
            if (id !== window.taskId) return
            if (window.queue.task(id).status === "done" || window.queue.task(id).status === "cancelled") closeTimer.start()
        }
    }
    Timer { id: closeTimer; interval: 600; onTriggered: window.close() }
    Shortcut { sequence: "Escape"; enabled: !prompts.busy; onActivated: window.close() }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 1; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 50; Layout.leftMargin: 16; Layout.rightMargin: 8; spacing: 10
            Icon { name: "archive"; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
            Text { text: "nook解压缩"; color: window.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true }
            WindowCloseButton { hint: "关闭（任务在后台继续）"; onClicked: window.close() }
            DragHandler { target: null; onActiveChanged: if (active) window.startSystemMove() }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 12
            clip: true; interactive: false
            model: window.queue
            delegate: ArchiveTaskCard {
                readonly property bool mine: taskId === window.taskId
                width: list.width
                height: mine ? implicitHeight : 0
                visible: mine
                compact: true
                showWarnings: mine && !window.wasRunning
                app: window.app
            }
        }
    }
    ArchivePrompts { id: prompts; app: window.app; host: window }
}
