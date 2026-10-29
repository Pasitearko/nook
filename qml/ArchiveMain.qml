import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import QtQuick.Window

// nook解压缩: one package per window. Empty, it offers to open or create one; open, it browses
// the package like a folder, with the actions along the toolbar.
ArchiveWindow {
    id: window
    objectName: "archiveWindow"
    required property var document
    readonly property var model: document.model
    readonly property var queue: app.queue
    readonly property bool ready: document.state === "ready"
    readonly property bool dialogsOpen: passwordDialog.visible || infoDialog.visible || extractDialog.visible || settingsDialog.visible || prompts.busy
    property bool dropping: false
    property string pendingSave: ""

    width: 1102; height: 709
    minimumWidth: 720; minimumHeight: 460
    title: document.name !== "" ? document.name + " · nook解压缩" : "nook解压缩"

    function chooseArchives() { app.choose(window, "archives") }
    // The first package opens here unless this window already shows one; the rest get windows.
    // Dropped or chosen files: packages open, anything else goes to a new package.
    function takePaths(paths) {
        const all = paths.every(p => app.looksLikeArchive(p))
        if (all) openPaths(paths)
        else app.openCompose(paths.map(p => app.localPath(p)))
    }
    function openPaths(paths) {
        let first = true
        for (const path of paths) {
            if (first && document.state === "empty") document.open(path)
            else app.openWindow(path)
            first = false
        }
    }
    function activate(row) {
        if (model.isFolder(row)) { searchBox.clear(); model.activate(row); fileList.focusList(); return }
        toast("打开压缩包里的文件将在后续版本中支持")
    }
    function ratioText() {
        const total = model.totalSize
        if (total <= 0) return ""
        const packed = model.packedKnown ? model.packedSize : document.archiveSize
        // Tiny packages are larger than what they hold; a ratio above 100% says nothing useful.
        return packed <= total ? "压缩率 " + Math.max(1, Math.round(packed / total * 100)) + "%" : ""
    }
    function pending(what) { toast(what + "将在后续版本中推出") }
    function startTask(request, extras) {
        if (document.encodingChoice !== "auto") request.encoding = document.encodingChoice
        queue.start(request, extras || {}, window)
        tasksDrawer.open()
    }
    // choices: destination, selection, subfolder, conflict, openAfter.
    function extractWith(choices) {
        const request = {op: "extract", archive: document.path, destination: choices.destination,
                         subfolder: choices.subfolder, conflict: choices.conflict, expectedSize: model.totalSize}
        if (choices.selection && model.selectedCount > 0) {
            request.entries = model.selectedPaths()
            if (model.query === "" && model.folder !== "") request.prefix = model.folder
            delete request.expectedSize
        }
        startTask(request, {openAfter: choices.openAfter === true})
    }
    // "解压到当前文件夹": literally the folder the package is in, with the remembered folder rule.
    function extractHere(subfolder, selection) {
        extractWith({destination: document.folderPath, selection: selection === true,
                     subfolder: subfolder || app.prefs.subfolder, conflict: app.prefs.conflict, openAfter: app.prefs.openAfter})
    }
    function testArchive() { startTask({op: "test", archive: document.path}) }

    Connections {
        target: window.app
        function onChosen(requester, purpose, paths) {
            if (requester !== window || paths.length === 0) return
            if (purpose === "archives") window.openPaths(paths)
            else window.app.openCompose(paths)
        }
        function onNotice(message) { window.toast(message) }
    }
    Connections {
        target: window.document
        function onStateChanged() {
            if (window.ready) {
                Qt.callLater(fileList.focusList)
                if (window.pendingSave !== "" && window.document.password !== "") window.app.passwords.save(window.pendingSave, window.document.password)
            }
            if (window.document.state !== "loading") window.pendingSave = ""
        }
        function onPasswordChanged() {
            if (window.document.passwordRequested) passwordDialog.open()
            else if (passwordDialog.visible) passwordDialog.close()
        }
    }
    Connections {
        target: window.queue
        function onTaskFinished(id) {
            if (window.active && window.queue.ownedBy(id, window)) {
                const task = window.queue.task(id)
                if (task.status !== "cancelled") window.toast(task.message)
            }
        }
    }
    Timer { id: searchTimer; interval: 180; onTriggered: window.model.setQuery(searchBox.text) }

    Shortcut { sequence: "Ctrl+O"; onActivated: window.chooseArchives() }
    Shortcut { sequence: "Ctrl+W"; onActivated: window.close() }
    Shortcut { sequences: ["Ctrl+F"]; enabled: window.ready; onActivated: { searchBox.forceActiveFocus(); searchBox.selectAll() } }
    Shortcut { sequence: "Ctrl+I"; enabled: window.ready; onActivated: infoDialog.open() }
    Shortcut { sequence: "Ctrl+E"; enabled: window.ready && !window.dialogsOpen; onActivated: extractDialog.openFor(false) }
    Shortcut { sequence: "Ctrl+N"; enabled: !window.dialogsOpen; onActivated: window.app.choose(window, "files") }
    Shortcut { sequence: "Ctrl+,"; enabled: !window.dialogsOpen; onActivated: settingsDialog.open() }
    Shortcut { sequence: "F5"; enabled: document.path !== ""; onActivated: document.reload() }
    Shortcut { sequence: "Alt+Left"; enabled: window.ready; onActivated: model.back() }
    Shortcut { sequence: "Alt+Right"; enabled: window.ready; onActivated: model.forward() }
    Shortcut { sequence: "Alt+Up"; enabled: window.ready; onActivated: model.up() }
    Shortcut { sequence: "Backspace"; enabled: window.ready && !window.editingText && !window.dialogsOpen; onActivated: model.up() }
    Shortcut {
        sequence: "Escape"
        enabled: !window.dialogsOpen
        onActivated: { if (searchBox.text !== "") { searchBox.clear(); fileList.focusList() } else window.model.clearSelection() }
    }

    component TaskButton: AbstractButton {
        id: taskButton
        objectName: "archiveTaskButton"
        readonly property var queue: window.queue
        visible: queue.count > 0
        implicitWidth: 36; implicitHeight: 36
        hoverEnabled: true
        background: Rectangle { radius: 8; color: taskButton.down ? window.theme.pressed : taskButton.hovered || tasksDrawer.visible ? window.theme.hover : "transparent" }
        contentItem: Item {
            Shape {
                anchors.centerIn: parent; width: 22; height: 22
                preferredRendererType: Shape.CurveRenderer
                ShapePath { strokeColor: window.theme.sunken; strokeWidth: 2.4; fillColor: "transparent"; PathAngleArc { centerX: 11; centerY: 11; radiusX: 9; radiusY: 9; startAngle: 0; sweepAngle: 360 } }
                ShapePath {
                    strokeColor: taskButton.queue.attentionCount > 0 ? window.theme.warning : window.theme.accent
                    strokeWidth: 2.4; fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    PathAngleArc { centerX: 11; centerY: 11; radiusX: 9; radiusY: 9; startAngle: -90; sweepAngle: taskButton.queue.activeCount > 0 ? Math.max(8, 360 * taskButton.queue.fraction) : 360 }
                }
            }
            Text {
                anchors.centerIn: parent
                text: taskButton.queue.activeCount > 0 ? taskButton.queue.activeCount : "✓"
                color: window.ink; font.pixelSize: 10; font.weight: Font.DemiBold
            }
        }
        NookToolTip { visible: taskButton.hovered; text: taskButton.queue.activeCount > 0 ? taskButton.queue.activeCount + " 个任务进行中" : "任务列表" }
        onClicked: tasksDrawer.visible ? tasksDrawer.close() : tasksDrawer.open()
    }
    component Chip: Rectangle {
        property alias text: chipLabel.text
        property color tone: window.theme.accentText
        implicitWidth: chipLabel.implicitWidth + 18; implicitHeight: 24; radius: 12
        color: Qt.alpha(tone, window.dark ? 0.16 : 0.10)
        border.width: 1; border.color: Qt.alpha(tone, 0.35)
        Text { id: chipLabel; anchors.centerIn: parent; color: parent.tone; font.pixelSize: 12; font.weight: Font.DemiBold }
    }
    component InfoRow: RowLayout {
        property string label
        property string value
        visible: value !== ""
        Layout.fillWidth: true; spacing: 12
        Text { text: parent.label; color: window.muted; font.pixelSize: 13; Layout.preferredWidth: 92; horizontalAlignment: Text.AlignRight }
        Text { text: parent.value; color: window.ink; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
    }

    Item {
        anchors.fill: parent
        Item {
            anchors.fill: parent
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 1; spacing: 0
                // Where: history, the folder inside the package, and search.
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 50
                    Layout.leftMargin: 10; Layout.rightMargin: 8; spacing: 4
                    RowLayout {
                        visible: !window.ready
                        Layout.fillWidth: true; Layout.leftMargin: 6; spacing: 10
                        Icon { name: "archive"; width: 22; height: 22 }
                        Text { text: "nook解压缩"; color: window.ink; font.pixelSize: 15; font.weight: Font.DemiBold }
                        Text { visible: document.name !== ""; text: document.name; color: window.muted; font.pixelSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Item { visible: document.name === ""; Layout.fillWidth: true }
                    }
                    ActionButton { visible: window.ready; iconName: "back"; hint: "后退 · Alt+←"; enabled: model.canBack; foreground: window.ink; onClicked: model.back() }
                    ActionButton { visible: window.ready; iconName: "forward"; hint: "前进 · Alt+→"; enabled: model.canForward; foreground: window.ink; onClicked: model.forward() }
                    ActionButton { visible: window.ready; iconName: "up"; hint: "上一级 · Backspace / Alt+↑"; enabled: model.canUp; foreground: window.ink; onClicked: model.up() }
                    Rectangle {
                        id: address
                        visible: window.ready
                        Layout.fillWidth: true; Layout.preferredHeight: 34; Layout.leftMargin: 4
                        radius: 8; color: window.theme.field; border.width: 1; border.color: window.line
                        clip: true
                        Flickable {
                            id: crumbs
                            anchors.fill: parent; anchors.leftMargin: 5; anchors.rightMargin: 5
                            contentWidth: crumbRow.width; contentHeight: height
                            boundsBehavior: Flickable.StopAtBounds
                            onContentWidthChanged: contentX = Math.max(0, contentWidth - width)
                            Row {
                                id: crumbRow; height: parent.height; spacing: 1
                                Repeater {
                                    model: window.model.breadcrumbs
                                    Row {
                                        required property var modelData
                                        required property int index
                                        height: crumbRow.height
                                        Text { visible: index > 0; text: "›"; color: window.muted; height: parent.height; verticalAlignment: Text.AlignVCenter; font.pixelSize: 14; width: index > 0 ? 11 : 0 }
                                        ActionButton {
                                            height: parent.height
                                            iconName: index === 0 ? "archive" : ""
                                            text: modelData.title; foreground: window.ink; hint: modelData.path || modelData.title
                                            onClicked: { searchBox.clear(); window.model.openFolder(modelData.path) }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ArchiveField {
                        id: searchBox; objectName: "archiveSearch"
                        visible: window.ready
                        Layout.preferredWidth: Math.min(230, window.layoutWidth * 0.26); Layout.leftMargin: 4
                        placeholderText: "在压缩包中搜索"
                        leftPadding: 32; rightPadding: text ? 36 : 10
                        onTextChanged: { if (!inputMethodComposing) searchTimer.restart() }
                        onAccepted: { searchTimer.stop(); window.model.setQuery(text); fileList.focusList() }
                        Icon { name: "search"; width: 14; height: 14; x: 11; anchors.verticalCenter: parent.verticalCenter }
                        ActionButton { visible: searchBox.text !== ""; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; implicitWidth: 30; implicitHeight: 30; leftPadding: 7; rightPadding: 7; iconName: "close"; hint: "清除搜索"; foreground: window.muted; onClicked: searchBox.clear() }
                        Keys.onDownPressed: fileList.focusList()
                    }
                    TaskButton { Layout.leftMargin: 4 }
                    ActionButton { objectName: "archiveSettingsButton"; iconName: "settings"; hint: "设置 · Ctrl+,"; foreground: window.ink; onClicked: settingsDialog.open() }
                    Rectangle { width: 1; height: 18; color: window.line; Layout.leftMargin: 4; Layout.rightMargin: 2 }
                    WindowCloseButton { objectName: "archiveClose"; hint: "关闭窗口 · Ctrl+W"; onClicked: window.close() }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: window.line; opacity: 0.7 }
                // What: the actions on the package, then how it is read.
                RowLayout {
                    visible: window.ready
                    Layout.fillWidth: true; Layout.preferredHeight: 48
                    Layout.leftMargin: 10; Layout.rightMargin: 12; spacing: 4
                    ActionButton { objectName: "archiveExtract"; text: "解压到…"; iconName: "extract"; accent: true; hint: "解压全部或选中的项目 · Ctrl+E"; onClicked: extractDialog.openFor(false) }
                    ActionButton { objectName: "archiveExtractHere"; text: "解压到此处"; foreground: window.ink; hint: "解压到压缩包所在的文件夹“" + document.folderPath + "”"; onClicked: window.extractHere() }
                    ActionButton { objectName: "archiveTest"; text: "测试"; iconName: "verify"; foreground: window.ink; hint: "检查压缩包是否完整、密码是否正确"; onClicked: window.testArchive() }
                    Rectangle { width: 1; height: 18; color: window.line; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                    ActionButton { text: "添加"; iconName: "add"; foreground: window.ink; enabled: !document.readOnly; hint: document.readOnly ? "这种格式只能查看和解压" : "向压缩包添加文件"; onClicked: window.pending("添加文件") }
                    ActionButton { text: "删除"; iconName: "trash"; foreground: window.ink; enabled: !document.readOnly && model.selectedCount > 0; hint: "从压缩包中删除选中的项目"; onClicked: window.pending("删除") }
                    ActionButton { text: "重命名"; iconName: "rename"; foreground: window.ink; enabled: !document.readOnly && model.selectedCount === 1; hint: "重命名选中的项目"; onClicked: window.pending("重命名") }
                    ActionButton { objectName: "archiveInfo"; text: "信息"; iconName: "info"; foreground: window.ink; hint: "压缩包信息 · Ctrl+I"; onClicked: infoDialog.open() }
                    Item { Layout.fillWidth: true }
                    Text { visible: encodingBox.visible; text: "文件名编码"; color: window.muted; font.pixelSize: 13 }
                    NookComboBox {
                        id: encodingBox; objectName: "archiveEncoding"
                        // Only names that are not UTF-8 can be read another way.
                        visible: document.legacyNames || document.encodingChoice !== "auto"
                        Layout.preferredWidth: 206; Layout.preferredHeight: 32
                        readonly property var choices: window.app.encodings
                        readonly property string detected: document.encoding === "GB18030" ? "GBK" : document.encoding
                        model: choices.map(c => c.value === "auto" && document.encoding !== "" ? "自动识别（" + detected + "）" : c.label)
                        currentIndex: Math.max(0, choices.findIndex(c => c.value === document.encodingChoice))
                        onActivated: document.setEncoding(choices[currentIndex].value)
                    }
                    Chip { objectName: "archiveFormat"; text: document.formatLabel; visible: text !== ""; Layout.leftMargin: 6 }
                    Chip { visible: document.readOnly; text: "只读"; tone: window.muted }
                }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.leftMargin: 10; Layout.rightMargin: 10; Layout.bottomMargin: window.ready ? 0 : 10
                    ArchiveList {
                        id: fileList
                        objectName: "archiveList"
                        anchors.fill: parent
                        visible: window.ready
                        model: window.model
                        onActivated: row => window.activate(row)
                        onContextRequested: (row, x, y) => { contextMenu.row = row; contextMenu.popup(fileList, x, y) }
                    }
                    // Nothing open: a place to drop packages, the two ways in, and recent ones.
                    Item {
                        id: emptyState
                        objectName: "archiveEmpty"
                        anchors.fill: parent
                        visible: document.state === "empty"
                        Shape {
                            anchors.fill: parent
                            ShapePath {
                                strokeColor: window.dropping ? window.theme.accent : window.theme.lineStrong
                                strokeWidth: window.dropping ? 2 : 1.4
                                strokeStyle: ShapePath.DashLine
                                dashPattern: [5, 4]
                                fillColor: window.dropping ? Qt.alpha(window.theme.accent, 0.06) : "transparent"
                                startX: 14; startY: 0
                                PathLine { x: emptyState.width - 14; y: 0 }
                                PathArc { x: emptyState.width; y: 14; radiusX: 14; radiusY: 14 }
                                PathLine { x: emptyState.width; y: emptyState.height - 14 }
                                PathArc { x: emptyState.width - 14; y: emptyState.height; radiusX: 14; radiusY: 14 }
                                PathLine { x: 14; y: emptyState.height }
                                PathArc { x: 0; y: emptyState.height - 14; radiusX: 14; radiusY: 14 }
                                PathLine { x: 0; y: 14 }
                                PathArc { x: 14; y: 0; radiusX: 14; radiusY: 14 }
                            }
                        }
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(560, parent.width - 48)
                            spacing: 0
                            Icon { name: "archive"; Layout.preferredWidth: 64; Layout.preferredHeight: 64; Layout.alignment: Qt.AlignHCenter }
                            Text { text: window.dropping ? "松开即可" : "把压缩包拖到这里"; color: window.ink; font.pixelSize: 20; font.weight: Font.DemiBold; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 16 }
                            Text { text: "ZIP、7z、RAR、tar 及常见压缩格式，中文文件名自动识别编码；拖入其他文件则新建压缩包"; color: window.muted; font.pixelSize: 13; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 22; spacing: 10
                                ActionButton { objectName: "archiveOpen"; text: "打开压缩包…"; iconName: "extract"; accent: true; implicitHeight: 40; hint: "Ctrl+O"; onClicked: window.chooseArchives() }
                                ActionButton { objectName: "archiveCreate"; text: "新建压缩包…"; iconName: "compress"; foreground: window.ink; implicitHeight: 40; hint: "选择要压缩的文件 · Ctrl+N（也可以直接把文件拖进来）"; onClicked: window.app.choose(window, "files") }
                            }
                            Rectangle {
                                objectName: "archiveRecent"
                                visible: window.app.recent.length > 0
                                Layout.fillWidth: true; Layout.topMargin: 30
                                Layout.preferredHeight: recentColumn.implicitHeight + 16
                                radius: 12; color: window.theme.raised; border.width: 1; border.color: window.line
                                ColumnLayout {
                                    id: recentColumn
                                    anchors.fill: parent; anchors.margins: 8; spacing: 2
                                    RowLayout {
                                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.bottomMargin: 2
                                        Text { text: "最近打开"; color: window.muted; font.pixelSize: 13; font.weight: Font.DemiBold; Layout.fillWidth: true }
                                        ActionButton { text: "清除"; implicitHeight: 26; foreground: window.muted; hint: "清除最近打开的记录"; onClicked: window.app.clearRecent() }
                                    }
                                    Repeater {
                                        model: window.app.recent.slice(0, 6)
                                        ItemDelegate {
                                            id: recentItem
                                            required property var modelData
                                            Layout.fillWidth: true; implicitHeight: 44
                                            hoverEnabled: true
                                            opacity: modelData.exists ? 1 : 0.5
                                            background: Rectangle { radius: 8; color: recentItem.hovered ? window.theme.hover : "transparent" }
                                            contentItem: RowLayout {
                                                spacing: 10
                                                Icon { name: "archive"; width: 22; height: 22 }
                                                ColumnLayout {
                                                    Layout.fillWidth: true; spacing: 1
                                                    Text { text: recentItem.modelData.name; color: window.ink; font.pixelSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                                    Text { text: recentItem.modelData.exists ? recentItem.modelData.folder : "已不存在"; color: window.muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                                }
                                                Text { text: recentItem.modelData.sizeText; color: window.muted; font.pixelSize: 12 }
                                            }
                                            onClicked: {
                                                if (modelData.exists) window.document.open(modelData.path)
                                                else { window.app.forgetRecent(modelData.path); window.toast("“" + modelData.name + "”已不存在") }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ColumnLayout {
                        objectName: "archiveLoading"
                        anchors.centerIn: parent
                        visible: document.state === "loading"
                        spacing: 12
                        BusyIndicator { running: parent.visible; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 40; Layout.preferredHeight: 40 }
                        Text {
                            text: document.loaded > 0 ? "正在读取 " + document.loaded.toLocaleString() + " 项…" : "正在打开“" + document.name + "”…"
                            color: window.muted; font.pixelSize: 14; Layout.alignment: Qt.AlignHCenter
                        }
                        ActionButton { text: "取消"; foreground: window.ink; Layout.alignment: Qt.AlignHCenter; onClicked: document.close() }
                    }
                    ColumnLayout {
                        objectName: "archiveError"
                        anchors.centerIn: parent
                        width: Math.min(520, parent.width - 48)
                        visible: document.state === "error"
                        spacing: 0
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            width: 56; height: 56; radius: 16
                            color: window.theme.dangerSoft; border.width: 1; border.color: Qt.alpha(window.theme.danger, 0.35)
                            Icon { anchors.centerIn: parent; name: document.errorCode === "cancelled" ? "lock" : "archive"; width: 28; height: 28; tint: window.theme.danger }
                        }
                        Text { text: "无法打开“" + document.name + "”"; color: window.ink; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 16; elide: Text.ElideMiddle; Layout.maximumWidth: parent.width }
                        Text { text: document.error; color: window.muted; font.pixelSize: 13; Layout.fillWidth: true; Layout.topMargin: 8; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap }
                        RowLayout {
                            Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 20; spacing: 10
                            ActionButton { text: "重试"; iconName: "refresh"; foreground: window.ink; onClicked: document.reload() }
                            ActionButton { text: "打开其他压缩包…"; accent: true; onClicked: window.chooseArchives() }
                        }
                    }
                }
                // How much: contents, ratio, selection.
                RowLayout {
                    visible: window.ready
                    Layout.fillWidth: true; Layout.preferredHeight: 34
                    Layout.leftMargin: 18; Layout.rightMargin: 18; spacing: 14
                    Text {
                        objectName: "archiveSummary"
                        text: model.fileCount.toLocaleString() + " 个文件" + (model.folderCount > 0 ? "、" + model.folderCount.toLocaleString() + " 个文件夹" : "")
                              + " · 解压后 " + app.formatSize(model.totalSize) + " · 压缩包 " + app.formatSize(document.archiveSize)
                              + (window.ratioText() !== "" ? " · " + window.ratioText() : "")
                        color: window.muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
                    }
                    Text { text: model.selectionText; color: window.theme.accentText; font.pixelSize: 12 }
                }
            }
        }
        DropArea {
            anchors.fill: parent
            keys: ["text/uri-list"]
            onEntered: drag => { window.dropping = drag.hasUrls }
            onExited: window.dropping = false
            onDropped: drop => {
                window.dropping = false
                const paths = []
                for (const url of drop.urls) {
                    const uri = String(url)
                    if (uri.startsWith("file:") && window.app.exists(uri)) paths.push(uri)
                }
                if (paths.length) { window.takePaths(paths); drop.accept(Qt.CopyAction) }
                else window.toast("只能拖入本机的文件和文件夹")
            }
        }
    }

    NookMenu {
        id: contextMenu
        property int row: -1
        NookMenuItem { text: "打开"; iconName: "folder"; visible: window.model.isFolder(contextMenu.row); onTriggered: window.activate(contextMenu.row) }
        NookMenuItem { text: "解压选中的项目到…"; iconName: "extract"; onTriggered: extractDialog.openFor(true) }
        NookMenuItem { text: "解压选中的项目到当前文件夹"; onTriggered: window.extractHere("", true) }
        NookMenuItem { text: "解压选中的项目到“" + extractDialog.stem + "”文件夹"; onTriggered: window.extractHere("always", true) }
        NookMenuItem { text: "全选"; iconName: "select-all"; shortcutText: "Ctrl+A"; onTriggered: window.model.selectAll() }
        NookMenuItem { text: "压缩包信息"; iconName: "info"; shortcutText: "Ctrl+I"; onTriggered: infoDialog.open() }
    }

    ArchivePasswordDialog {
        id: passwordDialog
        app: window.app
        archiveName: window.document.name
        suggestedName: extractDialog.stem
        wrong: window.document.passwordWrong
        listing: true
        onSubmitted: (password, saveName) => { window.pendingSave = saveName; window.document.submitPassword(password) }
        onDeclined: if (window.document.passwordRequested) window.document.cancelPassword()
    }

    ArchiveExtractDialog {
        id: extractDialog
        app: window.app
        document: window.document
        selectedCount: window.model.selectedCount
        onExtract: choices => window.extractWith(choices)
    }
    ArchiveSettings { id: settingsDialog; app: window.app }
    ArchiveTasks { id: tasksDrawer; app: window.app }
    ArchivePrompts { id: prompts; app: window.app; host: window }

    NookDialog {
        id: infoDialog
        objectName: "archiveInfoDialog"
        iconName: "info"
        title: "压缩包信息"
        message: window.document.name
        wrapAnywhere: true
        readonly property var info: window.document.info
        InfoRow { label: "位置"; value: window.document.folderPath }
        InfoRow { label: "格式"; value: window.document.formatLabel + (window.document.readOnly ? "（只读）" : "") }
        InfoRow { label: "压缩包大小"; value: window.app.formatSize(window.document.archiveSize) }
        InfoRow { label: "内容"; value: window.model.fileCount.toLocaleString() + " 个文件，" + window.model.folderCount.toLocaleString() + " 个文件夹" }
        InfoRow { label: "解压后大小"; value: window.app.formatSize(window.model.totalSize) }
        InfoRow { label: "压缩率"; value: window.ratioText().replace("压缩率 ", "") }
        InfoRow { label: "加密"; value: infoDialog.info.headerEncrypted ? "内容和文件名都已加密" : infoDialog.info.encrypted ? "内容已加密，文件名可见" : "未加密" }
        InfoRow { label: "文件名编码"; value: window.document.legacyNames || window.document.encodingChoice !== "auto" ? window.document.encoding : "" }
        InfoRow { label: "分卷"; value: infoDialog.info.split ? (infoDialog.info.volumes || []).length + " 个分卷" : "" }
        InfoRow { label: "读取方式"; value: infoDialog.info.backend === "7z" ? "7-Zip" : infoDialog.info.backend === "libarchive" ? "libarchive" : "" }
        actions: [
            ActionButton { text: "在文件夹中显示"; foreground: window.ink; implicitHeight: 38; onClicked: window.app.showInFolder(window.document.path) },
            ActionButton { text: "关闭"; accent: true; implicitHeight: 38; onClicked: infoDialog.close() }
        ]
    }
}
