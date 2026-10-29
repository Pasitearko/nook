import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The compact compression window: what goes in, the name and format, where it is saved, how
// hard to compress, a password and volumes. After "压缩" it follows its task(s) and closes on
// success (the notification says where the package is); it stays open to ask and on failure.
ArchiveWindow {
    id: window
    objectName: "archiveCompose"
    property var items: []
    property var taskIds: []
    readonly property var queue: app.queue
    readonly property var choicesSaved: app.prefs
    readonly property var defaults: app.compressDefaults(items)
    readonly property var formats: app.formats.filter(f => !f.single || singleFiles)
    readonly property var format: formats[Math.max(0, formatBox.currentIndex)] || app.formats[0]
    // Single-file formats need each package to hold exactly one file.
    readonly property bool singleFiles: items.length > 0 && items.every(p => app.isFile(p)) && (items.length === 1 || separate.checked)
    readonly property bool working: taskIds.length > 0
    readonly property string suffix: format.suffix
    readonly property string outputName: (format.single && !separateMode ? defaults.fileName : nameField.text.trim()) + suffix
    readonly property bool separateMode: separate.checked && items.length > 1
    readonly property var levels: ["仅存储", "最快", "快速", "标准", "较好", "最好"]
    readonly property var volumes: [{label: "不分卷", bytes: 0}, {label: "100 MB", bytes: 100 * 1048576},
                                    {label: "700 MB（CD）", bytes: 700 * 1048576}, {label: "1 GB", bytes: 1073741824},
                                    {label: "4 GB（FAT32 / 部分网盘）", bytes: 4095 * 1048576}, {label: "自定义…", bytes: -1}]
    readonly property string problem: {
        if (items.length === 0) return "没有要压缩的项目"
        if (!separateMode && !format.single && nameField.text.trim() === "") return "请输入压缩包名称"
        if (/[\/\0]/.test(nameField.text)) return "名称里不能有“/”"
        if (folderField.text.trim() === "") return "请选择保存位置"
        if (format.encrypts && password.text !== "" && password.text !== confirm.text && !password.revealed) return "两次输入的密码不一致"
        if (format.volumes && volumes[volumeBox.currentIndex].bytes < 0 && !(Number(customVolume.text) > 0)) return "请输入分卷大小（MB）"
        return ""
    }

    width: 600; height: 660
    minimumWidth: 520; minimumHeight: 420
    title: "新建压缩包 · nook解压缩"

    function addItems(paths) {
        const next = items.slice()
        for (const p of paths) if (next.indexOf(p) < 0) next.push(p)
        items = next
    }
    function removeItem(index) { const next = items.slice(); next.splice(index, 1); items = next }
    function start() {
        if (problem !== "" || working) return
        const volume = volumes[volumeBox.currentIndex].bytes
        const choices = {
            name: nameField.text.trim(), folder: folderField.text.trim(), format: format.value,
            levelStep: levelBox.currentIndex, password: format.encrypts ? password.text : "",
            encryptNames: encryptNames.checked, zipCrypto: zipCrypto.checked,
            volumeSize: format.volumes ? (volume < 0 ? Math.round(Number(customVolume.text) * 1048576) : volume) : 0,
            separate: separateMode, testAfter: testAfter.checked
        }
        app.setPref("format", choices.format)
        app.setPref("levelStep", choices.levelStep)
        app.setPref("testAfter", choices.testAfter)
        if (format.value === "7z") app.setPref("encryptNames", choices.encryptNames)
        if (format.value === "zip") app.setPref("zipCrypto", choices.zipCrypto)
        const extras = {deleteSources: deleteSources.checked, closesOnSuccess: true,
                        saveName: keep.checked && choices.password !== "" ? keepName.text.trim() : ""}
        const ids = []
        for (const request of app.compressRequests(items, choices)) ids.push(queue.start(request, extras, window))
        taskIds = ids
    }
    function checkFinished() {
        if (!working) return
        const states = taskIds.map(id => queue.task(id).status)
        if (states.every(s => s === "done")) closeTimer.start()
        else if (states.every(s => s === "done" || s === "failed" || s === "cancelled") && states.every(s => s !== "failed")) close()
    }
    Timer { id: closeTimer; interval: 500; onTriggered: window.close() }
    Connections {
        target: window.queue
        function onTaskFinished(id) { if (window.taskIds.indexOf(id) >= 0) window.checkFinished() }
        function onChanged() { if (window.taskIds.length > 0 && window.taskIds.every(id => !window.queue.task(id).taskId)) window.close() }
    }
    Connections {
        target: window.app
        function onChosen(requester, purpose, paths) {
            if (requester !== window || paths.length === 0) return
            if (purpose === "save") folderField.text = paths[0]
            else window.addItems(paths)
        }
    }
    Component.onCompleted: {
        nameField.text = defaults.name || ""
        folderField.text = defaults.folder || ""
        formatBox.currentIndex = Math.max(0, formats.findIndex(f => f.value === choicesSaved.format))
        levelBox.currentIndex = choicesSaved.levelStep
        encryptNames.checked = choicesSaved.encryptNames === true
        zipCrypto.checked = choicesSaved.zipCrypto === true
        testAfter.checked = choicesSaved.testAfter === true
        nameField.forceActiveFocus(); nameField.selectAll()
    }
    Shortcut { sequence: "Escape"; enabled: !prompts.busy; onActivated: window.close() }
    Shortcut { sequences: ["Ctrl+Return", "Ctrl+Enter"]; enabled: !window.working; onActivated: window.start() }

    component FormLabel: Text { color: window.muted; font.pixelSize: 13; Layout.alignment: Qt.AlignRight | Qt.AlignVCenter; horizontalAlignment: Text.AlignRight; Layout.preferredWidth: 72 }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 1; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 64; Layout.leftMargin: 18; Layout.rightMargin: 8; spacing: 12
            Rectangle {
                Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: 11
                color: window.theme.accentSoft; border.width: 1; border.color: Qt.alpha(window.theme.accent, 0.35)
                Icon { anchors.centerIn: parent; name: "compress"; width: 20; height: 20; tint: window.theme.accentText }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Text { text: window.working ? "正在压缩" : "新建压缩包"; color: window.ink; font.pixelSize: 17; font.weight: Font.DemiBold }
                Text {
                    text: window.items.length === 1 ? window.items[0].split("/").pop() : window.items.length + " 个项目"
                    color: window.muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true
                }
            }
            WindowCloseButton { hint: window.working ? "关闭窗口（任务在后台继续）" : "关闭 · Esc"; onClicked: window.close() }
            DragHandler { target: null; onActiveChanged: if (active) window.startSystemMove() }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }

        // The form.
        Flickable {
            id: form
            visible: !window.working
            Layout.fillWidth: true; Layout.fillHeight: true
            contentHeight: formColumn.implicitHeight + 28; clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ColumnLayout {
                id: formColumn
                x: 18; y: 14; width: form.width - 36; spacing: 12
                // What goes in.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(4, window.items.length) * 30 + 44
                    radius: 10; color: window.theme.pane; border.width: 1; border.color: window.line
                    ColumnLayout {
                        anchors.fill: parent; anchors.margins: 6; spacing: 2
                        ListView {
                            id: itemList
                            objectName: "archiveComposeItems"
                            Layout.fillWidth: true; Layout.fillHeight: true
                            clip: true; boundsBehavior: Flickable.StopAtBounds
                            model: window.items
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            delegate: RowLayout {
                                required property string modelData
                                required property int index
                                width: itemList.width; height: 30; spacing: 8
                                Icon { name: window.app.isFile(modelData) ? "file" : "folder"; Layout.preferredWidth: 18; Layout.preferredHeight: 18; Layout.leftMargin: 6 }
                                Text { text: modelData.split("/").pop(); color: window.ink; font.pixelSize: 13; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                ActionButton { iconName: "close"; hint: "不压缩这一项"; implicitWidth: 26; implicitHeight: 26; leftPadding: 5; rightPadding: 5; foreground: window.muted; onClicked: window.removeItem(index) }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 2
                            ActionButton { text: "添加文件…"; iconName: "add"; implicitHeight: 28; foreground: window.ink; onClicked: window.app.choose(window, "files", folderField.text) }
                            ActionButton { text: "添加文件夹…"; iconName: "folder"; implicitHeight: 28; foreground: window.ink; onClicked: window.app.choose(window, "folders", folderField.text) }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
                GridLayout {
                    Layout.fillWidth: true; columns: 2; columnSpacing: 12; rowSpacing: 10
                    FormLabel { text: "名称" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        ArchiveField {
                            id: nameField; objectName: "archiveComposeName"
                            Layout.fillWidth: true
                            enabled: !window.separateMode && !window.format.single
                            text: ""
                            placeholderText: window.separateMode ? "每个项目用自己的名称" : ""
                            onAccepted: window.start()
                        }
                        Text { text: window.separateMode ? "" : window.suffix; color: window.muted; font.pixelSize: 14 }
                    }
                    FormLabel { text: "格式" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        NookComboBox {
                            id: formatBox; objectName: "archiveComposeFormat"
                            Layout.preferredWidth: 130
                            model: window.formats.map(f => f.value === "zip" || f.value === "7z" ? f.value.toUpperCase() : f.value)
                        }
                        Text { text: window.format.note; color: window.muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                    }
                    FormLabel { text: "保存到" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        ArchiveField { id: folderField; objectName: "archiveComposeFolder"; Layout.fillWidth: true }
                        ActionButton { text: "浏览…"; implicitHeight: 34; foreground: window.ink; onClicked: window.app.choose(window, "save", folderField.text) }
                    }
                    Item { width: 1; height: 1; visible: existsNote.visible }
                    Text {
                        id: existsNote
                        visible: !window.separateMode && window.outputName !== window.suffix && window.app.exists(folderField.text + "/" + window.outputName)
                        text: "“" + window.outputName + "”已存在，压缩时会询问如何处理"
                        color: window.theme.warning; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true
                    }
                    FormLabel { text: "压缩等级"; visible: window.format.levels }
                    NookComboBox {
                        id: levelBox; objectName: "archiveComposeLevel"
                        visible: window.format.levels
                        Layout.preferredWidth: 130
                        model: window.levels
                    }
                    FormLabel { text: "密码"; visible: window.format.encrypts }
                    ColumnLayout {
                        visible: window.format.encrypts
                        Layout.fillWidth: true; spacing: 8
                        ArchivePasswordField { id: password; objectName: "archiveComposePassword"; Layout.fillWidth: true; placeholderText: "不加密请留空" }
                        ArchivePasswordField {
                            id: confirm; objectName: "archiveComposeConfirm"
                            visible: password.text !== "" && !password.revealed
                            Layout.fillWidth: true; placeholderText: "再输入一次密码"
                        }
                        ArchiveCheck {
                            id: encryptNames
                            visible: window.format.value === "7z" && password.text !== ""
                            text: "同时加密文件名"
                            detail: "没有密码就看不到压缩包里有哪些文件"
                        }
                        ArchiveCheck {
                            id: zipCrypto
                            visible: window.format.value === "zip" && password.text !== ""
                            text: "使用兼容加密（ZipCrypto）"
                            detail: "Windows 资源管理器能直接打开，但安全性弱；默认使用 AES-256"
                        }
                        RowLayout {
                            visible: password.text !== ""
                            Layout.fillWidth: true; spacing: 8
                            ArchiveCheck { id: keep; text: "保存到密钥环"; enabled: window.app.passwords.available }
                            ArchiveField { id: keepName; visible: keep.checked; Layout.fillWidth: true; placeholderText: "名称"; text: nameField.text }
                        }
                    }
                    FormLabel { text: "分卷"; visible: window.format.volumes }
                    RowLayout {
                        visible: window.format.volumes
                        Layout.fillWidth: true; spacing: 8
                        NookComboBox { id: volumeBox; Layout.preferredWidth: 210; model: window.volumes.map(v => v.label) }
                        ArchiveField { id: customVolume; visible: window.volumes[volumeBox.currentIndex].bytes < 0; Layout.preferredWidth: 90; placeholderText: "MB"; validator: IntValidator { bottom: 1 } }
                        Text { visible: customVolume.visible; text: "MB"; color: window.muted; font.pixelSize: 13 }
                        Item { Layout.fillWidth: true }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 84; spacing: 8
                    ArchiveCheck {
                        id: separate; objectName: "archiveComposeSeparate"
                        visible: window.items.length > 1
                        text: "每个项目单独压缩"
                        detail: "生成 " + window.items.length + " 个压缩包，各自以项目名称命名"
                    }
                    ArchiveCheck { id: testAfter; text: "压缩后校验"; detail: "完整读一遍新压缩包，确认可以正常解压" }
                    ArchiveCheck { id: deleteSources; objectName: "archiveComposeDelete"; text: "压缩后把源文件移到回收站" }
                }
            }
        }

        // Following the task(s).
        ListView {
            id: progressList
            visible: window.working
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 14
            spacing: 0; clip: true
            model: window.queue
            delegate: ArchiveTaskCard {
                readonly property bool mine: window.taskIds.indexOf(taskId) >= 0
                width: progressList.width
                height: mine ? implicitHeight + 8 : 0
                visible: mine
                app: window.app
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.8 }
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 60; Layout.leftMargin: 18; Layout.rightMargin: 14; spacing: 8
            Text {
                text: window.working ? "可以关闭这个窗口，压缩会在后台继续" : window.problem
                color: window.working ? window.muted : window.theme.warning
                font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
            }
            ActionButton { text: window.working ? "关闭" : "取消"; foreground: window.ink; implicitHeight: 38; onClicked: window.close() }
            ActionButton {
                objectName: "archiveComposeStart"
                visible: !window.working
                text: "压缩"; iconName: "compress"; accent: true; implicitHeight: 38
                enabled: window.problem === ""
                onClicked: window.start()
            }
        }
    }
    ArchivePrompts { id: prompts; app: window.app; host: window }
}
