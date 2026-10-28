import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: window
    objectName: "mainWindow"
    required property var controller
    readonly property var app: controller
    readonly property var prefs: app.settings.values
    readonly property bool dark: prefs.theme === "dark" || (prefs.theme === "system" && Qt.styleHints.colorScheme === Qt.ColorScheme.Dark)
    readonly property Theme theme: Theme { dark: window.dark; effects: prefs.effects }
    readonly property color ink: theme.ink
    readonly property color muted: theme.muted
    readonly property color surface: theme.raised
    readonly property color line: theme.line
    readonly property color accent: theme.accent
    readonly property bool searching: searchBox.text.trim() !== ""
    property bool filtersVisible: false
    property var contextModel: null
    property var contextEntry: ({})
    property var contextApps: []
    property var deleteUris: []
    property string quickAccessUri: ""
    property string inputKind: "mkdir"
    property string inputDestination: ""
    readonly property real uiScale: Math.max(0.75, Math.min(1.5, Number(prefs.uiScale) || 1))
    readonly property real layoutWidth: width / uiScale
    readonly property real layoutHeight: height / uiScale
    readonly property bool editingText: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    readonly property bool dialogsOpen: settingsDialog.visible || propertiesDialog.visible || nameDialog.visible || deleteDialog.visible || conflictDialog.visible || closeDialog.visible || errorsDialog.visible
    readonly property bool menusOpen: contextMenu.visible || blankMenu.visible || quickAccessMenu.visible || sortBox.popup.visible || scopeBox.popup.visible || typeBox.popup.visible || afterDate.pickerOpen || beforeDate.pickerOpen
    property bool permanent: false
    property bool closingAfterTask: false
    property var hoveredEntry: ({})
    property string suppressedHoverUri: ""
    property string toastMessage: ""
    // Selection mode: clicks toggle items and any drag draws a band (see FilePane).
    property bool selectionMode: false
    onSelectionModeChanged: if (selectionMode) toast("选择模式：单击逐个勾选，任意处拖动框选 · Esc 退出")
    // Spring-loading: hovering a drag over a place, path segment, tab or folder opens it.
    readonly property int springDelay: 700
    property var springOrigin: null
    property bool springMoved: false
    property string dropHintText: ""
    property real dropHintX: 0
    property real dropHintY: 0
    property string hintUri: ""
    property int hintModifiers: -1
    property string hintAction: ""
    width: 1280; height: 820
    minimumWidth: 920; minimumHeight: 640
    visible: true
    title: "nook · " + (app.activePane ? app.activePane.title : "文件管理器")
    color: "transparent"
    palette.window: surface
    palette.base: theme.field
    palette.text: ink
    palette.windowText: ink
    palette.buttonText: ink
    palette.button: surface
    palette.highlight: theme.selection
    palette.highlightedText: ink
    palette.mid: line
    palette.placeholderText: muted

    function currentModel() { return searching ? app.search.model : app.activePane }
    function selectedEntry(source) {
        const model = source || currentModel()
        return model ? model.firstSelected() : ({})
    }
    function toast(message) {
        toastLeave.stop()
        toastMessage = message; toastTimer.restart()
        if (prefs.animations) toastEnter.restart()
    }
    // File uris carried by a drag: from the platform drag, or from an in-window drag source
    // that lists them itself (Qt Quick's internal drags carry no mime data).
    function dragUris(drag) {
        let values = []
        if (drag.hasUrls) {
            for (let u of drag.urls) {
                const uri = String(u)
                if (uri.startsWith("file:") || uri.startsWith("trash:")) values.push(uri)
            }
        } else if (drag.source && drag.source.fileUris)
            values = drag.source.fileUris.slice()
        return values
    }
    function dropLabel(action, name) { return ({move: "移动到 ", copy: "复制到 ", trash: "移入 "})[action] + "“" + name + "”" }
    // The bubble beside the pointer over targets outside the file list (path, places, tabs).
    // Returns whether the items can be dropped there; the action is re-checked only when the
    // target or the modifier keys change.
    function dropHint(source, x, y, urls, uri, name) {
        const mods = app.modifiers()
        if (uri !== hintUri || mods !== hintModifiers) {
            hintUri = uri; hintModifiers = mods
            hintAction = uri && urls.length && app.canDrop(urls, uri) ? app.dropAction(urls, uri) : ""
        }
        const p = source.mapToItem(workspace, x, y)
        dropHintX = p.x; dropHintY = p.y
        dropHintText = hintAction ? dropLabel(hintAction, name) : "停留片刻打开“" + name + "”"
        return hintAction !== ""
    }
    function clearDropHint() { dropHintText = ""; hintUri = ""; hintModifiers = -1; hintAction = "" }
    function springLoad(open) {
        springMoved = true
        searchBox.clear()
        open()
    }
    // Starts the platform drag of the selected items. Qt begins an automatic drag on its own a
    // moment after `active` is set and ends it with dragFinished, so nothing here may reset it.
    function startFileDrag(uris, icon) {
        if (!uris.length || fileDrag.Drag.active) return
        fileDrag.fileUris = uris
        fileDrag.iconName = icon || "file"
        fileDrag.Drag.mimeData = {"text/uri-list": uris.join("\r\n")}
        springOrigin = {tab: app.activeTab, side: app.activeSide, uri: app.activePane.uri}
        springMoved = false
        fileDrag.Drag.active = true
    }
    // A cancelled drag (Esc, or released where nothing accepts it) returns to where it began;
    // after a drop the view stays at the destination.
    function fileDragFinished(action) {
        const origin = springOrigin
        springOrigin = null
        clearDropHint()
        if (action !== Qt.IgnoreAction || !springMoved || !origin) return
        springMoved = false
        if (origin.tab < app.tabs.length) app.activeTab = origin.tab
        app.activeSide = origin.side
        if (app.activePane.uri !== origin.uri) app.navigate(origin.uri)
    }
    function focusActivePane() { if (app.activeSide === 1 && rightPane.visible) rightPane.focusFiles(); else leftPane.focusFiles() }
    function openInput(kind) {
        inputKind = kind
        inputDestination = contextModel && blankMenu.visible ? contextModel.uri : app.activePane.uri
        const entry = kind === "rename" ? selectedEntry() : ({})
        nameDialog.originalName = kind === "rename" ? entry.fileName || "" : ""
        nameDialog.entryIcon = kind === "rename" ? entry.iconName || "file" : kind === "touch" ? "document" : "folder"
        nameDialog.entryFolder = kind === "rename" ? !!entry.folder : kind === "mkdir"
        nameDialog.renameParent = entry.fileUri ? app.parentUri(entry.fileUri) : ""
        nameInput.text = kind === "rename" ? entry.fileName || "" : kind === "touch" ? "新建文本文档.txt" : "新建文件夹"
        nameDialog.open(); nameInput.forceActiveFocus()
        // Like Windows: select the base name and leave the extension alone.
        const dot = nameInput.text.lastIndexOf(".")
        if (!nameDialog.entryFolder && dot > 0) nameInput.select(0, dot); else nameInput.selectAll()
    }
    function removeSelection(force) {
        if (!currentModel() || !currentModel().selectedCount) return
        permanent = force || currentModel().uri.startsWith("trash:")
        deleteUris = currentModel().selectedUris.slice()
        deleteDialog.open()
    }
    function activate(entry) {
        if (!entry.fileUri) return
        if (entry.folder) { searchBox.clear(); app.navigate(entry.fileUri) }
        else app.openExternal(entry.fileUri)
    }
    function preview(entry) {
        hoverTimer.stop()
        if (app.preview.visible) app.preview.close()
        else if (entry.fileUri) app.preview.open(entry.fileUri)
    }
    function hoverEntry(entry) {
        hoveredEntry = entry
        if (!entry.fileUri) { suppressedHoverUri = ""; hoverTimer.stop() }
        else if (entry.fileUri !== suppressedHoverUri && app.preview.hoverEligible(entry) && !app.preview.visible) hoverTimer.restart()
        else hoverTimer.stop()
    }
    function showContext(model, item, x, y) {
        hoverTimer.stop()
        contextModel = model
        contextEntry = selectedEntry(model)
        contextApps = contextEntry.fileUri && !contextEntry.folder ? app.openWithApplications(contextEntry.fileUri) : []
        let pos = item.mapToItem(workspace, x, y)
        contextMenu.x = Math.max(8, Math.min(pos.x, window.layoutWidth - contextMenu.width - 8))
        contextMenu.y = Math.max(8, Math.min(pos.y, window.layoutHeight - contextMenu.implicitHeight - 8))
        contextMenu.open()
    }
    function showBlankContext(model, item, x, y) {
        hoverTimer.stop(); contextModel = model; contextEntry = ({})
        const p = item.mapToItem(workspace, x, y)
        blankMenu.x = Math.max(8, Math.min(p.x, layoutWidth - blankMenu.width - 8))
        blankMenu.y = Math.max(8, Math.min(p.y, layoutHeight - blankMenu.implicitHeight - 8))
        blankMenu.open()
    }
    function inspectProperties(uri) { app.properties.inspect(uri); propertiesDialog.open() }
    function runSearch() {
        if (!searching) { app.search.query(""); return }
        let min = Number(minSize.text || "0") * 1024 * 1024
        let max = Number(maxSize.text || "0") * 1024 * 1024
        let after = afterDate.text ? Date.parse(afterDate.text + "T00:00:00") / 1000 : 0
        let before = beforeDate.text ? Date.parse(beforeDate.text + "T23:59:59") / 1000 : 0
        if (!isFinite(min) || !isFinite(max) || !isFinite(after) || !isFinite(before) || min < 0 || max < 0 || (max && min > max)) {
            toast("请检查大小或日期筛选，日期格式为 YYYY-MM-DD"); return
        }
        app.search.query(searchBox.text, scopeBox.currentIndex === 1 ? app.activePane.uri : "", ["all", "folder", "file", "image", "document", "video", "music"][typeBox.currentIndex], min, max, after, before)
    }
    // Search filter bar parts: a muted label followed by its controls, and a field styled
    // like the address/search boxes with an optional unit suffix and inline validation.
    component FilterGroup: Row {
        property string title: ""
        spacing: 6
        Text { text: parent.title; color: muted; font.pixelSize: 12; height: 34; verticalAlignment: Text.AlignVCenter; rightPadding: 2 }
    }
    component FilterField: TextField {
        id: field
        property string suffix: ""
        property bool date: false
        readonly property bool valid: text === "" || (date ? /^\d{4}-\d{2}-\d{2}$/.test(text) && isFinite(Date.parse(text + "T00:00:00")) : acceptableInput)
        height: 34
        font.pixelSize: 13; color: ink
        placeholderTextColor: theme.faint
        leftPadding: 11; rightPadding: suffix ? unit.implicitWidth + 16 : 11
        selectByMouse: true
        selectionColor: theme.selection; selectedTextColor: theme.selectionText
        ContextMenu.menu: null
        TextMenuArea { target: field }
        inputMethodHints: date ? Qt.ImhDate : Qt.ImhFormattedNumbersOnly
        validator: RegularExpressionValidator { regularExpression: /^[0-9-]{0,10}$/ }
        onEditingFinished: { if (valid) searchTimer.restart() }
        background: Rectangle {
            radius: 8
            color: theme.field
            border.width: 1
            border.color: !field.valid && !field.activeFocus ? theme.danger : field.activeFocus ? accent : field.hovered ? theme.lineStrong : line
            Behavior on border.color { enabled: prefs.animations; ColorAnimation { duration: 110 } }
            Rectangle {
                anchors.fill: parent; anchors.margins: -3; radius: 10; z: -1
                color: "transparent"; border.width: 2; border.color: theme.focusHalo
                opacity: field.activeFocus ? 1 : 0
                Behavior on opacity { enabled: prefs.animations; NumberAnimation { duration: 120 } }
            }
        }
        Text {
            id: unit
            visible: field.suffix !== ""
            text: field.suffix; color: muted; font.pixelSize: 12
            anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
        }
    }
    component DateField: FilterField {
        id: dateField
        property string rangeStart: ""
        property string rangeEnd: ""
        readonly property alias pickerOpen: picker.visible
        width: 132
        date: true
        placeholderText: "选择日期"
        rightPadding: 34
        onPressed: if (!picker.visible) picker.open()
        AbstractButton {
            objectName: dateField.objectName + "Calendar"
            anchors.right: parent.right; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter
            width: 26; height: 26
            hoverEnabled: true; focusPolicy: Qt.NoFocus
            Accessible.name: "选择日期"
            background: Rectangle { radius: 6; color: picker.visible ? theme.activeFill : parent.hovered ? theme.hover : "transparent" }
            contentItem: Item { Icon { anchors.centerIn: parent; name: "calendar"; width: 16; height: 16; tint: picker.visible ? theme.accentText : theme.icon } }
            onClicked: picker.visible ? picker.close() : picker.open()
        }
        DatePicker {
            id: picker
            y: dateField.height + 6
            value: dateField.valid ? dateField.text : ""
            rangeStart: dateField.rangeStart
            rangeEnd: dateField.rangeEnd
            onPicked: date => { dateField.text = date; searchTimer.restart() }
            onCleared: { dateField.clear(); searchTimer.restart() }
        }
    }
    onClosing: event => {
        app.preview.close()
        if (app.operations.busy) { event.accepted = false; closeDialog.open() }
        else app.saveSession()
    }
    Timer { id: searchTimer; interval: 220; onTriggered: window.runSearch() }
    Timer { id: toastTimer; interval: 4500; onTriggered: { if (prefs.animations) toastLeave.restart(); else window.toastMessage = "" } }
    Timer {
        id: hoverTimer; interval: 600
        onTriggered: { if (prefs.hoverPreview && hoveredEntry.fileUri && app.preview.hoverEligible(hoveredEntry) && !app.preview.visible && !window.dialogsOpen && !window.menusOpen) app.preview.open(hoveredEntry.fileUri) }
    }
    Timer {
        id: applyIndexTimer; interval: 100; repeat: true
        onTriggered: { if (!app.search.scanning) { stop(); app.search.start() } }
    }
    Connections { target: app; function onNotice(message) { window.toast(message) } }
    Connections {
        target: app.operations
        function onFinished() {
            window.toast(app.operations.status)
            if (window.closingAfterTask) window.close()
        }
        function onConflictChanged() { if (app.operations.waitingConflict) conflictDialog.open(); else conflictDialog.close() }
    }
    Connections {
        target: app.search
        function onScanFinished() { if (window.searching) searchTimer.restart() }
    }
    Connections {
        target: app.activePane
        function onLocationChanged() { address.text = app.activePane.displayPath }
    }
    Connections {
        target: app
        function onActivePaneChanged() { address.text = app.activePane.displayPath }
    }

    Shortcut { sequence: "Space"; enabled: app.preview.visible || (!window.editingText && !window.dialogsOpen && !window.menusOpen && currentModel().selectedCount > 0); autoRepeat: false; onActivated: window.preview(window.selectedEntry()) }
    Shortcut { sequence: "Ctrl+T"; onActivated: app.addTab(app.activePane.uri) }
    Shortcut { sequence: "Ctrl+W"; onActivated: { if (app.preview.visible) app.preview.close(); else if (app.tabs.length > 1) app.closeTab(app.activeTab); else window.close() } }
    Shortcut { sequence: "Ctrl+N"; onActivated: app.requestWindow() }
    Shortcut { sequence: "Ctrl+L"; onActivated: { address.forceActiveFocus(); address.selectAll() } }
    Shortcut { sequence: "Ctrl+K"; onActivated: { searchBox.forceActiveFocus(); searchBox.selectAll() } }
    Shortcut { sequence: "Ctrl+F"; onActivated: { searchBox.forceActiveFocus(); searchBox.selectAll() } }
    Shortcut { sequence: "Alt+Left"; onActivated: app.activePane.back() }
    Shortcut { sequence: "Alt+Right"; onActivated: app.activePane.forward() }
    Shortcut { sequence: "Alt+Up"; onActivated: app.activePane.up() }
    Shortcut { sequence: "F5"; onActivated: { app.activePane.refresh(); app.refreshPlaces() } }
    Shortcut { sequence: "Ctrl+H"; onActivated: app.activePane.showHidden = !app.activePane.showHidden }
    Shortcut { sequence: "Ctrl+1"; onActivated: app.dual = false }
    Shortcut { sequence: "Ctrl+2"; onActivated: app.dual = true }
    Shortcut { sequence: "Escape"; onActivated: { if (app.preview.visible) app.preview.close(); else if (address.activeFocus) { address.text = app.activePane.displayPath; window.focusActivePane() } else if (selectionMode) selectionMode = false; else if (searching) searchBox.clear(); else currentModel().clearSelection() } }

    // Keep geometry, text, icons and popup hit targets in the same logical coordinate system.
    Binding { target: window.Overlay.overlay; property: "scale"; value: window.uiScale }
    Binding { target: window.Overlay.overlay; property: "width"; value: window.layoutWidth }
    Binding { target: window.Overlay.overlay; property: "height"; value: window.layoutHeight }
    Binding { target: window.Overlay.overlay; property: "transformOrigin"; value: Item.TopLeft }
    Item {
        id: workspace
        objectName: "scaledWorkspace"
        width: window.layoutWidth; height: window.layoutHeight
        scale: window.uiScale; transformOrigin: Item.TopLeft
    Rectangle {
        anchors.fill: parent
        radius: 12
        color: theme.window
        border.width: 1; border.color: line
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 1; spacing: 0
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 48
                color: theme.chrome
                // Dragging files over a tab switches to it after a moment; dropping puts them in its folder.
                DropArea {
                    id: tabDrop
                    objectName: "tabDropArea"
                    x: tabBar.x + tabsList.x; y: tabBar.y + tabsList.y
                    width: tabsList.width; height: tabsList.height
                    property var urls: []
                    property int index: -1
                    property bool allowed: false
                    function locate(px, py) {
                        const i = tabsList.indexAt(px + tabsList.contentX, py)
                        if (i !== index) {
                            index = i
                            if (i >= 0 && i !== app.activeTab) tabSpring.restart(); else tabSpring.stop()
                        }
                        if (i >= 0) allowed = window.dropHint(tabDrop, px, py, urls, app.tabs[i].uri, app.tabs[i].title)
                        else { allowed = false; window.clearDropHint() }
                    }
                    function reset() { tabSpring.stop(); index = -1; urls = []; allowed = false; window.clearDropHint() }
                    onEntered: drag => {
                        urls = window.dragUris(drag)
                        if (!urls.length) { drag.accepted = false; return }
                        index = -1
                        locate(drag.x, drag.y)
                    }
                    onPositionChanged: drag => locate(drag.x, drag.y)
                    onExited: reset()
                    onDropped: drop => {
                        locate(drop.x, drop.y)
                        const i = index, ok = allowed, values = urls
                        reset()
                        if (!ok) return
                        app.dragDrop(values, app.tabs[i].uri)
                        drop.accept(Qt.CopyAction)
                    }
                    Timer { id: tabSpring; interval: window.springDelay; onTriggered: { const i = tabDrop.index; if (i >= 0 && i < app.tabs.length) window.springLoad(() => app.activeTab = i) } }
                }
                RowLayout {
                    id: tabBar
                    anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 14; spacing: 12
                    Rectangle {
                        width: 26; height: 26; radius: 8; color: theme.accentSoft
                        Icon { anchors.centerIn: parent; name: "folder"; width: 20; height: 20 }
                    }
                    Text { text: "nook"; color: ink; font.pixelSize: 16; font.weight: Font.DemiBold }
                    Rectangle { width: 1; height: 18; color: line; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                    ListView {
                        id: tabsList
                        Layout.fillWidth: true; Layout.fillHeight: true
                        orientation: ListView.Horizontal
                        spacing: 4; clip: true
                        model: app.tabs
                        delegate: Item {
                            required property int index
                            required property var modelData
                            width: Math.min(190, Math.max(110, tabsList.width / Math.max(3, app.tabs.length)))
                            height: tabsList.height
                            Rectangle {
                                objectName: "tabCard"
                                width: parent.width; height: 36
                                anchors.verticalCenter: parent.verticalCenter
                                radius: 7
                                readonly property bool dropTarget: tabDrop.index === index
                                color: dropTarget ? theme.dropFill : app.activeTab === index ? theme.tab : tabMouse.containsMouse ? theme.hover : "transparent"
                                border.width: dropTarget || app.activeTab === index ? 1 : 0; border.color: dropTarget ? accent : theme.line
                                SpringProgress { running: parent.dropTarget && tabSpring.running; color: accent; duration: window.springDelay }
                                MouseArea {
                                    id: tabMouse; anchors.fill: parent; hoverEnabled: true
                                    // Middle-click closes the tab, as in browsers and other file managers.
                                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                    onClicked: mouse => { if (mouse.button === Qt.MiddleButton) app.closeTab(index); else app.activeTab = index }
                                }
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 5; spacing: 7
                                    Icon { name: "folder"; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                                    Text { text: modelData.title; color: app.activeTab === index ? ink : muted; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true; Layout.minimumWidth: 0 }
                                    ActionButton { iconName: "close"; hint: "关闭标签"; implicitWidth: 28; implicitHeight: 28; leftPadding: 5; rightPadding: 5; topPadding: 5; bottomPadding: 5; foreground: muted; onClicked: app.closeTab(index) }
                                }
                            }
                        }
                    }
                    Rectangle { width: 1; height: 18; color: line }
                    ActionButton { iconName: dark ? "sun" : "moon"; hint: "切换深浅主题"; foreground: ink; onClicked: app.settings.set("theme", dark ? "light" : "dark") }
                    ActionButton { objectName: "settingsButton"; iconName: "settings"; hint: "设置"; onClicked: settingsDialog.open() }
                    WindowCloseButton { objectName: "windowCloseButton"; hint: "关闭窗口 · Ctrl+W"; Layout.leftMargin: 2; onClicked: window.close() }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 57
                Layout.leftMargin: 15; Layout.rightMargin: 17; spacing: 7
                ActionButton { iconName: "back"; hint: "后退 · Alt+←"; enabled: app.activePane.canBack; foreground: ink; onClicked: app.activePane.back() }
                ActionButton { iconName: "forward"; hint: "前进 · Alt+→"; enabled: app.activePane.canForward; foreground: ink; onClicked: app.activePane.forward() }
                ActionButton { objectName: "upButton"; iconName: "up"; hint: "上一级 · Backspace / Alt+↑"; foreground: ink; onClicked: app.activePane.up() }
                ActionButton { objectName: "refreshButton"; iconName: "refresh"; hint: "刷新 · F5"; foreground: ink; onClicked: { app.activePane.refresh(); app.refreshPlaces() } }
                TextField {
                    id: address; objectName: "addressField"
                    ContextMenu.menu: null
                    TextMenuArea { id: addressMenu; target: address }
                    Layout.fillWidth: true; Layout.preferredHeight: 38
                    text: app.activePane.displayPath
                    font.pixelSize: 15; color: activeFocus ? ink : "transparent"
                    leftPadding: 13; rightPadding: 12
                    selectByMouse: true
                    // Enter jumps to a typed folder, or opens a file's folder with the file selected.
                    onAccepted: { if (!inputMethodComposing && text) { if (app.openLocation(text)) { searchBox.clear(); window.focusActivePane() } else selectAll() } }
                    onActiveFocusChanged: {
                        text = app.activePane.displayPath
                        if (activeFocus) Qt.callLater(selectAll)
                    }
                    background: Rectangle { radius: 8; color: theme.field; border.width: 1; border.color: address.activeFocus ? accent : line }
                    Flickable {
                        id: crumbs
                        anchors.fill: parent; anchors.leftMargin: 7; anchors.rightMargin: 7
                        visible: !address.activeFocus
                        objectName: "crumbTrail"
                        clip: true; contentWidth: crumbRow.width; contentHeight: height
                        boundsBehavior: Flickable.StopAtBounds
                        // A long path keeps its last segments (the current folder and its nearest
                        // parents) in view; while a drag hovers the trail it stays where it is.
                        function showEnd() { if (!crumbDrop.containsDrag) contentX = Math.max(0, contentWidth - width) }
                        onContentWidthChanged: showEnd()
                        onWidthChanged: showEnd()
                        // Clicking beside the breadcrumbs switches to the full, pre-selected path.
                        MouseArea {
                            objectName: "addressEditArea"
                            width: Math.max(crumbs.width, crumbRow.width); height: crumbs.height
                            cursorShape: Qt.IBeamCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: mouse => { if (mouse.button === Qt.LeftButton) { address.forceActiveFocus(); address.selectAll() } }
                            // Right-click: switch to the full, selected path and open the text menu at the pointer.
                            onPressed: mouse => {
                                if (mouse.button !== Qt.RightButton) return
                                address.forceActiveFocus(); address.selectAll()
                                const p = mapToItem(address, mouse.x, mouse.y)
                                addressMenu.menu.popup(address, p.x, p.y)
                            }
                        }
                        Row {
                            id: crumbRow; height: parent.height; spacing: 1
                            Repeater {
                                model: crumbDrop.trail || app.activePane.breadcrumbs
                                Row {
                                    required property var modelData
                                    required property int index
                                    readonly property int crumbIndex: index
                                    height: crumbRow.height
                                    Text { visible: index > 0; text: "›"; color: muted; height: parent.height; verticalAlignment: Text.AlignVCenter; font.pixelSize: 15; width: index > 0 ? 12 : 0 }
                                    ActionButton {
                                        objectName: "crumbButton"
                                        height: parent.height; text: modelData.title; foreground: ink; hint: modelData.title
                                        active: crumbDrop.index === index
                                        onClicked: { searchBox.clear(); app.navigate(modelData.uri) }
                                        SpringProgress { running: parent.active && crumbSpring.running; color: accent; duration: window.springDelay }
                                    }
                                }
                            }
                        }
                    }
                    // Files dragged onto a path segment move there (Ctrl copies, Shift moves); resting on
                    // one opens it, so any folder up the path is one hover away.
                    DropArea {
                        id: crumbDrop
                        objectName: "crumbDropArea"
                        anchors.fill: crumbs
                        enabled: crumbs.visible
                        property var urls: []
                        property int index: -1
                        property bool allowed: false
                        // The trail is held while a drag is over it: opening a parent must not shift
                        // the segments under the pointer, and the deeper ones stay reachable.
                        property var trail: null
                        readonly property var segments: trail || app.activePane.breadcrumbs
                        function crumbAt(px, py) {
                            const p = mapToItem(crumbRow, px, py)
                            const child = crumbRow.childAt(p.x, p.y)
                            return child && child.crumbIndex !== undefined ? child.crumbIndex : -1
                        }
                        function locate(px, py) {
                            const i = crumbAt(px, py)
                            const crumb = i >= 0 ? segments[i] : null
                            if (i !== index) {
                                index = i
                                if (crumb && crumb.uri !== app.activePane.uri) crumbSpring.restart(); else crumbSpring.stop()
                            }
                            if (crumb) allowed = window.dropHint(crumbDrop, px, py, urls, crumb.uri, crumb.title)
                            else { allowed = false; window.clearDropHint() }
                        }
                        function reset() { crumbSpring.stop(); index = -1; urls = []; allowed = false; trail = null; window.clearDropHint() }
                        onEntered: drag => {
                            urls = window.dragUris(drag)
                            if (!urls.length) { drag.accepted = false; return }
                            trail = app.activePane.breadcrumbs
                            index = -1
                            locate(drag.x, drag.y)
                        }
                        onPositionChanged: drag => locate(drag.x, drag.y)
                        onExited: reset()
                        onDropped: drop => {
                            locate(drop.x, drop.y)
                            const crumb = index >= 0 ? segments[index] : null
                            const ok = allowed, values = urls
                            reset()
                            if (!ok || !crumb) return
                            app.dragDrop(values, crumb.uri)
                            drop.accept(Qt.CopyAction)
                        }
                        Timer {
                            id: crumbSpring; interval: window.springDelay
                            onTriggered: {
                                const crumb = crumbDrop.segments[crumbDrop.index]
                                if (!crumb) return
                                window.springLoad(() => app.navigate(crumb.uri))
                                // "…" stands for the folders in between: resting on it keeps climbing.
                                if (crumb.title !== "…") return
                                crumbDrop.trail = app.activePane.breadcrumbs
                                const next = crumbDrop.segments[crumbDrop.index]
                                if (next && next.title === "…" && next.uri !== app.activePane.uri) crumbSpring.restart()
                            }
                        }
                        // Long paths: resting near either end scrolls the hidden segments into reach.
                        Timer {
                            interval: 16; repeat: true
                            running: crumbDrop.containsDrag && (crumbDrop.drag.x < 28 || crumbDrop.drag.x > crumbDrop.width - 28)
                            onTriggered: {
                                const step = crumbDrop.drag.x < 28 ? -8 : 8
                                crumbs.contentX = Math.max(0, Math.min(crumbs.contentX + step, crumbs.contentWidth - crumbs.width))
                            }
                        }
                    }
                }
                ActionButton { objectName: "newTabButton"; iconName: "add"; hint: "新标签页 · Ctrl+T"; foreground: ink; onClicked: app.addTab(app.activePane.uri) }
                TextField {
                    id: searchBox; objectName: "searchField"
                    ContextMenu.menu: null
                    TextMenuArea { target: searchBox }
                    Layout.preferredWidth: window.layoutWidth < 1050 ? 220 : 290; Layout.preferredHeight: 38
                    placeholderText: "搜索本地文件 · 支持拼音"; color: ink; font.pixelSize: 15
                    leftPadding: 34; rightPadding: text ? 38 : 52
                    selectByMouse: true
                    onTextChanged: { if (!inputMethodComposing) searchTimer.restart() }
                    onInputMethodComposingChanged: { if (!inputMethodComposing) searchTimer.restart() }
                    onAccepted: { if (!inputMethodComposing) window.runSearch() }
                    background: Rectangle { radius: 8; color: theme.field; border.width: 1; border.color: searchBox.activeFocus ? accent : line }
                    Icon { name: "search"; width: 15; height: 15; x: 11; anchors.verticalCenter: parent.verticalCenter }
                    Text { visible: !searchBox.text; anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: "Ctrl K"; color: muted; font.pixelSize: 12 }
                    ActionButton { visible: searchBox.text !== ""; anchors.right: parent.right; implicitWidth: 34; implicitHeight: 34; leftPadding: 8; rightPadding: 8; iconName: "close"; hint: "清除搜索"; foreground: muted; onClicked: searchBox.clear() }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: line; opacity: 0.7 }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 51; Layout.leftMargin: 18; Layout.rightMargin: 18; spacing: 5
                ActionButton { text: window.layoutWidth < 1050 ? "新建" : "新建文件夹"; iconName: "add"; accent: true; enabled: !searching && !app.activePane.uri.startsWith("trash:"); onClicked: window.openInput("mkdir") }
                Rectangle { width: 1; height: 19; color: line; Layout.leftMargin: 9; Layout.rightMargin: 9 }
                ActionButton { iconName: "cut"; hint: "剪切 · Ctrl+X"; foreground: ink; enabled: currentModel().selectedCount > 0; onClicked: app.copy(true, currentModel()) }
                ActionButton { iconName: "copy"; hint: "复制 · Ctrl+C"; foreground: ink; enabled: currentModel().selectedCount > 0; onClicked: app.copy(false, currentModel()) }
                ActionButton { iconName: "paste"; hint: "粘贴 · Ctrl+V"; foreground: ink; enabled: !searching; onClicked: app.paste() }
                ActionButton { iconName: "rename"; hint: "重命名 · F2"; foreground: ink; enabled: currentModel().selectedCount === 1 && !app.activePane.uri.startsWith("trash:"); onClicked: window.openInput("rename") }
                ActionButton { iconName: "trash"; hint: "移入回收站 · Delete"; enabled: currentModel().selectedCount > 0; onClicked: window.removeSelection(false) }
                Rectangle { width: 1; height: 19; color: line; Layout.leftMargin: 9; Layout.rightMargin: 9 }
                NookComboBox { id: sortBox; Layout.preferredWidth: 108; Layout.preferredHeight: 36; font.pixelSize: 13; model: ["名称排序", "大小排序", "日期排序", "类型排序"]; enabled: !searching; onActivated: app.activePane.sortBy(["name", "size", "date", "type"][currentIndex], currentIndex === 2) }
                ActionButton { text: window.layoutWidth < 1100 ? "" : "隐藏文件"; iconName: "hidden"; hint: "显示/隐藏点文件 · Ctrl+H"; foreground: ink; active: app.activePane.showHidden; onClicked: app.activePane.showHidden = !app.activePane.showHidden }
                ActionButton { text: window.layoutWidth < 1050 ? "" : "筛选"; iconName: "filter"; hint: "搜索筛选"; foreground: ink; active: filtersVisible; onClicked: filtersVisible = !filtersVisible }
                ActionButton {
                    objectName: "selectionModeButton"
                    text: window.layoutWidth < 1050 ? "" : "选择"; iconName: "select"; foreground: ink
                    hint: selectionMode ? "退出选择模式 · Esc（保留已选项目）" : "选择模式：单击逐个勾选，任意处拖动框选"
                    active: selectionMode
                    onClicked: { selectionMode = !selectionMode; window.focusActivePane() }
                }
                Item { Layout.fillWidth: true }
                ActionButton { iconName: "list"; hint: "列表视图"; active: !app.grid; foreground: ink; onClicked: app.grid = false }
                ActionButton { iconName: "grid"; hint: "网格视图"; active: app.grid; foreground: ink; onClicked: app.grid = true }
                Rectangle { width: 1; height: 19; color: line; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                ActionButton { objectName: "dualPaneButton"; iconName: "split"; hint: "左右双栏 · Ctrl+2"; active: app.dual; foreground: ink; onClicked: app.dual = !app.dual }
                ActionButton { iconName: "terminal"; hint: "在此处打开 kitty"; foreground: ink; onClicked: app.terminal() }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.leftMargin: 10; Layout.rightMargin: 14; Layout.bottomMargin: 10; spacing: 12
                ColumnLayout {
                    id: sidebarColumn
                    Layout.preferredWidth: window.layoutWidth < 1000 ? 202 : 224; Layout.minimumWidth: window.layoutWidth < 1000 ? 202 : 224; Layout.maximumWidth: window.layoutWidth < 1000 ? 202 : 224; Layout.fillHeight: true
                    spacing: 10
                    Sidebar {
                        id: sidebar
                        objectName: "sidebar"
                        Layout.fillWidth: true; Layout.fillHeight: true
                        controller: app; host: window; searching: window.searching
                        onActivate: place => { searchBox.clear(); app.activatePlace(place) }
                        onRemoveRequested: (uri, x, y) => {
                            quickAccessUri = uri
                            const p = sidebar.mapToItem(workspace, x, y)
                            quickAccessMenu.x = Math.min(p.x, layoutWidth - quickAccessMenu.width - 8)
                            quickAccessMenu.y = Math.min(p.y, layoutHeight - quickAccessMenu.implicitHeight - 8)
                            quickAccessMenu.open()
                        }
                    }
                    RecentActivity {
                        controller: app; host: window
                        maximumRecentHeight: Math.max(100, sidebarColumn.height * 0.45)
                        Layout.fillWidth: true
                        Layout.preferredHeight: implicitHeight
                        Layout.maximumHeight: implicitHeight
                        onActivate: uri => { searchBox.clear(); app.navigate(uri) }
                        onShowErrors: errorsDialog.open()
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.minimumWidth: 240; Layout.fillHeight: true; spacing: 8
                    Rectangle {
                        id: filterBar
                        objectName: "searchFilterBar"
                        visible: filtersVisible
                        Layout.fillWidth: true; Layout.preferredHeight: filterLayout.implicitHeight + 20
                        radius: 11
                        color: theme.sunken
                        border.width: 1; border.color: line
                        readonly property bool active: scopeBox.currentIndex > 0 || typeBox.currentIndex > 0 || minSize.text || maxSize.text || afterDate.text || beforeDate.text
                        function clear() {
                            scopeBox.currentIndex = 0; typeBox.currentIndex = 0
                            minSize.clear(); maxSize.clear(); afterDate.clear(); beforeDate.clear()
                            searchTimer.restart()
                        }
                        opacity: visible ? 1 : 0
                        Behavior on opacity { enabled: prefs.animations; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                        Flow {
                            id: filterLayout
                            anchors.fill: parent; anchors.margins: 10; anchors.leftMargin: 14; anchors.rightMargin: 12
                            spacing: 18
                            FilterGroup {
                                title: "范围"
                                NookComboBox { id: scopeBox; objectName: "filterScope"; width: 138; height: 34; model: ["全部本地位置", "当前目录内"]; onActivated: searchTimer.restart() }
                            }
                            FilterGroup {
                                title: "类型"
                                NookComboBox { id: typeBox; objectName: "filterType"; width: 112; height: 34; model: ["所有类型", "文件夹", "文件", "图片", "文档", "视频", "音频"]; onActivated: searchTimer.restart() }
                            }
                            FilterGroup {
                                title: "大小"
                                FilterField { id: minSize; width: 92; placeholderText: "最小"; suffix: "MB"; validator: DoubleValidator { bottom: 0; notation: DoubleValidator.StandardNotation } }
                                Text { text: "–"; color: muted; font.pixelSize: 13; height: 34; verticalAlignment: Text.AlignVCenter }
                                FilterField { id: maxSize; width: 92; placeholderText: "最大"; suffix: "MB"; validator: DoubleValidator { bottom: 0; notation: DoubleValidator.StandardNotation } }
                            }
                            FilterGroup {
                                title: "修改日期"
                                DateField { id: afterDate; objectName: "afterDateField"; rangeStart: valid ? text : ""; rangeEnd: beforeDate.valid ? beforeDate.text : "" }
                                Text { text: "至"; color: muted; font.pixelSize: 12; height: 34; verticalAlignment: Text.AlignVCenter }
                                DateField { id: beforeDate; objectName: "beforeDateField"; rangeStart: afterDate.valid ? afterDate.text : ""; rangeEnd: valid ? text : "" }
                            }
                            ActionButton {
                                objectName: "clearFiltersButton"
                                visible: filterBar.active
                                text: "清除筛选"; iconName: "close"; foreground: ink; implicitHeight: 34
                                onClicked: filterBar.clear()
                            }
                        }
                    }
                    SplitView {
                        id: splitView
                        Layout.fillWidth: true; Layout.fillHeight: true
                        orientation: Qt.Horizontal
                        handle: Rectangle { implicitWidth: 9; color: "transparent"; Rectangle { anchors.centerIn: parent; width: 2; height: 28; radius: 1; color: SplitHandle.hovered ? accent : line } }
                        FilePane {
                            id: leftPane; objectName: "leftPane"
                            SplitView.fillWidth: true; SplitView.minimumWidth: 240
                            directoryModel: window.searching ? app.search.model : app.left
                            controller: app; gridMode: app.grid; focusedPane: app.activeSide === 0; searchMode: window.searching; dark: window.dark; dense: prefs.dense
                            onFocused: app.activeSide = 0
                            onActivated: entry => window.activate(entry)
                            onPreviewRequested: entry => window.preview(entry)
                            onColumnsSaved: window.toast("当前列宽已保存")
                            onContextRequested: (model, x, y) => window.showContext(model, leftPane, x, y)
                            onBlankContextRequested: (model, x, y) => window.showBlankContext(model, leftPane, x, y)
                            onHovered: entry => window.hoverEntry(entry)
                            onSpringRequested: uri => window.springLoad(() => { app.activeSide = 0; app.navigate(uri) })
                        }
                        FilePane {
                            id: rightPane; objectName: "rightPane"
                            visible: app.dual && !window.searching
                            SplitView.fillWidth: false; SplitView.preferredWidth: splitView.width / 2; SplitView.minimumWidth: visible ? 240 : 0
                            directoryModel: app.right; controller: app; gridMode: app.grid; focusedPane: app.activeSide === 1; dark: window.dark; dense: prefs.dense
                            onFocused: app.activeSide = 1
                            onActivated: entry => window.activate(entry)
                            onPreviewRequested: entry => window.preview(entry)
                            onColumnsSaved: window.toast("当前列宽已保存")
                            onContextRequested: (model, x, y) => window.showContext(model, rightPane, x, y)
                            onBlankContextRequested: (model, x, y) => window.showBlankContext(model, rightPane, x, y)
                            onHovered: entry => window.hoverEntry(entry)
                            onSpringRequested: uri => window.springLoad(() => { app.activeSide = 1; app.navigate(uri) })
                        }
                    }
                    ActionButton { visible: searching && app.search.hasMore; text: "加载更多结果"; foreground: ink; Layout.alignment: Qt.AlignHCenter; onClicked: app.search.nextPage() }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: app.operations.busy || app.operations.errors.length ? 43 : 29
                color: theme.statusBar
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 18; spacing: 12
                    Rectangle { width: 5; height: 5; radius: 3; color: app.search.scanning ? theme.warning : theme.success }
                    Text { text: app.operations.busy || app.operations.errors.length ? app.operations.status : currentModel().count.toLocaleString(Qt.locale(), 'f', 0) + " 个项目" + (currentModel().selectedCount ? " · 选中 " + currentModel().selectedCount + " 项" : ""); color: muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    ActionButton { visible: app.operations.errors.length > 0; text: "查看错误"; foreground: theme.warning; implicitHeight: 25; onClicked: errorsDialog.open() }
                    ActionButton { visible: app.operations.busy; text: "取消任务"; foreground: theme.warning; implicitHeight: 25; onClicked: app.operations.cancel() }
                    Text { visible: !app.operations.busy; text: searching ? app.search.status : "CachyOS  /  Wayland"; color: muted; font.pixelSize: 12; elide: Text.ElideRight; Layout.maximumWidth: 350 }
                }
            }
        }
    }
    Shortcut { sequence: "Ctrl+A"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: currentModel().selectAll() }
    Shortcut { sequence: "Ctrl+C"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: app.copy(false, currentModel()) }
    Shortcut { sequence: "Ctrl+X"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: app.copy(true, currentModel()) }
    Shortcut { sequence: "Ctrl+V"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: app.paste() }
    Shortcut { sequence: "Ctrl+Shift+C"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive) && currentModel().selectedCount > 0; onActivated: app.copyPaths(currentModel().selectedUris) }
    Shortcut { sequence: "F2"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive) && currentModel().selectedCount === 1; onActivated: window.openInput("rename") }
    // Backspace goes up one level, as in Windows Explorer; never while typing in a field.
    Shortcut { sequence: "Backspace"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && !window.searching && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: app.activePane.up() }
    Shortcut { sequence: "Delete"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: window.removeSelection(false) }
    Shortcut { sequence: "Shift+Delete"; enabled: !app.preview.visible && !window.dialogsOpen && !window.menusOpen && !window.editingText && (leftPane.keyboardActive || rightPane.keyboardActive); onActivated: window.removeSelection(true) }

    NookMenu {
        id: contextMenu
        objectName: "fileContextMenu"
        NookMenuItem { text: "打开"; iconName: "folder"; enabled: !!contextEntry.fileUri; onTriggered: window.activate(contextEntry) }
        NookMenu {
            id: openWithMenu
            objectName: "openWithMenu"
            title: "打开方式"
            enabled: !!contextEntry.fileUri && !contextEntry.folder && contextModel && contextModel.selectedCount === 1
            NookMenuItem { text: "系统默认应用"; iconName: "document"; onTriggered: app.openExternal(contextEntry.fileUri) }
            MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
            Instantiator {
                model: contextApps
                delegate: NookMenuItem {
                    required property var modelData
                    text: modelData.name + (modelData.isDefault ? " · 默认" : "")
                    onTriggered: app.openWith(contextEntry.fileUri, modelData.id)
                }
                onObjectAdded: (index, object) => openWithMenu.insertItem(index + 2, object)
                onObjectRemoved: (index, object) => openWithMenu.removeItem(object)
            }
            NookMenuItem { text: "没有其他关联应用"; enabled: false; visible: contextApps.length === 0 }
        }
        NookMenuItem { text: "快速预览"; iconName: "document"; shortcutText: "Space"; onTriggered: window.preview(contextEntry) }
        NookMenuItem { objectName: "addQuickAccessMenuItem"; text: "添加到快速访问"; iconName: "add"; enabled: !!contextEntry.folder && contextModel && contextModel.selectedCount === 1 && !contextEntry.fileUri.startsWith("trash:"); onTriggered: app.addQuickAccess(contextEntry.fileUri) }
        NookMenuItem { text: "打开所在位置"; iconName: "folder"; visible: window.searching; onTriggered: { let uri = contextEntry.fileUri; searchBox.clear(); app.reveal(uri) } }
        NookMenuItem { objectName: "copyPathMenuItem"; text: contextModel && contextModel.selectedCount > 1 ? "复制完整路径（" + contextModel.selectedCount + " 项）" : "复制完整路径"; iconName: "copy"; shortcutText: "Ctrl Shift C"; enabled: contextModel && contextModel.selectedCount > 0; onTriggered: app.copyPaths(contextModel.selectedUris) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
        NookMenuItem { text: "复制"; iconName: "copy"; shortcutText: "Ctrl C"; onTriggered: app.copy(false, contextModel) }
        NookMenuItem { text: "剪切"; iconName: "cut"; shortcutText: "Ctrl X"; onTriggered: app.copy(true, contextModel) }
        NookMenuItem { text: "粘贴到此处"; iconName: "paste"; shortcutText: "Ctrl V"; enabled: !window.searching; onTriggered: app.paste() }
        NookMenuItem { text: "重命名"; iconName: "rename"; shortcutText: "F2"; enabled: currentModel().selectedCount === 1 && !app.activePane.uri.startsWith("trash:"); onTriggered: window.openInput("rename") }
        NookMenuItem { text: "从回收站恢复"; iconName: "refresh"; visible: app.activePane.uri.startsWith("trash:"); onTriggered: app.operations.run("restore", currentModel().selectedUris) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
        NookMenuItem { text: "移入回收站…"; iconName: "trash"; shortcutText: "Del"; visible: !app.activePane.uri.startsWith("trash:"); onTriggered: window.removeSelection(false) }
        NookMenuItem { objectName: "permanentDeleteMenuItem"; text: "彻底删除…"; iconName: "trash"; shortcutText: "Shift Del"; danger: true; onTriggered: window.removeSelection(true) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
        NookMenuItem { objectName: "filePropertiesMenuItem"; text: "属性"; iconName: "settings"; enabled: contextModel && contextModel.selectedCount === 1; onTriggered: window.inspectProperties(contextEntry.fileUri) }
    }
    NookMenu {
        id: blankMenu
        objectName: "blankContextMenu"
        NookMenu {
            title: "新建"; objectName: "createSubmenu"
            enabled: !window.searching && contextModel && !contextModel.uri.startsWith("trash:")
            NookMenuItem { objectName: "newFolderMenuItem"; text: "文件夹"; iconName: "folder"; onTriggered: { window.openInput("mkdir"); inputDestination = contextModel.uri } }
            NookMenuItem { objectName: "newTextMenuItem"; text: "文本文档"; iconName: "document"; onTriggered: { window.openInput("touch"); inputDestination = contextModel.uri } }
        }
        NookMenuItem { objectName: "blankTerminalMenuItem"; text: "在此处打开 kitty"; iconName: "terminal"; enabled: !window.searching && contextModel && !contextModel.uri.startsWith("trash:"); onTriggered: app.terminal(contextModel.uri) }
        NookMenuItem { text: "粘贴"; iconName: "paste"; enabled: !window.searching && contextModel && !contextModel.uri.startsWith("trash:"); onTriggered: app.paste() }
        NookMenuItem { objectName: "copyCurrentPathMenuItem"; text: "复制当前路径"; iconName: "copy"; enabled: !window.searching && !!contextModel; onTriggered: app.copyPaths([contextModel.uri]) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
        NookMenuItem { objectName: "blankPropertiesMenuItem"; text: "属性"; iconName: "settings"; enabled: !window.searching && !!contextModel; onTriggered: window.inspectProperties(contextModel.uri) }
    }
    NookMenu {
        id: quickAccessMenu
        NookMenuItem { objectName: "removeQuickAccessMenuItem"; text: "从快速访问移除"; iconName: "close"; onTriggered: app.removeQuickAccess(quickAccessUri) }
        NookMenuItem { text: "复制路径"; iconName: "copy"; onTriggered: app.copyPaths([quickAccessUri]) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: line; opacity: 0.7 } }
        NookMenuItem { objectName: "resetQuickAccessMenuItem"; text: "恢复默认快速访问项"; iconName: "refresh"; enabled: prefs.hiddenPlaces.length > 0; onTriggered: app.resetQuickAccess() }
    }
    Popup {
        id: nameDialog
        objectName: "nameDialog"
        anchors.centerIn: parent
        width: Math.min(456, window.layoutWidth - 40)
        modal: true; focus: true; padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        property string originalName: ""
        property string entryIcon: "file"
        property bool entryFolder: false
        property string renameParent: ""
        readonly property bool renaming: inputKind === "rename"
        readonly property string directory: renaming ? renameParent : inputDestination
        readonly property string proposed: nameInput.text
        readonly property bool unchanged: renaming && proposed === originalName
        // Case-only renames refer to the same entry; everything else must not collide.
        readonly property string problem: proposed.length === 0 ? "请输入名称"
            : proposed.indexOf("/") >= 0 ? "名称不能包含“/”"
            : proposed === "." || proposed === ".." ? "不能使用“.”或“..”作为名称"
            : proposed.trim().length === 0 ? "名称不能只包含空格"
            : !unchanged && proposed.toLowerCase() !== originalName.toLowerCase() && directory !== "" && !app.nameAvailable(directory, proposed) ? "此位置已存在“" + proposed + "”"
            : ""
        readonly property string notice: problem !== "" ? problem
            : proposed.startsWith(".") && !originalName.startsWith(".") ? "以“.”开头的名称在 Linux 中会被隐藏"
            : renaming && !entryFolder && originalName.lastIndexOf(".") > 0 && proposed.lastIndexOf(".") > 0
              && originalName.slice(originalName.lastIndexOf(".")) !== proposed.slice(proposed.lastIndexOf(".")) ? "更改扩展名可能导致文件无法用原来的应用打开"
            : "Enter 确认 · Esc 取消"
        function submit() {
            if (problem !== "") return
            if (!unchanged) app.operations.run(inputKind, currentModel().selectedUris, inputDestination, nameInput.text)
            close()
        }
        onClosed: window.focusActivePane()
        Overlay.modal: Rectangle { color: theme.scrim; Behavior on opacity { NumberAnimation { duration: 140 } } }
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: prefs.animations ? 150 : 0; easing.type: Easing.OutCubic }
                NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: prefs.animations ? 190 : 0; easing.type: Easing.OutBack; easing.overshoot: 1.1 }
            }
        }
        exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: prefs.animations ? 90 : 0 } }
        background: Item {
            Rectangle { anchors.fill: parent; anchors.topMargin: 10; anchors.margins: -2; radius: 20; color: theme.shadow }
            Rectangle { anchors.fill: parent; anchors.topMargin: 4; radius: 18; color: theme.shadow }
            Rectangle {
                anchors.fill: parent; radius: 18
                color: theme.raised
                border.width: 1; border.color: theme.line
                // A soft accent wash at the top edge gives the card a lit, layered feel.
                Rectangle {
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                    height: 76; radius: 17
                    gradient: Gradient {
                        GradientStop { position: 0; color: Qt.alpha(theme.accent, dark ? 0.14 : 0.10) }
                        GradientStop { position: 1; color: "transparent" }
                    }
                }
            }
        }
        contentItem: ColumnLayout {
            spacing: 0
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 22; Layout.bottomMargin: 18; spacing: 14
                Rectangle {
                    Layout.preferredWidth: 46; Layout.preferredHeight: 46; radius: 13
                    color: theme.accentSoft; border.width: 1; border.color: Qt.alpha(theme.accent, 0.35)
                    Icon { anchors.centerIn: parent; name: nameDialog.entryIcon; width: 26; height: 26 }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 3
                    Text {
                        text: inputKind === "mkdir" ? "新建文件夹" : inputKind === "touch" ? "新建文本文档" : nameDialog.entryFolder ? "重命名文件夹" : "重命名文件"
                        color: ink; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.fillWidth: true
                    }
                    Text {
                        text: nameDialog.renaming ? "原名称：" + nameDialog.originalName : "位置：" + app.localPath(inputDestination)
                        color: muted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true
                    }
                }
            }
            TextField {
                id: nameInput; objectName: "nameInput"
                ContextMenu.menu: null
                TextMenuArea { target: nameInput }
                Layout.fillWidth: true; Layout.leftMargin: 22; Layout.rightMargin: 22
                Layout.preferredHeight: 46
                selectByMouse: true
                font.pixelSize: 15; color: ink
                leftPadding: 14; rightPadding: 14
                selectionColor: theme.selection; selectedTextColor: theme.selectionText
                onAccepted: { if (!inputMethodComposing) nameDialog.submit() }
                background: Rectangle {
                    radius: 11
                    color: theme.field
                    border.width: nameInput.activeFocus ? 2 : 1
                    border.color: nameDialog.problem !== "" && nameInput.text !== "" ? theme.danger : nameInput.activeFocus ? accent : line
                    Behavior on border.color { ColorAnimation { duration: 120 } }
                    // Focus halo
                    Rectangle {
                        anchors.fill: parent; anchors.margins: -4; radius: 14; z: -1
                        color: "transparent"; border.width: 3
                        border.color: nameDialog.problem !== "" && nameInput.text !== "" ? Qt.alpha(theme.danger, 0.2) : theme.focusHalo
                        opacity: nameInput.activeFocus ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 140 } }
                    }
                }
            }
            Text {
                objectName: "nameNotice"
                Layout.fillWidth: true; Layout.leftMargin: 24; Layout.rightMargin: 22; Layout.topMargin: 9
                text: nameDialog.notice
                color: nameDialog.problem !== "" && nameInput.text !== "" ? theme.danger : nameDialog.notice.startsWith("Enter") ? muted : theme.warning
                font.pixelSize: 12; wrapMode: Text.Wrap
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: 20; height: 1; color: line; opacity: 0.8 }
            RowLayout {
                Layout.fillWidth: true; Layout.margins: 14; Layout.leftMargin: 22; Layout.rightMargin: 16; spacing: 8
                Item { Layout.fillWidth: true }
                ActionButton { text: "取消"; foreground: ink; implicitHeight: 38; onClicked: nameDialog.close() }
                ActionButton {
                    id: nameSubmit; objectName: "nameSubmitButton"
                    text: nameDialog.renaming ? "重命名" : "创建"; accent: true; implicitHeight: 38
                    enabled: nameDialog.problem === ""
                    onClicked: nameDialog.submit()
                }
            }
        }
    }
    Dialog {
        id: deleteDialog; objectName: "deleteDialog"; anchors.centerIn: parent; width: 420; modal: true
        title: permanent ? "永久删除选中项目？" : "移入回收站？"
        contentItem: ColumnLayout {
            spacing: 20
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: permanent ? "这将永久删除 " + deleteUris.length + " 个项目，无法从回收站恢复。" : "将 " + deleteUris.length + " 个项目移入回收站。回收站不可用时会显示错误，不会改为永久删除。" }
            RowLayout { Layout.alignment: Qt.AlignRight
                Button { text: "取消"; onClicked: deleteDialog.close() }
                Button { objectName: "confirmDeleteButton"; text: permanent ? "永久删除" : "移入回收站"; onClicked: { app.operations.run(permanent ? "delete" : "trash", deleteUris); deleteDialog.close() } }
            }
        }
    }
    Dialog {
        id: conflictDialog; objectName: "conflictDialog"; anchors.centerIn: parent; width: 480; modal: true; closePolicy: Popup.NoAutoClose
        title: "目标位置已存在同名项目"
        contentItem: ColumnLayout {
            spacing: 16
            Label { text: app.operations.conflictName; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
            Label { visible: app.operations.conflictFolder; text: "文件夹不会自动合并或替换。"; color: muted; font.pixelSize: 13 }
            RowLayout {
                Button { text: "取消任务"; onClicked: app.operations.cancel() }
                Item { Layout.fillWidth: true }
                Button { text: "跳过"; onClicked: app.operations.resolveConflict(0) }
                Button { text: "保留两者"; onClicked: app.operations.resolveConflict(2) }
                Button { text: "替换文件"; enabled: !app.operations.conflictFolder; onClicked: app.operations.resolveConflict(1) }
            }
        }
    }
    Dialog {
        id: closeDialog; anchors.centerIn: parent; width: 420; modal: true
        title: "文件任务尚未结束"
        contentItem: ColumnLayout {
            spacing: 16
            Label { text: "可以继续等待，或取消未完成的任务后关闭。已完成的项目会保留。"; wrapMode: Text.Wrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                Button { text: "继续任务"; onClicked: closeDialog.close() }
                Button { text: "取消任务并关闭"; onClicked: { closingAfterTask = true; app.operations.cancel(); closeDialog.close() } }
            }
        }
    }
    Dialog {
        id: errorsDialog; objectName: "errorsDialog"; anchors.centerIn: parent; width: 600; height: Math.min(440, window.layoutHeight - 60); modal: true
        title: "任务错误 · 已完成的项目不会回滚"
        contentItem: ScrollView {
            TextArea {
                id: errorsText
                ContextMenu.menu: null
                TextMenuArea { target: errorsText }
                selectionColor: theme.selection; selectedTextColor: theme.selectionText
                text: app.operations.errors.join("\n\n"); readOnly: true; wrapMode: TextEdit.WrapAnywhere; selectByMouse: true; font.pixelSize: 15
            }
        }
        footer: Item { implicitHeight: 46; Button { anchors.right: parent.right; anchors.rightMargin: 12; text: "关闭"; onClicked: errorsDialog.close() } }
    }
    PropertiesDialog { id: propertiesDialog; controller: app.properties; host: window }
    SettingsPanel {
        id: settingsDialog
        controller: app; host: window
        onReindexRequested: { app.search.stop(); applyIndexTimer.start() }
    }

    Rectangle {
        id: previewOverlay
        anchors.fill: parent; z: 20
        visible: app.preview.visible
        color: theme.scrim
        function constrainPanel() {
            previewPanel.width = Math.max(440, Math.min(previewPanel.width, width - 32))
            previewPanel.height = Math.max(280, Math.min(previewPanel.height, height - 32))
            previewPanel.x = Math.max(16, Math.min(previewPanel.x, width - previewPanel.width - 16))
            previewPanel.y = Math.max(16, Math.min(previewPanel.y, height - previewPanel.height - 16))
        }
        onWidthChanged: if (visible) constrainPanel()
        onHeightChanged: if (visible) constrainPanel()
        // The preview worker starts in the same moment; render-thread animators keep the
        // open transition smooth even while the GUI thread handles the incoming result.
        ParallelAnimation {
            id: previewEnter
            OpacityAnimator { target: previewOverlay; from: 0; to: 1; duration: 150; easing.type: Easing.OutCubic }
            OpacityAnimator { target: previewPanel; from: 0; to: 1; duration: 170; easing.type: Easing.OutCubic }
            ScaleAnimator { target: previewPanel; from: 0.965; to: 1; duration: 210; easing.type: Easing.OutCubic }
        }
        onVisibleChanged: {
            hoverTimer.stop()
            if (visible && prefs.animations) previewEnter.restart()
            if (visible) constrainPanel()
            else {
                suppressedHoverUri = app.preview.uri
                if (app.activeSide === 1 && rightPane.visible) rightPane.focusFiles()
                else leftPane.focusFiles()
            }
        }
        MouseArea { anchors.fill: parent; onClicked: app.preview.close() }
        Rectangle {
            id: previewPanel; objectName: "previewPanel"
            width: Math.min(910, window.layoutWidth - 100); height: Math.min(650, window.layoutHeight - 90)
            x: (previewOverlay.width - width) / 2; y: (previewOverlay.height - height) / 2
            radius: 14; color: surface; border.width: 1; border.color: theme.lineStrong
            MouseArea { anchors.fill: parent }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 17; spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    Icon { name: "document"; width: 19; height: 19 }
                    Item {
                        Layout.fillWidth: true; Layout.preferredHeight: 44; Layout.minimumWidth: 60
                        MouseArea {
                            objectName: "previewMoveHandle"
                            anchors.fill: parent; cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                            drag.target: previewPanel; drag.minimumX: 16; drag.maximumX: previewOverlay.width - previewPanel.width - 16
                            drag.minimumY: 16; drag.maximumY: previewOverlay.height - previewPanel.height - 16
                        }
                        ColumnLayout { anchors.fill: parent; spacing: 3
                        Text { text: app.preview.content.title || "快速预览"; color: ink; font.pixelSize: 17; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text { text: app.preview.content.subtitle || ""; color: muted; font.pixelSize: 12 }
                    }
                    }
                    ActionButton { text: previewPanel.width < 600 ? "打开" : "用系统应用打开"; foreground: muted; onClicked: app.openExternal(app.preview.uri) }
                    ActionButton { iconName: "close"; hint: "关闭预览 · Esc"; foreground: ink; onClicked: app.preview.close() }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: line }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    BusyIndicator { anchors.centerIn: parent; running: app.preview.loading; visible: running }
                    Flickable {
                        id: imageView
                        objectName: "previewImageView"
                        anchors.fill: parent; clip: true
                        visible: !app.preview.loading && !app.preview.content.error && app.preview.content.mode === "image"
                        // zoom is relative to "fit in view"; the wheel zooms about the pointer.
                        property real zoom: 1
                        readonly property real minZoom: 0.25
                        readonly property real maxZoom: 16
                        readonly property real fitScale: previewImage.implicitWidth > 0 && previewImage.implicitHeight > 0
                            ? Math.min(width / previewImage.implicitWidth, height / previewImage.implicitHeight) : 1
                        readonly property real pixelScale: fitScale * zoom
                        contentWidth: Math.max(width, previewImage.width)
                        contentHeight: Math.max(height, previewImage.height)
                        boundsBehavior: Flickable.StopAtBounds
                        interactive: previewImage.width > width || previewImage.height > height
                        function zoomTo(value, px, py) {
                            const next = Math.max(minZoom, Math.min(maxZoom, value))
                            if (next === zoom) return
                            // Keep the image point under the pointer fixed while scaling.
                            const ix = (contentX + px - previewImage.x) / previewImage.width
                            const iy = (contentY + py - previewImage.y) / previewImage.height
                            zoom = next
                            contentX = Math.max(0, Math.min(contentWidth - width, previewImage.x + ix * previewImage.width - px))
                            contentY = Math.max(0, Math.min(contentHeight - height, previewImage.y + iy * previewImage.height - py))
                        }
                        function reset() { zoom = 1; contentX = 0; contentY = 0 }
                        Image {
                            id: previewImage
                            asynchronous: true; smooth: true; mipmap: true
                            source: app.preview.content.image || ""
                            width: implicitWidth * imageView.pixelScale
                            height: implicitHeight * imageView.pixelScale
                            x: Math.max(0, (imageView.contentWidth - width) / 2)
                            y: Math.max(0, (imageView.contentHeight - height) / 2)
                            onSourceChanged: imageView.reset()
                        }
                        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        MouseArea {
                            // Wheel and double-click only; dragging is left to the Flickable for panning.
                            width: imageView.contentWidth; height: imageView.contentHeight
                            acceptedButtons: Qt.LeftButton
                            cursorShape: imageView.interactive ? Qt.OpenHandCursor : Qt.ArrowCursor
                            onWheel: wheel => {
                                const delta = wheel.pixelDelta.y !== 0 && wheel.angleDelta.y % 120 !== 0 ? wheel.pixelDelta.y * 1.6 : wheel.angleDelta.y
                                const p = mapToItem(imageView, wheel.x, wheel.y)
                                imageView.zoomTo(imageView.zoom * Math.pow(1.0015, delta), p.x, p.y)
                                wheel.accepted = true
                            }
                            onDoubleClicked: event => {
                                const p = mapToItem(imageView, event.x, event.y)
                                // Toggle between "fit" and the image's own pixels (or 2× when it already fits).
                                const actual = imageView.fitScale > 0 ? 1 / imageView.fitScale : 2
                                imageView.zoomTo(Math.abs(imageView.zoom - 1) > 0.01 ? 1 : (Math.abs(actual - 1) < 0.05 ? 2 : actual), p.x, p.y)
                            }
                        }
                    }
                    ScrollView {
                        id: textView
                        anchors.fill: parent; clip: true
                        visible: !app.preview.loading && !app.preview.content.error && app.preview.content.mode !== "image"
                        // Re-laying out up to 256 KiB of text per wheel notch stutters. While the
                        // wheel turns, only the rendered text is scaled on the GPU; the real font
                        // size (one layout pass) is committed once the gesture pauses.
                        property int fontSize: 16
                        property int targetSize: 16
                        function zoomText(size) {
                            targetSize = Math.max(10, Math.min(48, size))
                            commitZoom.restart()
                        }
                        function resetText() { commitZoom.stop(); targetSize = 16; fontSize = 16 }
                        Timer { id: commitZoom; interval: 160; onTriggered: textView.fontSize = textView.targetSize }
                        TextArea {
                            id: previewText
                            objectName: "previewText"
                            ContextMenu.menu: null
                            TextMenuArea { target: previewText }
                            selectionColor: theme.selection; selectedTextColor: theme.selectionText
                            transformOrigin: Item.TopLeft
                            scale: textView.targetSize / textView.fontSize
                            text: app.preview.content.text || "暂不支持此格式的内容预览，可使用系统应用打开。"
                            textFormat: app.preview.content.mode === "rich" ? TextEdit.RichText : TextEdit.PlainText
                            readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
                            color: ink; font.pixelSize: textView.fontSize; background: null
                            onLinkActivated: link => app.openExternal(link)
                            onTextChanged: textView.resetText()
                        }
                    }
                    MouseArea {
                        // Ctrl+wheel scales text; a plain wheel keeps scrolling the document.
                        anchors.fill: textView; visible: textView.visible
                        acceptedButtons: Qt.NoButton
                        onWheel: wheel => {
                            if (!(wheel.modifiers & Qt.ControlModifier)) { wheel.accepted = false; return }
                            textView.zoomText(textView.targetSize + (wheel.angleDelta.y > 0 ? 2 : -2))
                            wheel.accepted = true
                        }
                    }
                    Label { anchors.centerIn: parent; width: Math.min(430, parent.width); text: app.preview.content.error || ""; visible: !app.preview.loading && text !== ""; color: muted; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: app.preview.content.notice || (imageView.visible ? "滚轮缩放 · 拖动平移 · 双击切换适应/原始大小 · 空格关闭"
                                                                               : "Ctrl + 滚轮缩放文字 · 空格 / Esc 关闭 · 拖动边缘调整窗口")
                        color: muted; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight
                    }
                    Row {
                        visible: imageView.visible || textView.visible
                        spacing: 2
                        ActionButton { objectName: "previewZoomOut"; text: "−"; hint: "缩小"; foreground: ink; implicitWidth: 32; implicitHeight: 30
                            onClicked: imageView.visible ? imageView.zoomTo(imageView.zoom / 1.25, imageView.width / 2, imageView.height / 2) : textView.zoomText(textView.targetSize - 2) }
                        ActionButton {
                            objectName: "previewZoomReset"
                            text: imageView.visible ? Math.round(imageView.zoom * 100) + "%" : Math.round(textView.targetSize / 16 * 100) + "%"
                            hint: "恢复为适应窗口"; foreground: muted; implicitHeight: 30; implicitWidth: 62
                            onClicked: { imageView.reset(); textView.resetText() }
                        }
                        ActionButton { objectName: "previewZoomIn"; text: "+"; hint: "放大"; foreground: ink; implicitWidth: 32; implicitHeight: 30
                            onClicked: imageView.visible ? imageView.zoomTo(imageView.zoom * 1.25, imageView.width / 2, imageView.height / 2) : textView.zoomText(textView.targetSize + 2) }
                    }
                    ActionButton { visible: (app.preview.content.pages || 1) > 1; iconName: "back"; hint: "上一页"; foreground: ink; enabled: app.preview.page > 1 && !app.preview.loading; onClicked: app.preview.open(app.preview.uri, app.preview.page - 1) }
                    Text { visible: (app.preview.content.pages || 1) > 1; text: app.preview.page + " / " + app.preview.content.pages; color: muted; font.pixelSize: 13 }
                    ActionButton { visible: (app.preview.content.pages || 1) > 1; iconName: "forward"; hint: "下一页"; foreground: ink; enabled: app.preview.page < (app.preview.content.pages || 1) && !app.preview.loading; onClicked: app.preview.open(app.preview.uri, app.preview.page + 1) }
                }
            }
            Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6; text: "◢"; color: muted; font.pixelSize: 12 }
            Repeater {
                model: [{l: true, t: true}, {r: true, t: true}, {l: true, b: true}, {r: true, b: true}, {l: true}, {r: true}, {t: true}, {b: true}]
                MouseArea {
                    id: edge
                    required property var modelData
                    required property int index
                    objectName: "previewResizeHandle" + index
                    readonly property bool corner: !!((modelData.l || modelData.r) && (modelData.t || modelData.b))
                    x: modelData.r ? previewPanel.width - width : modelData.l ? 0 : 12
                    y: modelData.b ? previewPanel.height - height : modelData.t ? 0 : 12
                    width: modelData.l || modelData.r ? 10 : previewPanel.width - 24
                    height: modelData.t || modelData.b ? 10 : previewPanel.height - 24
                    z: 5; preventStealing: true
                    cursorShape: corner ? ((modelData.l && modelData.t) || (modelData.r && modelData.b) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor) : modelData.l || modelData.r ? Qt.SizeHorCursor : Qt.SizeVerCursor
                    property var origin: ({})
                    property point start
                    onPressed: event => {
                        origin = {x: previewPanel.x, y: previewPanel.y, w: previewPanel.width, h: previewPanel.height}
                        start = mapToItem(previewOverlay, event.x, event.y)
                    }
                    onPositionChanged: event => {
                        if (!pressed) return
                        let point = mapToItem(previewOverlay, event.x, event.y)
                        let dx = point.x - start.x, dy = point.y - start.y
                        let x = origin.x, y = origin.y, w = origin.w, h = origin.h
                        if (modelData.l) { x = Math.max(16, Math.min(origin.x + dx, origin.x + origin.w - 440)); w = origin.x + origin.w - x }
                        if (modelData.r) w = Math.max(440, Math.min(origin.w + dx, previewOverlay.width - x - 16))
                        if (modelData.t) { y = Math.max(16, Math.min(origin.y + dy, origin.y + origin.h - 280)); h = origin.y + origin.h - y }
                        if (modelData.b) h = Math.max(280, Math.min(origin.h + dy, previewOverlay.height - y - 16))
                        previewPanel.x = x; previewPanel.y = y; previewPanel.width = w; previewPanel.height = h
                    }
                }
            }
        }
    }
    // Source of every file drag (see startFileDrag): it outlives the rows and folders the drag
    // passes through.
    Item {
        id: fileDrag
        objectName: "fileDragSource"
        property var fileUris: []
        property string iconName: "file"
        width: 1; height: 1; visible: false
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
        Drag.proposedAction: Qt.CopyAction
        Drag.imageSource: "icons/" + iconName + ".svg"
        Drag.onDragFinished: action => window.fileDragFinished(action)
    }
    Rectangle {
        objectName: "windowDropHint"
        visible: dropHintText !== ""
        z: 90
        x: Math.min(window.layoutWidth - width - 8, Math.max(8, dropHintX + 18))
        y: Math.min(window.layoutHeight - height - 8, Math.max(8, dropHintY + 20))
        width: windowHintLabel.implicitWidth + 22; height: 28; radius: 8
        color: theme.raised; border.color: theme.lineStrong
        Text { id: windowHintLabel; anchors.centerIn: parent; text: dropHintText; color: ink; font.pixelSize: 13 }
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 45
        id: toastCard
        z: 100; visible: toastMessage !== ""
        ParallelAnimation {
            id: toastEnter
            OpacityAnimator { target: toastCard; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
            ScaleAnimator { target: toastCard; from: 0.94; to: 1; duration: 200; easing.type: Easing.OutBack }
        }
        SequentialAnimation {
            id: toastLeave
            OpacityAnimator { target: toastCard; to: 0; duration: 180; easing.type: Easing.InCubic }
            ScriptAction { script: { window.toastMessage = ""; toastCard.opacity = 1 } }
        }
        width: Math.min(window.layoutWidth - 70, toastLabel.implicitWidth + 36); height: toastLabel.implicitHeight + 24; radius: 9
        color: theme.raised; border.color: theme.lineStrong
        Text { id: toastLabel; anchors.centerIn: parent; text: toastMessage; color: ink; font.pixelSize: 15; width: Math.min(implicitWidth, window.layoutWidth - 106); wrapMode: Text.Wrap }
    }
    }
}
