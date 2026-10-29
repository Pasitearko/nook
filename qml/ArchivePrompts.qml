import QtQuick

// Answers the questions of the tasks this window owns, one at a time: passwords and names
// that already exist. Any window with tasks of its own has one.
Item {
    id: prompts
    required property var app
    required property var host
    readonly property var queue: app.queue
    property var waiting: []
    property int current: 0
    readonly property bool busy: passwordDialog.visible || conflictDialog.visible

    function offer(id) {
        if (!queue.ownedBy(id, host)) return
        if (waiting.indexOf(id) < 0 && current !== id) waiting = waiting.concat([id])
        Qt.callLater(next)
    }
    function next() {
        if (busy || current !== 0 || waiting.length === 0) return
        const id = waiting[0]
        waiting = waiting.slice(1)
        const task = queue.task(id)
        if (task.status === "password") {
            current = id
            passwordDialog.archiveName = task.archiveName
            passwordDialog.suggestedName = task.archiveStem
            passwordDialog.wrong = task.passwordWrong
            passwordDialog.open()
        } else if (task.status === "conflict") {
            current = id
            conflictDialog.conflict = task.conflict
            conflictDialog.adding = task.kind === "add"
            conflictDialog.open()
        } else {
            Qt.callLater(next)
        }
    }
    function done() { current = 0; Qt.callLater(next) }
    Component.onCompleted: { for (const id of queue.waitingFor(host)) offer(id) }

    Connections {
        target: prompts.queue
        function onAttention(id) { prompts.offer(id) }
        function onTaskFinished(id) {
            // Cancelled from the task list while its question was open.
            if (id !== prompts.current) return
            prompts.current = 0
            passwordDialog.answered = true; passwordDialog.close()
            conflictDialog.answered = true; conflictDialog.close()
            Qt.callLater(prompts.next)
        }
    }
    ArchivePasswordDialog {
        id: passwordDialog
        objectName: "archiveTaskPasswordDialog"
        parent: prompts.host.contentItem
        app: prompts.app
        onSubmitted: (password, saveName) => { prompts.queue.submitPassword(prompts.current, password, saveName); prompts.done() }
        onDeclined: { prompts.queue.cancel(prompts.current); prompts.done() }
    }
    ArchiveConflictDialog {
        id: conflictDialog
        parent: prompts.host.contentItem
        app: prompts.app
        onResolved: (choice, all) => { prompts.queue.resolveConflict(prompts.current, choice, all); prompts.done() }
        onStopRequested: { prompts.queue.cancel(prompts.current); prompts.done() }
    }
}
