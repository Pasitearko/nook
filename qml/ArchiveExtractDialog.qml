import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "解压到…": where, what (everything or the selection), whether a folder is made for it, and
// what happens to names that already exist. The last choices are remembered.
NookDialog {
    id: dialog
    objectName: "archiveExtractDialog"
    required property var app
    required property var document
    property int selectedCount: 0
    readonly property var prefs: app.prefs
    readonly property var subfolders: [
        {value: "smart", label: "需要时新建文件夹", note: "压缩包里有多个顶层项目时，放进“" + stem + "”文件夹"},
        {value: "always", label: "总是新建“" + stem + "”文件夹", note: ""},
        {value: "never", label: "直接放进目标文件夹", note: ""}]
    readonly property var conflicts: [
        {value: "ask", label: "每次询问"}, {value: "overwrite", label: "替换已有的"},
        {value: "skip", label: "跳过，保留已有的"}, {value: "rename", label: "保留两者（新的加编号）"}]
    readonly property string stem: document.name.replace(/\.(tar\.)?[^.]+$/, "").replace(/\.part0*1$/i, "") || "解压"
    signal extract(var choices)

    property bool forSelection: false
    function openFor(selection) { forSelection = selection && selectedCount > 0; open() }
    function chosen(path) { if (path) destination.text = path }
    function start() {
        if (destination.text.trim() === "") return
        const choices = {
            destination: destination.text.trim(),
            selection: scope.currentIndex === 1,
            subfolder: subfolders[subfolderBox.currentIndex].value,
            conflict: conflicts[conflictBox.currentIndex].value,
            openAfter: openAfter.checked
        }
        app.setPref("subfolder", choices.subfolder)
        app.setPref("conflict", choices.conflict)
        app.setPref("openAfter", choices.openAfter)
        close()
        extract(choices)
    }
    iconName: "extract"
    title: "解压“" + document.name + "”"
    message: ""
    wrapAnywhere: true
    width: Math.min(520, windowWidth - 40)
    onAboutToShow: {
        destination.text = document.folderPath
        scope.currentIndex = forSelection ? 1 : 0
        subfolderBox.currentIndex = Math.max(0, subfolders.findIndex(s => s.value === prefs.subfolder))
        conflictBox.currentIndex = Math.max(0, conflicts.findIndex(c => c.value === prefs.conflict))
        openAfter.checked = prefs.openAfter === true
    }
    onOpened: destination.forceActiveFocus()
    onAccepted: start()

    Text { text: "解压到"; color: dialog.theme.muted; font.pixelSize: 13 }
    RowLayout {
        Layout.fillWidth: true; spacing: 6
        ArchiveField { id: destination; objectName: "archiveExtractDestination"; Layout.fillWidth: true; onAccepted: dialog.start() }
        ActionButton { text: "浏览…"; foreground: dialog.theme.ink; implicitHeight: 34; onClicked: dialog.app.choose(dialog, "destination", destination.text) }
    }
    RowLayout {
        Layout.fillWidth: true; spacing: 4
        ActionButton {
            text: "压缩包所在文件夹"; implicitHeight: 28; foreground: dialog.theme.muted
            active: destination.text === dialog.document.folderPath
            onClicked: destination.text = dialog.document.folderPath
        }
        Item { Layout.fillWidth: true }
    }
    GridLayout {
        Layout.fillWidth: true; Layout.topMargin: 4; columns: 2; columnSpacing: 12; rowSpacing: 8
        Text { visible: dialog.selectedCount > 0; text: "内容"; color: dialog.theme.muted; font.pixelSize: 13 }
        NookComboBox {
            id: scope
            visible: dialog.selectedCount > 0
            Layout.fillWidth: true
            model: ["整个压缩包", "选中的 " + dialog.selectedCount + " 项"]
        }
        Text { text: "文件夹"; color: dialog.theme.muted; font.pixelSize: 13 }
        NookComboBox { id: subfolderBox; objectName: "archiveExtractSubfolder"; Layout.fillWidth: true; model: dialog.subfolders.map(s => s.label) }
        Item { width: 1; height: 1; visible: subfolderNote.visible }
        Text { id: subfolderNote; visible: text !== ""; text: dialog.subfolders[subfolderBox.currentIndex].note; color: dialog.theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
        Text { text: "同名项目"; color: dialog.theme.muted; font.pixelSize: 13 }
        NookComboBox { id: conflictBox; objectName: "archiveExtractConflict"; Layout.fillWidth: true; model: dialog.conflicts.map(c => c.label) }
    }
    ArchiveCheck { id: openAfter; text: "完成后打开所在位置" }
    Connections {
        target: dialog.app
        function onChosen(requester, purpose, paths) { if (requester === dialog && paths.length) dialog.chosen(paths[0]) }
    }
    actions: [
        ActionButton { text: "取消"; foreground: dialog.theme.ink; implicitHeight: 38; onClicked: dialog.close() },
        ActionButton { objectName: "archiveExtractStart"; text: "解压"; accent: true; implicitHeight: 38; enabled: destination.text.trim() !== ""; onClicked: dialog.start() }
    ]
}
