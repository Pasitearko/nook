import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: pane
    required property var directoryModel
    required property var controller
    property bool gridMode: false
    property bool focusedPane: false
    property bool searchMode: false
    property bool dark: true
    property bool dense: false
    property bool thumbnails: true
    property bool showHeader: true // folder title, column-width button and item count
    property string emptyText: "这个文件夹是空的"
    readonly property var theme: ApplicationWindow.window.theme
    readonly property var host: ApplicationWindow.window
    // Selection mode (toolbar): a click toggles an item, a drag from anywhere draws a band,
    // and nothing can be dragged away.
    readonly property bool pickMode: !!host && host.selectionMode === true
    // While a dialog or the preview covers the pane, nothing in it may start a file drag:
    // selecting text in the Properties dialog must never drag the row underneath out as an icon.
    readonly property bool covered: {
        const w = ApplicationWindow.window
        return !!w && (w.dialogsOpen || w.controller.preview.visible)
    }
    property color ink: theme.ink
    property color muted: theme.muted
    readonly property bool keyboardActive: keyArea.activeFocus && ApplicationWindow.window && ApplicationWindow.window.activeFocusItem === keyArea
    property int currentRow: -1
    onCurrentRowChanged: { list.currentIndex = currentRow; grid.currentIndex = currentRow }
    readonly property bool animated: controller.settings.values.animations
    property int dropRow: -1
    property int springRow: -1 // the folder that will open if the drag keeps hovering over it
    // Row transitions only for in-place refreshes; a freshly entered folder fades in as a whole.
    property bool populating: false
    readonly property bool rowAnimations: animated && !populating
    function revealContents() {
        if (!populating) return
        settleTimer.restart()
        if (animated) enterFade.restart(); else files.opacity = 1
    }
    Timer { id: settleTimer; interval: 80; onTriggered: pane.populating = false }
    // Runs on the render thread, so it stays smooth while the GUI thread fills the model.
    OpacityAnimator { id: enterFade; target: files; from: 0; to: 1; duration: 170; easing.type: Easing.OutCubic }
    Connections {
        target: pane.directoryModel
        function onLocationChanged() {
            pane.populating = true
            settleTimer.stop()
            if (pane.animated) { enterFade.stop(); files.opacity = 0 }
        }
        // Reveal on the first real rows, or once loading ends (empty folder / error text).
        function onCountChanged() { if (pane.directoryModel.count > 0) pane.revealContents() }
        function onStatusChanged() { if (!pane.directoryModel.loading) pane.revealContents() }
    }
    property var localWidths: []
    onDirectoryModelChanged: { localWidths = []; currentRow = -1; sortColumn = -1 }
    // Keys belong to the file list itself, never to a control that once had focus in the pane.
    function focusFiles() {
        const w = ApplicationWindow.window
        const focused = w ? w.activeFocusItem : null
        for (let item = focused ? focused.parent : null; item; item = item.parent)
            if (item === keyArea) { focused.focus = false; break }
        keyArea.forceActiveFocus()
    }
    property bool descending: false
    property int sortColumn: -1
    property bool useSavedWidths: true // false: columns fit this pane (the file chooser)
    readonly property var savedWidths: useSavedWidths ? controller.settings.values.columnWidths || [] : []
    readonly property string savedSignature: JSON.stringify(savedWidths)
    onSavedSignatureChanged: localWidths = []
    readonly property real columnsWidth: columnWidth(0) + columnWidth(1) + columnWidth(2) + columnWidth(3)
    function columnWidth(index) {
        const minimums = [140, 100, 70, 65]
        const defaults = [Math.max(200, pane.width - 370), 150, 100, 85]
        const widths = localWidths.length === 4 ? localWidths : savedWidths
        return widths.length === 4 ? Math.max(minimums[index], Math.min(1600, Number(widths[index]) || defaults[index])) : defaults[index]
    }
    function resizeColumn(index, value) {
        let widths = [columnWidth(0), columnWidth(1), columnWidth(2), columnWidth(3)]
        widths[index] = Math.max([140, 100, 70, 65][index], Math.min(1600, value))
        localWidths = widths
    }
    function saveColumns() {
        controller.settings.set("columnWidths", [columnWidth(0), columnWidth(1), columnWidth(2), columnWidth(3)])
        localWidths = []
        columnsSaved()
    }
    Connections {
        target: pane.directoryModel
        function onLocationChanged() { pane.localWidths = []; pane.currentRow = -1 }
    }
    Connections {
        target: pane.controller.settings
        function onColumnWidthsReset() { pane.localWidths = [] }
    }
    signal columnsSaved()
    signal activated(var entry)
    signal previewRequested(var entry)
    signal contextRequested(var model, real x, real y)
    signal blankContextRequested(var model, real x, real y)
    signal focused()
    signal hovered(var entry)
    // View delegates survive removal in the reuse pool. Hover belongs only to a live row.
    property Item hoverOwner: null
    signal springRequested(string uri)
    color: theme.pane
    radius: 10
    border.width: 1
    border.color: focusedPane ? theme.lineStrong : theme.line
    clip: true
    Accessible.role: Accessible.Pane
    Accessible.name: searchMode ? "搜索结果" : directoryModel.title
    function selectRow(index, modifiers) {
        currentRow = index
        pane.focused()
        directoryModel.select(index, ((modifiers & Qt.ControlModifier) ? 1 : 0) | ((modifiers & Qt.ShiftModifier) ? 2 : 0))
        pane.focusFiles()
    }
    function toggleRow(index, modifiers) {
        currentRow = index
        pane.focused()
        directoryModel.select(index, 1 | ((modifiers & Qt.ShiftModifier) ? 2 : 0))
        pane.focusFiles()
    }
    function currentEntry() { return directoryModel.firstSelected() }
    readonly property real rowHeight: searchMode ? 62 : (dense ? 36 : 43)
    // Rows whose visual box intersects the band (content coordinates), for list or grid.
    function rowsIn(x1, y1, x2, y2) {
        let rows = []
        const count = directoryModel.count
        if (!gridMode) {
            if (x1 > columnsWidth + 18 || x2 < 9) return rows
            const first = Math.max(0, Math.floor(y1 / rowHeight)), last = Math.min(count - 1, Math.floor(y2 / rowHeight))
            for (let i = first; i <= last; ++i) rows.push(i)
            return rows
        }
        const cw = grid.cellWidth, ch = grid.cellHeight, cols = Math.max(1, Math.floor(grid.width / cw)), inset = 4
        const r1 = Math.max(0, Math.floor(y1 / ch)), r2 = Math.floor(y2 / ch)
        const c1 = Math.max(0, Math.floor(x1 / cw)), c2 = Math.min(cols - 1, Math.floor(x2 / cw))
        for (let r = r1; r <= r2; ++r)
            for (let c = c1; c <= c2; ++c) {
                const i = r * cols + c
                if (i >= count) break
                if (x2 < c * cw + inset || x1 > (c + 1) * cw - inset || y2 < r * ch + inset || y1 > (r + 1) * ch - inset) continue
                rows.push(i)
            }
        return rows
    }
    FocusScope {
        id: keyArea
        anchors.fill: parent
        focus: pane.focusedPane
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Down || event.key === Qt.Key_Up) {
                let step = pane.gridMode ? Math.max(1, Math.floor(files.width / 176)) : 1
                let row = Math.max(0, Math.min(directoryModel.count - 1, pane.currentRow + (event.key === Qt.Key_Down ? step : -step)))
                pane.selectRow(row, event.modifiers)
                if (pane.gridMode) grid.positionViewAtIndex(row, GridView.Contain)
                else list.positionViewAtIndex(row, ListView.Contain)
                event.accepted = true
            } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                pane.activated(pane.currentEntry()); event.accepted = true
            }
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0
            Rectangle {
                visible: pane.showHeader
                Layout.fillWidth: true; Layout.preferredHeight: 45
                color: "transparent"
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 15; anchors.rightMargin: 12
                    Icon { name: pane.searchMode ? "search" : "folder"; width: 16; height: 16 }
                    Text { text: pane.searchMode ? "搜索结果" : directoryModel.title; color: pane.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle }
                    ActionButton {
                        objectName: "saveColumnWidthsButton"
                        // Never a focus stop: inside the pane's FocusScope it would keep coming back as
                        // the focused child, and Enter/Space meant for the files would save widths.
                        focusPolicy: Qt.NoFocus
                        visible: !pane.gridMode
                        text: "保存列宽"; iconName: "list"; implicitHeight: 28
                        foreground: pane.theme.accentText
                        hint: "将当前列宽设为默认，下次继续使用"
                        onClicked: { pane.saveColumns(); pane.focusFiles() }
                    }
                    Text {
                        objectName: "paneCount"
                        visible: pane.width > 460
                        text: (pane.pickMode ? "已选 " + directoryModel.selectedCount.toLocaleString() + " / " : "") + directoryModel.count.toLocaleString() + " 项"
                        color: pane.pickMode ? pane.theme.accentText : pane.muted; font.pixelSize: 13
                    }
                    BusyIndicator { running: directoryModel.loading; visible: running; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                }
            }
            Rectangle {
                visible: !pane.gridMode
                Layout.fillWidth: true; Layout.preferredHeight: 34
                color: pane.theme.sunken
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: pane.theme.line }
                clip: true
                Row {
                    x: 14 - list.contentX; height: parent.height
                    Repeater {
                        model: [pane.searchMode ? "名称 / 位置" : "名称", "修改时间", "类型", "大小"]
                        Item {
                            id: column
                            required property int index
                            required property string modelData
                            width: pane.columnWidth(index); height: 34
                            Text { x: 7; width: parent.width - 20; anchors.verticalCenter: parent.verticalCenter; text: column.modelData + (pane.sortColumn === column.index ? (pane.descending ? "  ↓" : "  ↑") : ""); color: pane.muted; font.pixelSize: 13; elide: Text.ElideRight }
                            MouseArea {
                                anchors.fill: parent; anchors.rightMargin: 7
                                enabled: !pane.searchMode
                                onClicked: {
                                    pane.descending = pane.sortColumn === column.index ? !pane.descending : false
                                    pane.sortColumn = column.index
                                    directoryModel.sortBy(["name", "date", "type", "size"][column.index], pane.descending)
                                }
                            }
                            Rectangle { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; width: 1; height: 16; color: handle.containsMouse || handle.pressed ? pane.theme.accent : pane.theme.lineStrong }
                            MouseArea {
                                id: handle
                                objectName: "columnResizeHandle" + column.index
                                anchors.right: parent.right; width: 10; height: parent.height
                                cursorShape: Qt.SplitHCursor; hoverEnabled: true
                                property real startX: 0
                                property real startWidth: 0
                                preventStealing: true
                                onPressed: event => { startX = mapToItem(pane, event.x, event.y).x; startWidth = pane.columnWidth(column.index) }
                                onPositionChanged: event => { if (pressed) pane.resizeColumn(column.index, startWidth + mapToItem(pane, event.x, event.y).x - startX) }
                                onDoubleClicked: pane.localWidths = []
                            }
                        }
                    }
                }
            }
            Item {
                id: files
                Layout.fillWidth: true; Layout.fillHeight: true
                // Rows kept in the cache buffer above the viewport must not paint over, or take
                // clicks from, the column header.
                clip: true
                ListView {
                    id: list
                    objectName: "fileList"
                    anchors.fill: parent; anchors.margins: 5
                    contentWidth: Math.max(width, pane.columnsWidth + 24)
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    visible: !pane.gridMode
                    model: pane.gridMode ? null : directoryModel
                    reuseItems: true
                    cacheBuffer: 100
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    highlightFollowsCurrentItem: false
                    highlight: selectionHighlight
                    add: Transition {
                        enabled: pane.rowAnimations
                        ParallelAnimation {
                            OpacityAnimator { from: 0; to: 1; duration: 220; easing.type: Easing.OutCubic }
                            ScaleAnimator { from: 0.96; to: 1; duration: 220; easing.type: Easing.OutCubic }
                        }
                    }
                    remove: Transition {
                        enabled: pane.rowAnimations
                        ParallelAnimation {
                            OpacityAnimator { to: 0; duration: 160; easing.type: Easing.InCubic }
                            ScaleAnimator { to: 0.96; duration: 160; easing.type: Easing.InCubic }
                        }
                    }
                    // Displacement is retargeted while rows keep moving, so it stays a GUI-side
                    // animation; it also restores rows whose add/remove fade was interrupted.
                    displaced: Transition {
                        enabled: pane.rowAnimations
                        NumberAnimation { properties: "x,y"; duration: 220; easing.type: Easing.OutCubic }
                        NumberAnimation { property: "opacity"; to: 1; duration: 120 }
                        NumberAnimation { property: "scale"; to: 1; duration: 120 }
                    }
                    move: Transition {
                        enabled: pane.rowAnimations
                        NumberAnimation { properties: "x,y"; duration: 240; easing.type: Easing.OutCubic }
                        NumberAnimation { property: "opacity"; to: 1; duration: 120 }
                        NumberAnimation { property: "scale"; to: 1; duration: 120 }
                    }
                    delegate: fileDelegate
                    onCurrentIndexChanged: if (!pane.gridMode && currentIndex !== pane.currentRow) pane.currentRow = currentIndex
                }
                GridView {
                    id: grid
                    anchors.fill: parent; anchors.margins: 8
                    visible: pane.gridMode
                    model: pane.gridMode ? directoryModel : null
                    objectName: "fileGrid"
                    onCurrentIndexChanged: if (pane.gridMode && currentIndex !== pane.currentRow) pane.currentRow = currentIndex
                    cellWidth: 176; cellHeight: 180
                    reuseItems: true; cacheBuffer: 180
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    highlightFollowsCurrentItem: false
                    highlight: selectionHighlight
                    add: Transition {
                        enabled: pane.rowAnimations
                        ParallelAnimation {
                            OpacityAnimator { from: 0; to: 1; duration: 220; easing.type: Easing.OutCubic }
                            ScaleAnimator { from: 0.96; to: 1; duration: 220; easing.type: Easing.OutCubic }
                        }
                    }
                    remove: Transition {
                        enabled: pane.rowAnimations
                        ParallelAnimation {
                            OpacityAnimator { to: 0; duration: 160; easing.type: Easing.InCubic }
                            ScaleAnimator { to: 0.96; duration: 160; easing.type: Easing.InCubic }
                        }
                    }
                    // Displacement is retargeted while rows keep moving, so it stays a GUI-side
                    // animation; it also restores rows whose add/remove fade was interrupted.
                    displaced: Transition {
                        enabled: pane.rowAnimations
                        NumberAnimation { properties: "x,y"; duration: 220; easing.type: Easing.OutCubic }
                        NumberAnimation { property: "opacity"; to: 1; duration: 120 }
                        NumberAnimation { property: "scale"; to: 1; duration: 120 }
                    }
                    move: Transition {
                        enabled: pane.rowAnimations
                        NumberAnimation { properties: "x,y"; duration: 240; easing.type: Easing.OutCubic }
                        NumberAnimation { property: "opacity"; to: 1; duration: 120 }
                        NumberAnimation { property: "scale"; to: 1; duration: 120 }
                    }
                    delegate: fileDelegate
                }
                // Blank-space pointer: click clears, right-click opens the blank menu, and a left
                // drag draws a Windows-style selection band (Ctrl adds to the current selection).
                // In the list view the date/type/size cells start a band too (only the name drags
                // files, as in Explorer's details view); in selection mode every press does, bands
                // flip what they cover, and clicks toggle items instead of replacing the selection.
                MouseArea {
                    id: band
                    objectName: "emptyAreaPointer"
                    anchors.fill: parent; z: 2
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true
                    property bool selecting: false
                    property bool dragged: false
                    property point origin: Qt.point(0, 0)   // view content coordinates
                    property point pointer: Qt.point(0, 0)  // local coordinates
                    property var base: []
                    property int pressRow: -1
                    readonly property var view: pane.gridMode ? grid : list
                    function contentPoint(x, y) {
                        const p = mapToItem(view, x, y)
                        return Qt.point(p.x + view.contentX - view.originX, p.y + view.contentY - view.originY)
                    }
                    function onScrollbar(x, y) {
                        const p = mapToItem(view, x, y)
                        return p.x >= view.width - 16 || p.y >= view.height - 16
                    }
                    function rowAt(x, y) {
                        const p = mapToItem(view, x, y)
                        return view.indexAt(p.x + view.contentX, p.y + view.contentY)
                    }
                    function emptyAt(x, y) {
                        if (onScrollbar(x, y)) return false
                        const p = mapToItem(view, x, y)
                        if (!pane.gridMode && p.x + view.contentX > pane.columnsWidth + 18) return true
                        return rowAt(x, y) < 0
                    }
                    function bandAt(x, y) {
                        if (onScrollbar(x, y)) return false
                        if (pane.pickMode || emptyAt(x, y)) return true
                        if (pane.gridMode) return false
                        return mapToItem(view, x, y).x + view.contentX > 9 + pane.columnWidth(0)
                    }
                    function refreshBand() {
                        const c = contentPoint(pointer.x, pointer.y)
                        directoryModel.selectRows(pane.rowsIn(Math.min(origin.x, c.x), Math.min(origin.y, c.y),
                                                              Math.max(origin.x, c.x), Math.max(origin.y, c.y)), base, pane.pickMode)
                    }
                    onPressed: event => {
                        // Right presses on an item belong to it (its context menu).
                        const mine = event.button === Qt.RightButton ? emptyAt(event.x, event.y) : bandAt(event.x, event.y)
                        if (!mine) { event.accepted = false; return }
                        selecting = false; dragged = false
                        pressRow = emptyAt(event.x, event.y) ? -1 : rowAt(event.x, event.y)
                        origin = contentPoint(event.x, event.y); pointer = Qt.point(event.x, event.y)
                        // Selection mode bands flip what they cover (picked items are let go), keeping
                        // everything outside the band as it was.
                        base = pane.pickMode || (event.modifiers & Qt.ControlModifier) ? directoryModel.selectedUris.slice() : []
                        pane.focused(); pane.focusFiles()
                    }
                    onPositionChanged: event => {
                        if (!(pressedButtons & Qt.LeftButton)) return
                        pointer = Qt.point(event.x, event.y)
                        if (!selecting) {
                            const c = contentPoint(event.x, event.y)
                            if (Math.abs(c.x - origin.x) < 5 && Math.abs(c.y - origin.y) < 5) return
                            selecting = true; dragged = true; pane.currentRow = -1
                        }
                        refreshBand()
                    }
                    onReleased: selecting = false
                    onCanceled: selecting = false
                    // The second click of a quick pair: toggles again in selection mode, opens otherwise.
                    onDoubleClicked: event => {
                        if (event.button !== Qt.LeftButton || pressRow < 0) return
                        if (pane.pickMode) pane.toggleRow(pressRow, event.modifiers)
                        else pane.activated(directoryModel.entry(pressRow))
                    }
                    onClicked: event => {
                        if (dragged) return
                        if (event.button === Qt.LeftButton && pressRow >= 0) {
                            if (pane.pickMode) pane.toggleRow(pressRow, event.modifiers)
                            else pane.selectRow(pressRow, event.modifiers)
                            return
                        }
                        // Blank space clears; in selection mode a right click there keeps the picks.
                        if (pane.pickMode ? event.button === Qt.LeftButton
                                          : !(event.modifiers & Qt.ControlModifier) || event.button === Qt.RightButton)
                            directoryModel.clearSelection()
                        pane.focused(); pane.focusFiles()
                        if (event.button === Qt.RightButton) {
                            const p = mapToItem(pane, event.x, event.y)
                            pane.blankContextRequested(directoryModel, p.x, p.y)
                        }
                    }
                    // Keep selecting while the pointer rests above or below the list.
                    Timer {
                        interval: 16; repeat: true
                        running: band.selecting && (band.pointer.y < 28 || band.pointer.y > band.height - 28)
                        onTriggered: {
                            const v = band.view
                            const edge = band.pointer.y < 28 ? band.pointer.y - 28 : band.pointer.y - (band.height - 28)
                            const step = Math.max(-40, Math.min(40, edge * 0.6))
                            v.contentY = Math.max(v.originY, Math.min(v.contentY + step, v.originY + Math.max(0, v.contentHeight - v.height)))
                            band.refreshBand()
                        }
                    }
                }
                Rectangle {
                    objectName: "selectionBand"
                    visible: band.selecting
                    z: 3
                    // The anchor is stored in content coordinates, so it stays put while scrolling.
                    readonly property point start: band.view.mapToItem(files, band.origin.x - band.view.contentX + band.view.originX,
                                                                        band.origin.y - band.view.contentY + band.view.originY)
                    readonly property real bandLeft: Math.max(0, Math.min(start.x, band.pointer.x))
                    readonly property real bandTop: Math.max(0, Math.min(start.y, band.pointer.y))
                    x: bandLeft; y: bandTop
                    width: Math.min(files.width, Math.max(start.x, band.pointer.x)) - bandLeft
                    height: Math.min(files.height, Math.max(start.y, band.pointer.y)) - bandTop
                    radius: 3
                    color: pane.theme.bandFill
                    border.width: 1; border.color: pane.theme.selectionBar
                }
                Column {
                    anchors.centerIn: parent; spacing: 12
                    visible: directoryModel.count === 0 && !directoryModel.loading
                    Icon { anchors.horizontalCenter: parent.horizontalCenter; name: pane.searchMode ? "search" : "folder"; width: 46; height: 46; opacity: 0.5 }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: directoryModel.error || (pane.searchMode ? "没有匹配的项目" : pane.emptyText); color: pane.muted; font.pixelSize: 16; width: Math.min(320, pane.width - 40); wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; visible: pane.searchMode; text: "索引更新期间，结果可能尚不完整"; color: pane.muted; font.pixelSize: 13 }
                }
                DropArea {
                    id: dropZone
                    objectName: "paneDropArea"
                    anchors.fill: parent
                    property var urls: []
                    property string targetUri: ""
                    property string targetName: ""
                    property bool allowed: false
                    property string action: ""
                    property int modifiers: -1
                    property real pointerX: 0
                    property real pointerY: 0
                    property string springUri: ""
                    // After a folder opens under a resting pointer, the next one opens only once the
                    // pointer moves again, so a drag never keeps descending on its own.
                    property bool springArmed: true
                    property point restAt: Qt.point(0, 0)
                    function locate(x, y) {
                        const view = pane.gridMode ? grid : list
                        const p = mapToItem(view, x, y)
                        const row = view.indexAt(p.x + view.contentX, p.y + view.contentY)
                        const e = row >= 0 ? directoryModel.entry(row) : null
                        pointerX = x; pointerY = y
                        // A folder under the pointer is the target, unless it is one of the dragged items.
                        let uri = "", name = ""
                        if (e && e.folder && urls.indexOf(e.fileUri) < 0) { uri = e.fileUri; name = e.fileName; pane.dropRow = row }
                        else { pane.dropRow = -1; if (!pane.searchMode) { uri = directoryModel.uri; name = directoryModel.title } }
                        const mods = controller.modifiers()
                        if (uri !== targetUri || mods !== modifiers) {
                            targetUri = uri; targetName = name; modifiers = mods
                            allowed = uri !== "" && controller.canDrop(urls, uri)
                            action = allowed ? controller.dropAction(urls, uri) : ""
                        }
                        armSpring()
                    }
                    // Hovering a folder opens it after a moment, whether or not the items could be
                    // dropped there, so any folder can be reached in the middle of a drag.
                    function armSpring() {
                        const uri = springArmed && pane.dropRow >= 0 ? targetUri : ""
                        if (uri === springUri) return
                        springUri = uri
                        pane.springRow = uri ? pane.dropRow : -1
                        if (uri) springTimer.restart(); else springTimer.stop()
                    }
                    function reset() {
                        springTimer.stop(); springUri = ""; pane.springRow = -1
                        pane.dropRow = -1; targetUri = ""; allowed = false; action = ""; modifiers = -1; urls = []
                    }
                    onEntered: drag => {
                        urls = pane.host.dragUris(drag)
                        if (!urls.length) { drag.accepted = false; return }
                        springArmed = true
                        locate(drag.x, drag.y)
                    }
                    onPositionChanged: drag => {
                        if (!springArmed && (Math.abs(drag.x - restAt.x) > 4 || Math.abs(drag.y - restAt.y) > 4)) springArmed = true
                        locate(drag.x, drag.y)
                    }
                    onExited: reset()
                    onDropped: drop => {
                        locate(drop.x, drop.y)
                        const uri = targetUri, ok = allowed, values = urls
                        reset()
                        if (!ok) return
                        controller.dragDrop(values, uri)
                        // Never report a move back to the source app: the transfer is done here.
                        drop.accept(Qt.CopyAction)
                    }
                    Rectangle {
                        anchors.fill: parent; color: Qt.alpha(pane.theme.accent, 0.07); border.color: pane.theme.accent; radius: 8
                        visible: dropZone.containsDrag && dropZone.allowed && pane.dropRow < 0
                    }
                    Rectangle {
                        objectName: "dropHint"
                        visible: dropZone.containsDrag && dropZone.allowed && dropZone.action !== ""
                        x: Math.min(parent.width - width - 8, Math.max(8, dropZone.drag.x + 18))
                        y: Math.min(parent.height - height - 8, Math.max(8, dropZone.drag.y + 20))
                        width: hintLabel.implicitWidth + 22; height: 28; radius: 8
                        color: pane.theme.raised; border.color: pane.theme.lineStrong
                        Text {
                            id: hintLabel; anchors.centerIn: parent
                            text: ({move: "移动到 ", copy: "复制到 ", trash: "移入 "})[dropZone.action] + "“" + dropZone.targetName + "”"
                            color: pane.ink; font.pixelSize: 13
                        }
                    }
                    Timer {
                        id: springTimer
                        interval: pane.host ? pane.host.springDelay : 700
                        onTriggered: {
                            const uri = dropZone.springUri
                            dropZone.springArmed = false
                            dropZone.restAt = Qt.point(dropZone.pointerX, dropZone.pointerY)
                            dropZone.springUri = ""; pane.springRow = -1
                            if (uri) pane.springRequested(uri)
                        }
                    }
                    // The listing under a resting pointer changes when a folder opens or loads.
                    Connections {
                        target: pane.directoryModel
                        function onCountChanged() { if (dropZone.containsDrag) dropZone.locate(dropZone.pointerX, dropZone.pointerY) }
                        function onLocationChanged() {
                            if (!dropZone.containsDrag) return
                            dropZone.targetUri = ""
                            dropZone.locate(dropZone.pointerX, dropZone.pointerY)
                        }
                    }
                    // Scroll while hovering near the edges so lower folders stay reachable.
                    Timer {
                        interval: 16; repeat: true
                        running: dropZone.containsDrag && (dropZone.pointerY < 36 || dropZone.pointerY > dropZone.height - 36)
                        onTriggered: {
                            const view = pane.gridMode ? grid : list
                            const step = dropZone.pointerY < 36 ? -12 : 12
                            view.contentY = Math.max(view.originY, Math.min(view.contentY + step, view.originY + view.contentHeight - view.height))
                        }
                    }
                }
            }
            Rectangle {
                visible: directoryModel.error !== "" && directoryModel.count > 0
                Layout.fillWidth: true; Layout.preferredHeight: errorLabel.implicitHeight + 16; color: pane.theme.warningSoft
                Text { id: errorLabel; anchors.fill: parent; anchors.margins: 8; text: directoryModel.error; color: pane.ink; font.pixelSize: 13; wrapMode: Text.Wrap }
            }
        }
    }
    Component {
        id: selectionHighlight
        Rectangle {
            id: glide
            objectName: "selectionHighlight"
            // Highlights live in the view's contentItem, whose parent is the view itself.
            readonly property var view: parent ? parent.parent : null
            readonly property Item target: view ? view.currentItem : null
            readonly property bool shown: !!target && target.live && target.selected === true && view.currentIndex >= 0 && view.currentIndex < directoryModel.count && view.currentIndex === pane.currentRow
            // Retirement is immediate; ordinary selection changes still glide/fade.
            visible: !!target && target.live
            readonly property real inset: pane.gridMode ? 4 : 1
            x: target ? target.x + inset : 0
            y: target ? target.y + inset : 0
            width: target ? target.width - 2 * inset : 0
            height: target ? target.height - 2 * inset : 0
            z: 0; radius: 6
            color: pane.theme.selection
            border.width: 1; border.color: pane.theme.selectionBorder
            opacity: shown ? 1 : 0
            // Glide between rows; appearing after an empty selection snaps instead of sweeping in.
            Behavior on x { enabled: pane.animated && glide.opacity > 0.4; NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
            Behavior on y { enabled: pane.animated && glide.opacity > 0.4; NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
            Behavior on opacity { enabled: pane.animated; NumberAnimation { duration: 90 } }
            Rectangle { visible: !pane.gridMode; width: 3; height: parent.height - 14; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; color: pane.theme.selectionBar; radius: 2 }
        }
    }
    Component {
        id: fileDelegate
        Item {
            id: item
            required property int index
            required property string fileName
            required property string fileUri
            required property bool folder
            required property bool isLink
            required property string sizeText
            required property string modifiedText
            required property string kind
            required property string iconName
            required property string fullPath
            required property bool selected
            required property double bytes
            required property bool hiddenItem
            property bool removing: false
            property bool pooled: false
            readonly property bool live: !removing && !pooled && index >= 0
            readonly property bool hovered: live && mouse.containsMouse && !pane.covered
            enabled: live
            function clearHover() {
                if (pane.hoverOwner === item) { pane.hoverOwner = null; pane.hovered(({})) }
            }
            onLiveChanged: if (!live) clearHover()
            onHoveredChanged: if (!hovered) clearHover()
            Component.onDestruction: clearHover()
            readonly property bool current: live && pane.currentRow === index
            readonly property bool dropTarget: live && pane.dropRow === index
            // Hidden entries read as "ghosted", like Windows: dimmed icon and text, same layout.
            readonly property real ghost: hiddenItem ? 0.5 : 1
            objectName: "fileEntry"
            width: pane.gridMode ? 176 : list.contentWidth
            height: pane.gridMode ? 180 : pane.searchMode ? 62 : (pane.dense ? 36 : 43)
            // The platform drag runs from a window-level item, not this row: hovering may open
            // other folders mid-drag, which recycles or destroys the row that started it.
            DragHandler {
                id: dragHandler; target: null; acceptedButtons: Qt.LeftButton
                enabled: item.live && !pane.covered && !pane.pickMode
                onActiveChanged: {
                    if (active && pane.host)
                        Qt.callLater(pane.host.startFileDrag, directoryModel.selectedUris.slice(), item.iconName)
                }
            }
            // Pooled rows may come back mid-fade from a remove transition, and must take their
            // new row's hover/selection state at once instead of fading from the old row's.
            property bool settled: true
            function recycle() {
                clearHover()
                opacity = 1; scale = 1; removing = false; pooled = false
                settled = false; Qt.callLater(() => item.settled = true)
            }
            ListView.onRemove: { removing = true; clearHover() }
            GridView.onRemove: { removing = true; clearHover() }
            ListView.onPooled: { pooled = true; clearHover() }
            GridView.onPooled: { pooled = true; clearHover() }
            ListView.onReused: recycle()
            GridView.onReused: recycle()
            // Each state is its own constant-colour layer and only its opacity animates.
            // Animating one colour from "transparent" (transparent *black*) to the selection
            // tint passes through grey, which flashed every row on Select All.
            Item {
                anchors.fill: parent; anchors.margins: pane.gridMode ? 4 : 1
                Rectangle {
                    anchors.fill: parent; radius: 6
                    color: pane.theme.hover
                    visible: item.live
                    opacity: item.hovered && !item.dropTarget && !(item.selected && !item.current) ? 1 : 0
                    Behavior on opacity { enabled: pane.animated && item.settled; NumberAnimation { duration: 90 } }
                }
                // The current selected row is painted by the sliding highlight; this covers the others.
                Rectangle {
                    anchors.fill: parent; radius: 6
                    color: pane.theme.selection
                    border.width: 1; border.color: pane.theme.selectionBorder
                    // Selection changes apply instantly (as in Windows): fading a whole list in on
                    // Select All reads as a flash. Only single-item motion (the glide) animates.
                    opacity: item.live && item.selected && !item.current && !item.dropTarget ? 1 : 0
                }
                Rectangle {
                    anchors.fill: parent; radius: 6
                    visible: item.dropTarget
                    color: pane.theme.dropFill; border.width: 1; border.color: pane.theme.accent
                    SpringProgress { running: pane.springRow === item.index; color: pane.theme.accent; duration: springTimer.interval }
                }
            }
            Spotlight {
                objectName: "fileEntrySpotlight"
                visible: item.live && supported && opacity > 0.01
                anchors.fill: parent; anchors.margins: pane.gridMode ? 4 : 1
                active: pane.animated && item.hovered && !dragHandler.active && !item.dropTarget
                center: Qt.point(mouse.mouseX - (pane.gridMode ? 4 : 1), mouse.mouseY - (pane.gridMode ? 4 : 1))
                radius: pane.gridMode ? 130 : 190
                strength: item.selected ? 0.3 : 0.42
                tint: pane.theme.selectionGlow
                opacity: active ? (pane.dark ? 0.55 : 0.3) : 0
            }
            Row {
                visible: !pane.gridMode
                x: 9; height: parent.height
                opacity: item.hiddenItem ? 0.85 : 1
                Item {
                    width: pane.columnWidth(0); height: parent.height
                    Icon { x: 7; anchors.verticalCenter: parent.verticalCenter; name: item.iconName; width: 23; height: 23; opacity: item.ghost }
                    Column {
                        x: 39; width: parent.width - 51; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                        Text { width: parent.width; text: item.fileName + (item.isLink ? " ↗" : ""); color: pane.ink; opacity: item.hiddenItem ? 0.62 : 1; font.pixelSize: 15; elide: Text.ElideMiddle }
                        Text { visible: pane.searchMode; width: parent.width; text: item.fullPath; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 12; elide: Text.ElideMiddle }
                    }
                }
                Text { width: pane.columnWidth(1); height: parent.height; leftPadding: 7; rightPadding: 10; verticalAlignment: Text.AlignVCenter; text: item.modifiedText; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 13 }
                Text { width: pane.columnWidth(2); height: parent.height; leftPadding: 7; rightPadding: 10; verticalAlignment: Text.AlignVCenter; text: item.kind; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 12 }
                Text { width: pane.columnWidth(3); height: parent.height; leftPadding: 7; rightPadding: 10; verticalAlignment: Text.AlignVCenter; text: item.sizeText; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 13; horizontalAlignment: Text.AlignRight }
            }
            Column {
                visible: pane.gridMode
                anchors.fill: parent; anchors.margins: 12; spacing: 8
                Item {
                    width: parent.width; height: 112
                    Icon { objectName: "gridFallbackIcon"; anchors.centerIn: parent; name: item.iconName; width: 84; height: 84; visible: thumbnail.status !== Image.Ready; opacity: item.ghost }
                    Image {
                        id: thumbnail
                        anchors.fill: parent
                        objectName: "gridThumbnail"
                        source: pane.gridMode && pane.thumbnails && (item.iconName === "image" || item.iconName === "video" || /\.pdf$/i.test(item.fileName)) ? "image://thumb/" + encodeURIComponent(item.fileUri) + "|" + item.bytes + item.modifiedText : ""
                        asynchronous: true; fillMode: Image.PreserveAspectFit
                        sourceSize: Qt.size(256, 256)
                        opacity: item.ghost
                    }
                }
                Text { width: parent.width; height: 40; text: item.fileName; color: pane.ink; opacity: item.hiddenItem ? 0.62 : 1; font.pixelSize: 13; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
            }
            // Selection mode marker: a check circle on the icon.
            Rectangle {
                objectName: "pickMark"
                visible: pane.pickMode
                width: pane.gridMode ? 22 : 14; height: width; radius: width / 2
                x: pane.gridMode ? 14 : 9 + 7 + 23 - width + 4
                y: pane.gridMode ? 14 : item.height / 2 + 11.5 - width + 4
                color: item.selected ? pane.theme.accent : pane.theme.pane
                border.width: 1.5; border.color: item.selected ? pane.theme.accent : pane.theme.lineStrong
                Text { anchors.centerIn: parent; visible: item.selected; text: "✓"; color: pane.theme.accentInk; font.pixelSize: parent.width * 0.68; font.bold: true }
            }
            MouseArea {
                id: mouse
                anchors.fill: parent; hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: event => {
                    if (event.button === Qt.RightButton && item.selected) { pane.focused(); pane.focusFiles(); return }
                    // Selection mode only ever adds: the menu then acts on all picks.
                    if (event.button === Qt.RightButton && pane.pickMode) { pane.toggleRow(item.index, 0); return }
                    if (!item.selected || (event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) pane.selectRow(item.index, event.modifiers)
                    else { pane.focused(); pane.currentRow = item.index; pane.focusFiles() }
                }
                onClicked: event => {
                    if (event.button === Qt.RightButton) { let p = mapToItem(pane, event.x, event.y); pane.contextRequested(directoryModel, p.x, p.y) }
                }
                onDoubleClicked: event => { if (event.button === Qt.LeftButton) pane.activated(directoryModel.entry(item.index)) }
                onEntered: if (item.live) { pane.hoverOwner = item; pane.hovered(directoryModel.entry(item.index)) }
                onExited: item.clearHover()
            }
            NookToolTip { objectName: "fileEntryToolTip"; visible: item.hovered && !dragHandler.active; text: item.fileName + "\n" + item.fullPath; delay: 1000 }
        }
    }
}
