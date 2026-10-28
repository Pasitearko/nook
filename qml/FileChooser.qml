import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

// Other applications' Open/Save dialogs (the desktop portal's file chooser), built from the
// same file list and places as the file manager. Files are only chosen here: nothing is
// dragged, moved or deleted.
ApplicationWindow {
    id: window
    objectName: "fileChooser"
    required property var controller
    required property var request
    readonly property var app: controller
    readonly property var prefs: app.settings.values
    readonly property bool dark: prefs.theme === "dark" || (prefs.theme === "system" && app.settings.systemDark)
    readonly property Theme theme: Theme { dark: window.dark; effects: prefs.effects }
    readonly property color ink: theme.ink
    readonly property color muted: theme.muted
    readonly property color line: theme.line
    readonly property color accent: theme.accent
    readonly property real uiScale: Math.max(0.75, Math.min(1.5, Number(prefs.uiScale) || 1))
    readonly property real layoutWidth: width / uiScale
    readonly property real layoutHeight: height / uiScale

    readonly property bool saving: request.mode === "save"
    readonly property bool savingFiles: request.mode === "saveFiles"
    readonly property bool pickFolders: request.directory || savingFiles
    readonly property bool searching: searchBox.text.trim() !== ""
    readonly property var listModel: searching ? app.search.model : app.left
    property int filterIndex: request.currentFilter
    property var choiceState: ({})
    property string pendingFolder: ""
    property string toastMessage: ""
    readonly property bool gridView: prefs.chooserGrid === true

    // What FilePane and Sidebar expect from their window.
    readonly property bool dialogsOpen: newFolderDialog.visible || overwriteDialog.visible
    readonly property bool menusOpen: false
    readonly property bool selectionMode: false
    readonly property int springDelay: 700
    function dragUris(drag) { return [] }
    function startFileDrag(uris, icon) {}
    function dropHint() { return false }
    function clearDropHint() {}
    function springLoad(open) { open() }

    width: 1102; height: 709
    minimumWidth: 620; minimumHeight: 400
    visible: true
    color: "transparent"
    title: request.title || (saving ? "保存文件" : savingFiles ? "保存到文件夹" : pickFolders ? "选择文件夹" : "打开文件")
    palette.window: theme.raised
    palette.base: theme.field
    palette.text: ink
    palette.windowText: ink
    palette.buttonText: ink
    palette.button: theme.raised
    palette.highlight: theme.selection
    palette.highlightedText: ink
    palette.mid: line
    palette.placeholderText: muted

    readonly property string acceptText: request.acceptLabel || (saving || savingFiles ? "保存" : pickFolders ? "选择" : "打开")
    readonly property bool canAccept: saving ? nameField.text.trim() !== ""
                                     : savingFiles || pickFolders ? true
                                     : listModel.selectedCount > 0 || nameField.text.trim() !== ""
    readonly property bool hasTypes: request.filters.length > 0
    // What the name field shows for the selection (Open): typing anything else names a file or path.
    property string selectionText: ""

    function toast(message) { toastMessage = message; toastTimer.restart() }
    function rulesOf(index) {
        const filters = request.filters
        return index >= 0 && index < filters.length ? filters[index].rules : []
    }
    function applyFilter() {
        app.left.setFilter(rulesOf(filterIndex), pickFolders)
        app.search.model.setFilter(rulesOf(filterIndex), pickFolders)
    }
    function enter(uri) {
        searchBox.clear()
        app.navigate(uri)
        listFocus()
    }
    function listFocus() { pane.focusFiles() }
    function choiceValues() {
        let values = {}
        for (const choice of request.choices)
            values[choice.id] = choiceState[choice.id] !== undefined ? choiceState[choice.id] : choice.initial
        return values
    }
    function finish(uris) { request.accept(uris, filterIndex, choiceValues()) }
    function extensionOf(index) {
        const filters = request.filters
        return index >= 0 && index < filters.length ? filters[index].extension : ""
    }
    function accept() {
        if (!canAccept) return
        const typed = nameField.text.trim()
        if (saving) {
            // A folder's name (or path) opens it, as in other file choosers.
            const folder = request.resolve(app.left.uri, typed)
            if (request.isFolder(folder)) { nameField.text = ""; enter(folder); return }
            // As on Windows, a name without an extension gets the chosen type's.
            const extension = extensionOf(filterIndex)
            const name = extension && typed.split("/").pop().lastIndexOf(".") <= 0 ? typed + "." + extension : typed
            const target = request.resolve(app.left.uri, name)
            if (request.exists(target)) { overwriteDialog.target = target; overwriteDialog.open(); return }
            finish([target])
            return
        }
        if (savingFiles) { finish(request.targetsIn(app.left.uri)); return }
        if (typed !== "" && typed !== selectionText) {
            const target = request.resolve(app.left.uri, typed)
            if (request.isFolder(target)) {
                if (pickFolders) finish([target])
                else { nameField.text = ""; enter(target) }
                return
            }
            if (!pickFolders && request.exists(target)) { finish([target]); return }
            toast("找不到“" + typed + "”")
            return
        }
        const entries = listModel.selectedEntries()
        if (pickFolders) {
            let folders = entries.filter(e => e.folder).map(e => e.fileUri)
            if (!folders.length) folders = [app.left.uri]
            finish(request.multiple ? folders : [folders[0]])
            return
        }
        const files = entries.filter(e => !e.folder)
        if (!files.length) {
            const folder = entries.find(e => e.folder)
            if (folder) enter(folder.fileUri)
            return
        }
        finish((request.multiple ? files : [files[0]]).map(e => e.fileUri))
    }
    // Double-click or Enter in the list.
    function activate(entry) {
        if (!entry || !entry.fileUri) return
        if (entry.folder) { enter(entry.fileUri); return }
        if (saving) { nameField.text = entry.fileName; accept(); return }
        if (!pickFolders) finish([entry.fileUri])
    }

    Component.onCompleted: {
        applyFilter()
        app.left.navigate(request.currentFolder)
        if (saving) {
            nameField.text = request.currentName
            nameField.forceActiveFocus()
            // Select the name without its extension, ready to be typed over.
            const dot = nameField.text.lastIndexOf(".")
            nameField.select(0, dot > 0 ? dot : nameField.text.length)
        } else
            Qt.callLater(listFocus)
    }
    onFilterIndexChanged: {
        applyFilter()
        // Saving: the type decides the extension, as on Windows.
        const extension = extensionOf(filterIndex)
        const name = nameField.text
        if (saving && extension && name) {
            const dot = name.lastIndexOf(".")
            nameField.text = (dot > 0 ? name.slice(0, dot) : name) + "." + extension
        }
    }
    Connections { target: app; function onNotice(message) { window.toast(message) } }
    // The name field follows the selection: a file picked while saving suggests its name;
    // when opening it lists what will be opened, as on Windows.
    Connections {
        target: window.listModel
        function onSelectionChanged() {
            if (window.savingFiles) return
            const entries = window.listModel.selectedEntries()
            if (window.saving) {
                const file = entries.find(e => !e.folder)
                if (file) nameField.text = file.fileName
                return
            }
            const picked = entries.filter(e => e.folder === window.pickFolders)
            const text = picked.length === 1 ? picked[0].fileName
                       : picked.map(e => "\"" + e.fileName + "\"").join(" ")
            if (nameField.text === window.selectionText || text !== "") nameField.text = text
            window.selectionText = text
        }
    }
    Connections {
        target: app.operations
        function onFinished() {
            if (!window.pendingFolder) return
            const uri = window.pendingFolder
            window.pendingFolder = ""
            Qt.callLater(() => app.left.selectUri(uri))
        }
    }
    Timer { id: searchTimer; interval: 260; onTriggered: app.search.query(searchBox.text, app.left.uri, window.pickFolders ? "folder" : "all") }
    Timer { id: toastTimer; interval: 3500; onTriggered: window.toastMessage = "" }

    readonly property bool editingText: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    Shortcut { sequence: "Escape"; onActivated: { if (address.activeFocus) { address.text = app.left.displayPath; listFocus() } else if (searching) searchBox.clear(); else request.cancel() } }
    Shortcut { sequences: ["Ctrl+L"]; onActivated: { address.forceActiveFocus(); address.selectAll() } }
    Shortcut { sequences: ["Ctrl+F"]; onActivated: { searchBox.forceActiveFocus(); searchBox.selectAll() } }
    Shortcut { sequence: "Ctrl+H"; onActivated: app.left.showHidden = !app.left.showHidden }
    Shortcut { sequence: "Alt+Left"; onActivated: app.left.back() }
    Shortcut { sequence: "Alt+Right"; onActivated: app.left.forward() }
    Shortcut { sequence: "Alt+Up"; onActivated: app.left.up() }
    Shortcut { sequence: "Backspace"; enabled: !window.editingText && !window.dialogsOpen; onActivated: app.left.up() }
    Shortcut { sequence: "F5"; onActivated: { app.left.refresh(); app.refreshPlaces() } }

    Binding { target: window.Overlay.overlay; property: "scale"; value: window.uiScale }
    Binding { target: window.Overlay.overlay; property: "width"; value: window.layoutWidth }
    Binding { target: window.Overlay.overlay; property: "height"; value: window.layoutHeight }
    Binding { target: window.Overlay.overlay; property: "transformOrigin"; value: Item.TopLeft }

    component Field: TextField {
        id: field
        ContextMenu.menu: null
        TextMenuArea { target: field }
        Layout.preferredHeight: 34
        color: window.ink; font.pixelSize: 14; selectByMouse: true
        selectionColor: window.theme.selection; selectedTextColor: window.theme.selectionText
        background: Rectangle { radius: 8; color: window.theme.field; border.width: 1; border.color: field.activeFocus ? window.accent : window.line }
    }
    component FooterLabel: Text {
        Layout.preferredWidth: 60
        color: window.ink; font.pixelSize: 14
        horizontalAlignment: Text.AlignRight
    }
    component TypeBox: NookComboBox {
        Layout.preferredHeight: 34
        model: request.filters.map(f => f.label)
        currentIndex: Math.max(0, window.filterIndex)
        onActivated: window.filterIndex = currentIndex
    }

    Item {
        id: workspace
        width: window.layoutWidth; height: window.layoutHeight
        scale: window.uiScale; transformOrigin: Item.TopLeft
        Rectangle {
            anchors.fill: parent
            radius: 12
            color: theme.window
            border.width: 1; border.color: line
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 1; spacing: 0
                // Where: history, the path (click to type one) and search. The title is the window's.
                RowLayout {
                    Layout.fillWidth: true; Layout.preferredHeight: 50
                    Layout.leftMargin: 10; Layout.rightMargin: 8; spacing: 4
                    ActionButton { iconName: "back"; hint: "后退 · Alt+←"; enabled: app.left.canBack; foreground: ink; onClicked: app.left.back() }
                    ActionButton { iconName: "forward"; hint: "前进 · Alt+→"; enabled: app.left.canForward; foreground: ink; onClicked: app.left.forward() }
                    ActionButton { iconName: "up"; hint: "上一级 · Backspace / Alt+↑"; foreground: ink; onClicked: app.left.up() }
                    Field {
                        id: address; objectName: "chooserAddress"
                        Layout.fillWidth: true; Layout.leftMargin: 4
                        text: app.left.displayPath
                        color: activeFocus ? ink : "transparent"
                        leftPadding: 12; rightPadding: 12
                        // Enter opens a typed folder, or a file's folder with the file selected.
                        onAccepted: { if (!inputMethodComposing && text) { if (app.openLocation(text)) { searchBox.clear(); window.listFocus() } else selectAll() } }
                        onActiveFocusChanged: { text = app.left.displayPath; if (activeFocus) Qt.callLater(selectAll) }
                        Flickable {
                            id: crumbs
                            anchors.fill: parent; anchors.leftMargin: 5; anchors.rightMargin: 5
                            visible: !address.activeFocus
                            clip: true; contentWidth: crumbRow.width; contentHeight: height
                            boundsBehavior: Flickable.StopAtBounds
                            onContentWidthChanged: contentX = Math.max(0, contentWidth - width)
                            MouseArea {
                                width: Math.max(crumbs.width, crumbRow.width); height: crumbs.height
                                cursorShape: Qt.IBeamCursor
                                onClicked: { address.forceActiveFocus(); address.selectAll() }
                            }
                            Row {
                                id: crumbRow; height: parent.height; spacing: 1
                                Repeater {
                                    model: app.left.breadcrumbs
                                    Row {
                                        required property var modelData
                                        required property int index
                                        height: crumbRow.height
                                        Text { visible: index > 0; text: "›"; color: muted; height: parent.height; verticalAlignment: Text.AlignVCenter; font.pixelSize: 14; width: index > 0 ? 11 : 0 }
                                        ActionButton { height: parent.height; text: modelData.title; foreground: ink; hint: modelData.title; onClicked: window.enter(modelData.uri) }
                                    }
                                }
                            }
                        }
                    }
                    Field {
                        id: searchBox; objectName: "chooserSearch"
                        Layout.preferredWidth: Math.min(220, window.layoutWidth * 0.26); Layout.leftMargin: 4
                        placeholderText: "搜索 · 支持拼音"
                        leftPadding: 32; rightPadding: text ? 36 : 10
                        onTextChanged: { if (!inputMethodComposing) searchTimer.restart() }
                        onAccepted: searchTimer.restart()
                        Icon { name: "search"; width: 14; height: 14; x: 11; anchors.verticalCenter: parent.verticalCenter }
                        ActionButton { visible: searchBox.text !== ""; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; implicitWidth: 30; implicitHeight: 30; leftPadding: 7; rightPadding: 7; iconName: "close"; hint: "清除搜索"; foreground: muted; onClicked: searchBox.clear() }
                    }
                    ActionButton { objectName: "chooserListView"; iconName: "list"; hint: "列表视图"; active: !window.gridView; foreground: ink; Layout.leftMargin: 4; onClicked: app.settings.set("chooserGrid", false) }
                    ActionButton { objectName: "chooserGridView"; iconName: "grid"; hint: "网格视图"; active: window.gridView; foreground: ink; onClicked: app.settings.set("chooserGrid", true) }
                    Rectangle { width: 1; height: 18; color: line; Layout.leftMargin: 4; Layout.rightMargin: 2 }
                    WindowCloseButton { objectName: "chooserClose"; hint: "取消 · Esc"; onClicked: request.cancel() }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: line; opacity: 0.7 }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.leftMargin: 6; Layout.rightMargin: 10; Layout.topMargin: 8; spacing: 8
                    Sidebar {
                        id: sidebar
                        objectName: "chooserSidebar"
                        Layout.preferredWidth: 172; Layout.fillHeight: true
                        controller: app; host: window; searching: window.searching; compact: true
                        onActivate: place => { searchBox.clear(); app.activatePlace(place) }
                    }
                    FilePane {
                        id: pane
                        objectName: "chooserPane"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        directoryModel: window.listModel
                        controller: app; gridMode: window.gridView; focusedPane: true; searchMode: window.searching
                        dark: window.dark; dense: true; showHeader: false; useSavedWidths: false
                        emptyText: window.rulesOf(window.filterIndex).length || window.pickFolders ? "这里没有符合“" + (window.pickFolders ? "文件夹" : request.filters[window.filterIndex].name) + "”的项目" : "这个文件夹是空的"
                        onActivated: entry => window.activate(entry)
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: line; opacity: 0.7; Layout.topMargin: 8 }
                // Laid out as on Windows: the name, its type, then the answer.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.topMargin: 10; Layout.bottomMargin: 12
                    spacing: 8
                    RowLayout {
                        visible: !window.savingFiles
                        Layout.fillWidth: true; spacing: 10
                        FooterLabel { text: window.pickFolders ? "文件夹" : "文件名" }
                        Field {
                            id: nameField; objectName: "chooserName"
                            Layout.fillWidth: true; Layout.minimumWidth: 160
                            leftPadding: 12; rightPadding: 12
                            placeholderText: window.saving ? "文件名" : window.pickFolders ? "选择文件夹，或输入路径" : "选择文件，或输入名称、路径"
                            onAccepted: window.accept()
                        }
                        // Opening: the type sits beside the name.
                        TypeBox { objectName: "chooserFilter"; visible: !window.saving && window.hasTypes; Layout.preferredWidth: Math.min(260, window.layoutWidth * 0.32) }
                    }
                    RowLayout {
                        visible: window.saving && window.hasTypes
                        Layout.fillWidth: true; spacing: 10
                        FooterLabel { text: "保存类型" }
                        TypeBox { objectName: "chooserSaveType"; Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        ActionButton {
                            objectName: "chooserNewFolder"
                            visible: window.saving || window.savingFiles || window.pickFolders
                            text: "新建文件夹"; iconName: "add"; foreground: ink; enabled: !window.searching
                            onClicked: { folderName.text = ""; newFolderDialog.open() }
                        }
                        Repeater {
                            model: request.choices
                            RowLayout {
                                required property var modelData
                                spacing: 6
                                Text { visible: modelData.options.length > 0; text: modelData.label; color: muted; font.pixelSize: 13 }
                                NookComboBox {
                                    visible: modelData.options.length > 0
                                    Layout.preferredWidth: 130; Layout.preferredHeight: 34
                                    model: modelData.options.map(o => o.label)
                                    currentIndex: Math.max(0, modelData.options.findIndex(o => o.id === modelData.initial))
                                    onActivated: { let state = window.choiceState; state[modelData.id] = modelData.options[currentIndex].id; window.choiceState = state }
                                }
                                CheckBox {
                                    id: check
                                    visible: modelData.options.length === 0
                                    text: modelData.label
                                    checked: modelData.initial === "true"
                                    onToggled: { let state = window.choiceState; state[modelData.id] = checked ? "true" : "false"; window.choiceState = state }
                                    contentItem: Text { text: check.text; color: ink; font.pixelSize: 13; leftPadding: check.indicator.width + 7; verticalAlignment: Text.AlignVCenter }
                                    indicator: Rectangle {
                                        implicitWidth: 17; implicitHeight: 17; radius: 5
                                        y: (check.height - height) / 2
                                        color: check.checked ? accent : theme.field; border.color: check.checked ? accent : theme.lineStrong
                                        Text { anchors.centerIn: parent; visible: check.checked; text: "✓"; color: theme.accentInk; font.pixelSize: 11; font.bold: true }
                                    }
                                }
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: window.savingFiles ? "保存 " + request.files.length + " 个文件到“" + app.left.title + "”"
                                : window.pickFolders && listModel.selectedCount === 0 && nameField.text === "" ? "将选择当前文件夹“" + app.left.title + "”"
                                : ""
                            color: muted; font.pixelSize: 13; elide: Text.ElideMiddle
                        }
                        ActionButton { objectName: "chooserCancel"; text: "取消"; foreground: ink; implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, 84); onClicked: request.cancel() }
                        ActionButton { objectName: "chooserAccept"; text: window.acceptText; accent: true; enabled: window.canAccept; implicitWidth: Math.max(implicitContentWidth + leftPadding + rightPadding, 84); onClicked: window.accept() }
                    }
                }
            }
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 72
            visible: window.toastMessage !== ""; z: 100
            width: Math.min(window.layoutWidth - 70, toastLabel.implicitWidth + 36); height: toastLabel.implicitHeight + 22; radius: 9
            color: theme.raised; border.color: theme.lineStrong
            Text { id: toastLabel; anchors.centerIn: parent; text: window.toastMessage; color: ink; font.pixelSize: 14; width: Math.min(implicitWidth, window.layoutWidth - 106); wrapMode: Text.Wrap }
        }
    }

    NookDialog {
        id: newFolderDialog
        objectName: "chooserNewFolderDialog"
        iconName: "folder"
        title: "新建文件夹"
        message: "位置：" + app.left.displayPath
        wrapAnywhere: true
        readonly property string problem: folderName.text.trim() === "" ? "请输入名称"
            : folderName.text.indexOf("/") >= 0 ? "名称不能包含“/”"
            : !app.nameAvailable(app.left.uri, folderName.text) ? "此位置已存在“" + folderName.text + "”" : ""
        function submit() {
            if (problem !== "") return
            window.pendingFolder = request.resolve(app.left.uri, folderName.text)
            app.operations.run("mkdir", [], app.left.uri, folderName.text)
            close()
        }
        onOpened: folderName.forceActiveFocus()
        onClosed: window.listFocus()
        Field { id: folderName; objectName: "chooserFolderName"; Layout.fillWidth: true; Layout.preferredHeight: 40; leftPadding: 12; rightPadding: 12; onAccepted: newFolderDialog.submit() }
        Text { text: newFolderDialog.problem || "Enter 确认 · Esc 取消"; color: newFolderDialog.problem && folderName.text ? theme.warning : muted; font.pixelSize: 12 }
        actions: [
            ActionButton { text: "取消"; foreground: ink; implicitHeight: 38; onClicked: newFolderDialog.close() },
            ActionButton { text: "创建"; accent: true; implicitHeight: 38; enabled: newFolderDialog.problem === ""; onClicked: newFolderDialog.submit() }
        ]
    }
    NookDialog {
        id: overwriteDialog
        objectName: "chooserOverwriteDialog"
        property string target: ""
        danger: true; iconName: "file"
        title: "“" + app.localPath(target).split("/").pop() + "”已存在"
        message: "替换后，原文件的内容会被覆盖。"
        wrapAnywhere: true
        function replace() { const chosen = target; close(); window.finish([chosen]) }
        onAccepted: replace()
        onClosed: window.listFocus()
        actions: [
            ActionButton { text: "取消"; foreground: ink; implicitHeight: 38; onClicked: overwriteDialog.close() },
            ActionButton { objectName: "chooserReplace"; text: "替换"; accent: true; danger: true; implicitHeight: 38; onClicked: overwriteDialog.replace() }
        ]
    }
}
