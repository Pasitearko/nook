import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A name that already exists where the files go: replace it, skip the new one, or keep both
// (the new one gets a number). "对之后的同名项目也这样处理" answers the rest of the job too.
NookDialog {
    id: dialog
    objectName: "archiveConflictDialog"
    required property var app
    property var conflict: ({})
    // Adding to a package: the existing one is inside it, the incoming one on disk.
    property bool adding: false
    readonly property var existing: conflict.existing || {}
    readonly property var incoming: conflict.incoming || {}
    readonly property string path: conflict.path || ""
    property bool answered: false
    signal resolved(string choice, bool all)
    signal stopRequested()

    function answer(choice) { answered = true; resolved(choice, all.checked); close() }
    function describe(side) {
        if (side.directory) return "文件夹"
        const when = side.mtime ? new Date(side.mtime * 1000).toLocaleString(Qt.locale(), "yyyy-MM-dd HH:mm") : ""
        return dialog.app.formatSize(side.size || 0) + (when ? " · " + when : "")
    }
    iconName: "file"
    title: "“" + path.split("/").pop() + "”已存在"
    message: path.indexOf("/") >= 0 ? "位置：" + path.substring(0, path.lastIndexOf("/"))
             : adding ? "压缩包里已有同名项目。" : "目标文件夹里已有同名项目。"
    wrapAnywhere: true
    closePolicy: Popup.NoAutoClose
    onAboutToShow: { answered = false; all.checked = false }

    GridLayout {
        Layout.fillWidth: true; columns: 2; columnSpacing: 14; rowSpacing: 6
        Text { text: dialog.adding ? "压缩包里已有的" : "已有的"; color: dialog.theme.muted; font.pixelSize: 13 }
        Text { text: dialog.describe(dialog.existing); color: dialog.theme.ink; font.pixelSize: 13; Layout.fillWidth: true }
        Text { text: dialog.adding ? "要添加的" : "压缩包里的"; color: dialog.theme.muted; font.pixelSize: 13 }
        Text {
            text: dialog.describe(dialog.incoming) + (dialog.incoming.mtime && dialog.existing.mtime && dialog.incoming.mtime > dialog.existing.mtime ? "（较新）" : "")
            color: dialog.theme.ink; font.pixelSize: 13; Layout.fillWidth: true
        }
    }
    ArchiveCheck { id: all; objectName: "archiveConflictAll"; text: "对之后的同名项目也这样处理" }
    actions: [
        ActionButton { text: "停止"; foreground: dialog.theme.ink; implicitHeight: 38; hint: "取消这个任务"; onClicked: { dialog.answered = true; dialog.stopRequested(); dialog.close() } },
        ActionButton { objectName: "archiveConflictSkip"; text: "跳过"; foreground: dialog.theme.ink; implicitHeight: 38; onClicked: dialog.answer("skip") },
        ActionButton { objectName: "archiveConflictRename"; text: "保留两者"; foreground: dialog.theme.ink; implicitHeight: 38; hint: "新的加上编号"; onClicked: dialog.answer("rename") },
        ActionButton { objectName: "archiveConflictOverwrite"; text: "替换"; accent: true; implicitHeight: 38; onClicked: dialog.answer("overwrite") }
    ]
}
