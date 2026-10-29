import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The small progress window: what nook's right-click menu shows while it extracts or compresses,
// and what a task gets when the window that started it has closed. It asks the tasks' questions,
// closes by itself once they have all succeeded (a notification says where the results are) and
// stays open on a failure. Opened for a task that already ended (from a notification), it shows
// the outcome and its details.
ArchiveWindow {
    id: window
    objectName: "archiveTaskWindow"
    property var taskIds: []
    readonly property var queue: app.queue
    property bool wasRunning: false
    readonly property int shown: taskIds.length

    // Room for a question dialog over the tasks.
    width: 540; height: Math.min(560, 260 + Math.max(0, shown - 1) * 92)
    minimumWidth: 460; minimumHeight: 300
    title: "任务 · nook解压缩"

    onTaskIdsChanged: takeTasks()
    Component.onCompleted: takeTasks()
    function takeTasks() {
        for (const id of taskIds) {
            if (!queue.task(id).finished) wasRunning = true
            queue.adopt(id, window)
            prompts.offer(id) // a question may already be waiting
        }
        checkFinished()
    }
    function checkFinished() {
        if (taskIds.length === 0 || !wasRunning) return
        const states = taskIds.map(id => queue.task(id).status)
        if (states.every(s => s === "done" || s === "cancelled")) closeTimer.start()
    }
    Connections {
        target: window.queue
        function onTaskFinished(id) { if (window.taskIds.indexOf(id) >= 0) window.checkFinished() }
    }
    Timer { id: closeTimer; interval: 600; onTriggered: window.close() }
    Shortcut { sequence: "Escape"; enabled: !prompts.busy; onActivated: window.close() }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 1; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 50; Layout.leftMargin: 16; Layout.rightMargin: 8; spacing: 10
            Icon { name: "archive"; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
            Text {
                text: window.shown > 1 ? "nook解压缩 · " + window.shown + " 个任务" : "nook解压缩"
                color: window.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true
            }
            WindowCloseButton { hint: "关闭（任务在后台继续）"; onClicked: window.close() }
            DragHandler { target: null; onActiveChanged: if (active) window.startSystemMove() }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 12
            clip: true; boundsBehavior: Flickable.StopAtBounds
            model: window.queue
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: ArchiveTaskCard {
                readonly property bool mine: window.taskIds.indexOf(taskId) >= 0
                width: list.width
                height: mine ? implicitHeight + (window.shown > 1 ? 8 : 0) : 0
                visible: mine
                compact: window.shown === 1
                showWarnings: mine && !window.wasRunning
                app: window.app
            }
        }
    }
    ArchivePrompts { id: prompts; app: window.app; host: window }
}
