import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "motion.js" as Motion

Rectangle {
    id: pane
    required property var directoryModel
    required property var controller
    property bool gridMode: false
    property bool focusedPane: false
    property bool searchMode: false
    // Bound by the window to its search field (including the chooser's independent search).
    property string searchText: ""
    Binding {
        target: pane.directoryModel; property: "smartDates"
        value: pane.controller.settings.values.smartDates !== false
    }
    function escapeText(value) {
        return String(value).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                            .replace(/"/g, "&quot;").replace(/'/g, "&#39;")
    }
    function highlightText(value) {
        const text = String(value), query = searchMode ? searchText.trim() : ""
        if (!query) return escapeText(text)
        // Literal matching only: neither filenames nor the query can inject markup/regular expressions.
        const expression = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi")
        let result = "", start = 0, match
        while ((match = expression.exec(text)) !== null) {
            result += escapeText(text.slice(start, match.index))
                    + '<b><font color="' + theme.accentText + '">' + escapeText(match[0]) + '</font></b>'
            start = match.index + match[0].length
        }
        return result + escapeText(text.slice(start))
    }
    // Items from many folders (search results, a tag view): each row says where it is.
    readonly property bool listing: searchMode || directoryModel.tagView === true
    readonly property var tagStore: controller.tags
    // Pressing a row's tag dot: this folder shows only that tag; in a tag view, opens its view.
    function pickTag(color) {
        if (searchMode) return
        if (directoryModel.tagView) controller.navigate("tag:" + color)
        else directoryModel.tagFilter = directoryModel.tagFilter === color ? "" : color
    }
    property bool dark: true
    property bool dense: false
    // The view's own size (the toolbar slider), as seen on screen: one for the list, one for the
    // grid. The window scales everything by its interface scale, which is undone here, so that
    // setting sizes the rest of nook and these the files.
    property real listZoom: 1
    property real gridZoom: 1
    property real interfaceScale: 1
    readonly property real ls: listZoom / interfaceScale
    readonly property real gs: gridZoom / interfaceScale
    // Grid names stay readable when the tiles get small.
    readonly property real gridFont: Math.max(11, 13 * gridZoom) / interfaceScale
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
    // A freshly entered folder streams its rows in; only later insertions count as new rows.
    property bool streaming: false
    readonly property bool rowAnimations: animated && !populating && motionKind === "" && !pendingFrom
    function revealContents() {
        if (!populating) return
        settleTimer.restart()
        if (animated) { enterFade.restart(); enterSlide.restart() } else files.opacity = 1
    }
    Timer { id: settleTimer; interval: 80; onTriggered: pane.populating = false }
    // Runs on the render thread, so it stays smooth while the GUI thread fills the model.
    OpacityAnimator { id: enterFade; target: files; from: 0; to: 1; duration: enterDirection ? 220 : 170; easing.type: Easing.OutCubic }
    // A folder opened below comes in from the right; going back up, from the left.
    property string shownUri: ""
    property int enterDirection: 0
    readonly property real enterShift: 40
    NumberAnimation {
        id: enterSlide; target: filesShift; property: "x"; from: pane.enterShift * pane.enterDirection; to: 0
        duration: Motion.smooth.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.smooth.curve
    }
    function directionTo(next) {
        const was = shownUri
        if (!was || !next || was === next) return 0
        if (next.startsWith(was.endsWith("/") ? was : was + "/")) return 1
        if (was.startsWith(next.endsWith("/") ? next : next + "/")) return -1
        return 0
    }
    Connections {
        target: pane.directoryModel
        function onLocationChanged() {
            pane.populating = true
            pane.streaming = true
            settleTimer.stop()
            pane.endMotion()
            pane.enterDirection = pane.directionTo(pane.directoryModel.uri)
            pane.shownUri = pane.directoryModel.uri
            if (pane.animated) { enterFade.stop(); enterSlide.stop(); files.opacity = 0; filesShift.x = pane.enterShift * pane.enterDirection }
        }
        // Reveal on the first real rows, or once loading ends (empty folder / error text).
        function onCountChanged() { if (pane.directoryModel.count > 0) pane.revealContents() }
        function onStatusChanged() {
            if (pane.directoryModel.loading) return
            pane.revealContents()
            pane.streaming = false
        }
        function onRearranging() {
            if (!pane.animated || pane.populating || pane.motionKind === "morph") return
            pane.pendingFrom = pane.snapshot(pane.gridMode ? grid : list)
            pendingExpiry.restart()
        }
        function onModelReset() { pane.restartFit(); pane.rearranged() }
        function onRowsMoved() { pane.restartFit(); pane.rearranged() }
        function onRowsInserted() { pane.restartFit(); pane.rearranged() }
        function onRowsRemoved() { pane.restartFit(); pane.rearranged() }
        function onDataChanged() { pane.restartFit() }
    }
    // Rows that change place (Pulse's motion v2). A re-sort, filter or hidden-files change slides
    // every row on screen from where it was to its new slot, and rows that came into view fade in
    // a little later. Switching list <-> grid flies each icon from its old place to its new one
    // while the old labels fade out and the new ones fade in once the shapes are mostly there.
    // Only rows on screen take part; both are pure functions of the time since they began, and
    // hit-testing always uses the new layout.
    property string motionKind: ""     // "", "reflow" or "morph"
    property var motionFrom: ({})      // fileUri -> {x, y, ix, iy, iw, ih} in pane coordinates
    property real motionMs: 0
    property int motionFirst: 0        // lowest row index on screen, for the stagger
    property var pendingFrom: null     // a reflow waiting for the new order to arrive
    property string morphFrom: ""      // the view being left during a morph: "list" or "grid"
    property bool listLive: true
    property bool gridLive: false
    readonly property real motionTotal: motionKind === "morph" ? 400 + 90 : Math.max(Motion.smooth.ms, 60 + 160)
    readonly property real labelOpacity: motionKind === "morph" ? Motion.smoothStep(0.30, 0.85, motionMs / 400) : 1
    readonly property real leavingOpacity: motionKind === "morph" ? 1 - Motion.smoothStep(0, 0.35, motionMs / 400) : 0
    // Items start 6 ms apart, at most 90 ms: big folders stay quick.
    function morphProgress(order) { return Motion.morph((motionMs - Math.min(Math.max(0, order) * 6, 90)) / 400) }
    function reflowProgress() { return motionMs >= Motion.smooth.ms ? 1 : Motion.progress(Motion.smooth.k, Motion.smooth.c, motionMs) }
    function appearOpacity() { return Math.max(0, Math.min(1, (motionMs - 60) / 160)) }
    // Where a row is, in pane coordinates; the arguments name what it depends on.
    function placeOf(view, x, y, contentX, contentY) { return view.contentItem.mapToItem(pane, x, y) }
    // The icon of a row placed at `at`: the list's 23 px icon, or the grid's 112 px picture square
    // (at the view's zoom).
    function iconRect(at, inGrid, height) {
        return inGrid ? Qt.rect(at.x + (grid.cellWidth - 112 * gs) / 2, at.y + 12 * gs, 112 * gs, 112 * gs)
                      : Qt.rect(at.x + 9 + columnX(0) + 7 * ls, at.y + (height - 23 * ls) / 2, 23 * ls, 23 * ls)
    }
    // An icon on its way from its old rect to `to`: offset and scale for its transform.
    function flight(from, to, p) {
        const w = from.iw + (to.width - from.iw) * p
        return {dx: from.ix + (to.x - from.ix) * p - to.x, dy: from.iy + (to.y - from.iy) * p - to.y, scale: w / to.width}
    }
    function snapshot(view) {
        const rows = {}
        let first = -1
        for (const row of view.contentItem.children) {
            if (row.objectName !== "fileEntry" || !row.live) continue
            const at = placeOf(view, row.x, row.y, 0, 0)
            if (at.y + row.height < 0 || at.y > pane.height) continue
            const icon = iconRect(at, view === grid, row.height)
            rows[row.fileUri] = {x: at.x, y: at.y, ix: icon.x, iy: icon.y, iw: icon.width, ih: icon.height}
            if (first < 0 || row.index < first) first = row.index
        }
        return {rows: rows, first: Math.max(0, first)}
    }
    // Where a file's icon is on screen, in target's coordinates; null when it is not shown.
    function iconRectOf(uri, target) {
        const view = gridMode ? grid : list
        for (const row of view.contentItem.children) {
            if (row.objectName !== "fileEntry" || !row.live || row.fileUri !== uri) continue
            const at = placeOf(view, row.x, row.y, 0, 0)
            if (at.y + row.height < 0 || at.y > height) return null
            const r = iconRect(at, view === grid, row.height)
            const p = mapToItem(target, r.x, r.y)
            return Qt.rect(p.x, p.y, r.width, r.height)
        }
        return null
    }
    function startMotion(kind, taken) { motionFrom = taken.rows; motionFirst = taken.first; motionMs = 0; motionKind = kind }
    function endMotion() {
        motionKind = ""; motionFrom = ({}); morphFrom = ""
        listLive = !gridMode; gridLive = gridMode
    }
    function rearranged() {
        if (!pendingFrom) return
        const taken = pendingFrom
        pendingFrom = null
        if (pendingExpiry.running) startMotion("reflow", taken)
    }
    // The new order lands once the listing is re-read; a reflow waits for it this long.
    Timer { id: pendingExpiry; interval: 700; onTriggered: pane.pendingFrom = null }
    FrameAnimation {
        running: pane.motionKind !== ""
        // At most 33 ms a frame: a slow first frame of the new layout slows the motion for a
        // moment instead of skipping a third of it.
        onTriggered: {
            pane.motionMs += Math.min(frameTime * 1000, 33)
            if (pane.motionMs >= pane.motionTotal) pane.endMotion()
        }
    }
    Component.onCompleted: { listLive = !gridMode; gridLive = gridMode }
    onGridModeChanged: {
        const leaving = gridMode ? list : grid, view = gridMode ? grid : list
        // The first row on screen stays on screen in the other view.
        const top = leaving.indexAt(leaving.contentX + 12, leaving.contentY + 12)
        const taken = animated && !populating && morphFrom === "" && directoryModel.count > 0 ? snapshot(leaving) : null
        if (taken) { morphFrom = gridMode ? "list" : "grid"; startMotion("morph", taken) }
        else { motionKind = ""; motionFrom = ({}); morphFrom = "" } // a second switch mid-morph lands at once
        listLive = !gridMode || morphFrom === "list"; gridLive = gridMode || morphFrom === "grid"
        if (currentRow >= 0) view.positionViewAtIndex(currentRow, ListView.Contain)
        else if (top > 0) view.positionViewAtIndex(top, ListView.Beginning)
    }
    property var localWidths: []
    onDirectoryModelChanged: { localWidths = []; currentRow = -1; fitTimer.stop() }
    // Keys belong to the file list itself, never to a control that once had focus in the pane.
    function focusFiles() {
        const w = ApplicationWindow.window
        const focused = w ? w.activeFocusItem : null
        for (let item = focused ? focused.parent : null; item; item = item.parent)
            if (item === keyArea) { focused.focus = false; break }
        keyArea.forceActiveFocus()
    }
    readonly property bool descending: directoryModel.sortDescending
    readonly property int sortColumn: ["name", "date", "type", "size", "created"].indexOf(directoryModel.sortKey)
    readonly property var visibleColumns: controller.settings.values.visibleColumns || [true, true, true, true, false]
    function columnVisible(index) { return index === 0 || (index < visibleColumns.length ? visibleColumns[index] !== false : false) }
    // Order contains logical IDs; widths, sorting and visibility remain attached to the field.
    readonly property var savedColumnOrder: controller.settings.values.columnOrder || [0, 1, 2, 3, 4]
    property var localColumnOrder: []
    readonly property var columnOrder: localColumnOrder.length === 5 ? localColumnOrder : savedColumnOrder
    property int draggedColumn: -1
    property int columnFollowId: -1
    property int pressedColumn: -1
    property int columnHeldId: -1
    property real columnHeldX: 0
    property int liftedColumn: -1
    property real columnDragPointer: 0
    property real columnDragContentX: 0
    property real columnDragOffset: 0
    property bool columnMotionActive: false
    // Five shared positions keep header, row cells, hit tests and icon/tag overlays in sync.
    // Only displaced columns ease; the grabbed column follows the pointer without latency.
    property real nameColumnX: columnVisualTarget(0)
    property real dateColumnX: columnVisualTarget(1)
    property real typeColumnX: columnVisualTarget(2)
    property real sizeColumnX: columnVisualTarget(3)
    property real createdColumnX: columnVisualTarget(4)
    Behavior on nameColumnX { enabled: pane.animated && pane.columnMotionActive && pane.draggedColumn !== 0 && pane.pressedColumn !== 0; NumberAnimation { id: columnAnimation0; duration: 180; easing.type: Easing.OutCubic } }
    Behavior on dateColumnX { enabled: pane.animated && pane.columnMotionActive && pane.draggedColumn !== 1 && pane.pressedColumn !== 1; NumberAnimation { id: columnAnimation1; duration: 180; easing.type: Easing.OutCubic } }
    Behavior on typeColumnX { enabled: pane.animated && pane.columnMotionActive && pane.draggedColumn !== 2 && pane.pressedColumn !== 2; NumberAnimation { id: columnAnimation2; duration: 180; easing.type: Easing.OutCubic } }
    Behavior on sizeColumnX { enabled: pane.animated && pane.columnMotionActive && pane.draggedColumn !== 3 && pane.pressedColumn !== 3; NumberAnimation { id: columnAnimation3; duration: 180; easing.type: Easing.OutCubic } }
    Behavior on createdColumnX { enabled: pane.animated && pane.columnMotionActive && pane.draggedColumn !== 4 && pane.pressedColumn !== 4; NumberAnimation { id: columnAnimation4; duration: 180; easing.type: Easing.OutCubic } }
    Timer {
        id: columnSettleTimer; interval: 220
        onTriggered: { pane.columnMotionActive = false; pane.liftedColumn = -1 }
    }
    component ColumnDragBackground: Rectangle {
        required property int columnId
        property bool selected: false
        anchors.fill: parent; z: -1
        objectName: "columnDragBackdrop" + columnId
        visible: pane.liftedColumn === columnId
        color: selected ? pane.theme.selection : Qt.rgba(pane.theme.pane.r, pane.theme.pane.g, pane.theme.pane.b, 1)
        Rectangle { anchors.fill: parent; color: pane.theme.accent; opacity: 0.055 }
    }
    function columnX(id) { return [nameColumnX, dateColumnX, typeColumnX, sizeColumnX, createdColumnX][id] || 0 }
    function columnVisualTarget(id) {
        if (columnFollowId === id)
            return Math.max(0, Math.min(columnDragContentX - columnDragOffset, columnsWidth - columnWidth(id)))
        if (columnHeldId === id) return columnHeldX
        return columnTargetX(id)
    }
    function holdColumn(id) {
        columnSettleTimer.stop()
        columnHeldX = columnX(id)
        const animation = [columnAnimation0, columnAnimation1, columnAnimation2, columnAnimation3, columnAnimation4][id]
        animation.stop()
        pressedColumn = id
        columnHeldId = id
        liftedColumn = id
    }
    function releaseColumnHold() {
        if (columnHeldId < 0) return
        columnMotionActive = true
        pressedColumn = -1
        columnHeldId = -1
        columnSettleTimer.restart()
    }
    function columnTargetX(id) {
        let x = 0
        for (const column of columnOrder) {
            if (column === id) break
            x += columnWidth(column)
        }
        return x
    }
    function moveColumnDrag(id, pointerX, grabOffset) {
        const x = columnTitles.mapFromItem(pane, pointerX, 0).x
        columnDragPointer = pointerX
        columnDragContentX = x
        if (draggedColumn !== id) {
            columnSettleTimer.stop()
            columnMotionActive = false
            const animation = [columnAnimation0, columnAnimation1, columnAnimation2, columnAnimation3, columnAnimation4][id]
            animation.stop()
            // Mark the grabbed field before enabling easing: otherwise its first pointer update
            // could start an animation with the previous (ungrabbed) enabled state.
            columnDragOffset = typeof grabOffset === "number" ? grabOffset : columnWidth(id) / 2
            draggedColumn = id
            columnFollowId = id
            pressedColumn = -1
            columnHeldId = -1
            liftedColumn = id
            columnMotionActive = true
        }
        const order = Array.from(columnOrder), from = order.indexOf(id)
        let target = -1
        for (let i = 0; i < order.length; ++i) {
            const other = order[i]
            if (other === id || !columnVisible(other)) continue
            // Hit-test stable destination slots, never the columns still animating toward them.
            const middle = columnTargetX(other) + columnWidth(other) / 2
            if (i < from && x < middle) { target = i; break }
            if (i > from && x > middle) target = i
        }
        if (target >= 0) {
            order.splice(from, 1)
            order.splice(target, 0, id)
            localColumnOrder = order
        }
    }
    function finishColumnDrag(commit) {
        if (commit && localColumnOrder.length === 5)
            controller.settings.set("columnOrder", localColumnOrder)
        localColumnOrder = []
        pressedColumn = -1
        columnHeldId = -1
        // Re-enable easing before changing the pointer-follow target to its final slot.
        draggedColumn = -1
        columnFollowId = -1
        columnSettleTimer.restart()
    }
    Timer {
        interval: 30; repeat: true; running: pane.draggedColumn >= 0
        onTriggered: {
            const x = columnHeader.mapFromItem(pane, pane.columnDragPointer, 0).x
            const step = x < 28 ? -12 : x > columnHeader.width - 28 ? 12 : 0
            if (step) {
                list.contentX = Math.max(0, Math.min(list.contentX + step, list.contentWidth - list.width))
                pane.moveColumnDrag(pane.draggedColumn, pane.columnDragPointer)
            }
        }
    }
    function toggleColumn(index) {
        const columns = [true, columnVisible(1), columnVisible(2), columnVisible(3), columnVisible(4)]
        columns[index] = !columns[index]
        controller.settings.set("visibleColumns", columns)
    }
    property bool useSavedWidths: true // false: columns fit this pane (the file chooser)
    readonly property var savedWidths: useSavedWidths ? controller.settings.values.columnWidths || [] : []
    readonly property string savedSignature: JSON.stringify(savedWidths)
    onSavedSignatureChanged: localWidths = []
    readonly property real columnsWidth: columnWidth(0) + columnWidth(1) + columnWidth(2) + columnWidth(3) + columnWidth(4)
    // Widths are kept at the list's own size and grow with its zoom, so the columns keep fitting
    // their text; by default the name column takes what the others leave.
    function baseWidth(index) {
        const minimums = [140, 100, 70, 65, 100]
        const defaults = [Math.max(200, (pane.width - 35) / ls - 335), 150, 100, 85, 150]
        const widths = localWidths.length === 4 || localWidths.length === 5 ? localWidths : savedWidths
        return index < widths.length ? Math.max(minimums[index], Math.min(1600, Number(widths[index]) || defaults[index])) : defaults[index]
    }
    function columnWidth(index) { return columnVisible(index) ? baseWidth(index) * ls : 0 }
    // Measure in short event-loop slices, so fitting a large search result never freezes input.
    property var fitJob: null
    function restartFit() { if (fitTimer.running && fitJob) fitColumns(fitJob.column) }
    function fitColumns(column) {
        fitTimer.stop()
        const widths = [baseWidth(0), baseWidth(1), baseWidth(2), baseWidth(3), baseWidth(4)]
        const headers = [listing ? "名称 / 位置" : "名称", "修改时间", "类型", "大小", "创建时间"]
        for (let c = 0; c < 5; ++c) {
            if ((column < 0 && columnVisible(c)) || c === column) {
                fitMetrics.font.pixelSize = 13 * ls
                fitMetrics.text = headers[c] + "  ↓"
                widths[c] = fitMetrics.advanceWidth / ls + 30
            }
        }
        fitJob = {row: 0, column: column, count: directoryModel.count, uri: directoryModel.uri, widths: widths}
        fitTimer.start()
    }
    TextMetrics { id: fitMetrics }
    Timer {
        id: fitTimer; interval: 1; repeat: true
        onTriggered: {
            const job = pane.fitJob
            if (!job || job.count !== directoryModel.count || job.uri !== directoryModel.uri) { stop(); return }
            const end = Math.min(job.count, job.row + 64)
            for (; job.row < end; ++job.row) {
                for (let c = 0; c < 5; ++c) {
                    if (job.column >= 0 ? c !== job.column : !pane.columnVisible(c)) continue
                    fitMetrics.font.pixelSize = (c === 0 ? 15 : c === 2 ? 12 : 13) * pane.ls
                    fitMetrics.text = directoryModel.columnText(job.row, c)
                    // Name's icon, link marker, tag dots and padding; other columns' cell padding.
                    let measured = fitMetrics.advanceWidth / pane.ls + (c === 0 ? 100 : 24)
                    if (c === 0 && pane.listing) {
                        fitMetrics.font.pixelSize = 12 * pane.ls
                        fitMetrics.text = directoryModel.columnText(job.row, 5)
                        measured = Math.max(measured, fitMetrics.advanceWidth / pane.ls + 56)
                    }
                    job.widths[c] = Math.min(1600, Math.max(job.widths[c], measured))
                }
            }
            if (job.row === job.count) {
                pane.localWidths = job.widths.map((value, index) => Math.max([140, 100, 70, 65, 100][index], value))
                stop(); pane.fitJob = null
            }
        }
    }
    NookMenu {
        id: columnMenu
        objectName: "columnMenu"
        NookMenuItem { text: "名称（始终显示）"; enabled: false; shortcutText: "✓" }
        NookMenuItem { text: "修改时间"; checkable: true; checked: pane.columnVisible(1); shortcutText: checked ? "✓" : ""; onTriggered: pane.toggleColumn(1) }
        NookMenuItem { text: "类型"; checkable: true; checked: pane.columnVisible(2); shortcutText: checked ? "✓" : ""; onTriggered: pane.toggleColumn(2) }
        NookMenuItem { text: "大小"; checkable: true; checked: pane.columnVisible(3); shortcutText: checked ? "✓" : ""; onTriggered: pane.toggleColumn(3) }
        NookMenuItem { objectName: "createdColumnMenuItem"; text: "创建时间"; checkable: true; checked: pane.columnVisible(4); onTriggered: pane.toggleColumn(4) }
        MenuSeparator {}
        NookMenuItem { text: "按内容调整列宽"; onTriggered: pane.fitColumns(-1) }
    }
    function resizeColumn(index, value) {
        let widths = [baseWidth(0), baseWidth(1), baseWidth(2), baseWidth(3), baseWidth(4)]
        widths[index] = Math.max([140, 100, 70, 65, 100][index], Math.min(1600, value / ls))
        localWidths = widths
    }
    function saveColumns() {
        controller.settings.set("columnWidths", [baseWidth(0), baseWidth(1), baseWidth(2), baseWidth(3), baseWidth(4)])
        localWidths = []
        columnsSaved()
    }
    Connections {
        target: pane.directoryModel
        function onLocationChanged() { pane.localWidths = []; pane.currentRow = -1; fitTimer.stop() }
    }
    Connections {
        target: pane.controller.settings
        function onColumnWidthsReset() { pane.localWidths = [] }
    }
    signal columnsSaved()
    // Ctrl and the wheel over the files: the view's size, in steps of 5%.
    signal zoomStep(real delta)
    signal activated(var entry)
    signal previewRequested(var entry)
    signal contextRequested(var model, real x, real y)
    signal blankContextRequested(var model, real x, real y)
    signal focused()
    signal hovered(var entry)
    // View delegates survive removal in the reuse pool. Hover belongs only to a live row.
    property Item hoverOwner: null
    // The row under the hover plate: not a drop target, and not a selected row other than the
    // current one, which its selection already paints.
    readonly property Item plateRow: hoverOwner && hoverOwner.hovered && !hoverOwner.dropTarget && !(hoverOwner.selected && !hoverOwner.current) ? hoverOwner : null
    signal springRequested(string uri)
    color: theme.pane
    radius: 10
    border.width: 1
    border.color: focusedPane ? theme.lineStrong : theme.line
    Behavior on border.color { enabled: pane.animated; ColorAnimation { duration: 180; easing.type: Easing.OutCubic } }
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
    readonly property real rowHeight: Math.round((listing ? 62 : (dense ? 36 : 43)) * ls)
    // Rows whose visual box intersects the band (content coordinates), for list or grid.
    function rowsIn(x1, y1, x2, y2) {
        let rows = []
        const count = directoryModel.count
        if (!gridMode) {
            // A row reaches across the whole list, past its last column.
            if (x2 < 9) return rows
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
                let step = pane.gridMode ? Math.max(1, Math.floor(grid.width / grid.cellWidth)) : 1
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
                    Icon { visible: !directoryModel.tagView; name: pane.searchMode ? "search" : "folder"; width: 16; height: 16 }
                    Rectangle {
                        visible: directoryModel.tagView === true && !pane.searchMode
                        width: 12; height: 12; radius: 6; Layout.leftMargin: 2; Layout.rightMargin: 2
                        color: pane.theme.tagColor(String(directoryModel.uri).slice(4))
                    }
                    Text { text: pane.searchMode ? "搜索结果" : directoryModel.title; color: pane.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideMiddle }
                    // This folder narrowed to one tag (a row's dot was pressed): the chip clears it.
                    AbstractButton {
                        objectName: "tagFilterChip"
                        visible: directoryModel.tagFilter !== "" && !pane.searchMode
                        focusPolicy: Qt.NoFocus
                        hoverEnabled: true
                        implicitHeight: 28; implicitWidth: chipRow.implicitWidth + 20
                        onClicked: directoryModel.tagFilter = ""
                        background: Rectangle {
                            radius: 14
                            color: parent.hovered ? pane.theme.hover : pane.theme.sunken
                            border.color: pane.theme.line
                        }
                        contentItem: Row {
                            id: chipRow
                            spacing: 7; leftPadding: 10
                            Rectangle { width: 9; height: 9; radius: 4.5; anchors.verticalCenter: parent.verticalCenter; color: pane.theme.tagColor(directoryModel.tagFilter) }
                            Text { text: "仅显示“" + (pane.tagStore.tags, pane.tagStore.nameOf(directoryModel.tagFilter)) + "”"; color: pane.ink; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                            Icon { name: "close"; width: 13; height: 13; anchors.verticalCenter: parent.verticalCenter }
                        }
                        NookToolTip { visible: parent.hovered; text: "显示全部项目"; delay: 600 }
                    }
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
                        // Rolls to a new count when rows come and go; a folder just opened shows its own.
                        property real shown: directoryModel.count
                        Behavior on shown { enabled: pane.rowAnimations && !pane.streaming; NumberAnimation { duration: Motion.smooth.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.smooth.curve } }
                        text: (pane.pickMode ? "已选 " + directoryModel.selectedCount.toLocaleString() + " / " : "") + Math.round(shown).toLocaleString(Qt.locale(), 'f', 0) + " 项"
                        color: pane.pickMode ? pane.theme.accentText : pane.muted; font.pixelSize: 13
                    }
                    BusyIndicator { running: directoryModel.loading; visible: running; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                }
            }
            Rectangle {
                id: columnHeader
                objectName: "columnHeader"
                visible: !pane.gridMode
                Layout.fillWidth: true; Layout.preferredHeight: Math.round(34 * Math.max(0.9, pane.ls))
                color: pane.theme.sunken
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: pane.theme.line }
                clip: true
                MouseArea {
                    anchors.fill: parent; acceptedButtons: Qt.RightButton
                    onClicked: event => { const p = mapToItem(pane, event.x, event.y); columnMenu.popup(p.x, p.y) }
                }
                Item {
                    id: columnTitles
                    x: 14 - list.contentX; width: pane.columnsWidth; height: parent.height
                    Repeater {
                        model: [pane.listing ? "名称 / 位置" : "名称", "修改时间", "类型", "大小", "创建时间"]
                        Item {
                            id: column
                            required property int index
                            required property string modelData
                            objectName: "columnHeader" + index
                            visible: pane.columnVisible(index)
                            x: pane.columnX(index); width: pane.columnWidth(index); height: parent.height
                            z: pane.liftedColumn === index ? 2 : 0
                            Rectangle { anchors.fill: parent; color: pane.theme.sunken; visible: pane.liftedColumn === column.index }
                            Rectangle {
                                anchors.fill: parent; color: pane.theme.accent
                                opacity: pane.liftedColumn === column.index ? 0.14 : 0
                                Behavior on opacity { enabled: pane.animated; NumberAnimation { duration: 130 } }
                            }
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: pane.theme.accent; visible: pane.liftedColumn === column.index }
                            Text { x: 7 * pane.ls; width: parent.width - 20; anchors.verticalCenter: parent.verticalCenter; text: column.modelData + (pane.sortColumn === column.index ? (pane.descending ? "  ↓" : "  ↑") : ""); color: pane.muted; font.pixelSize: 13 * pane.ls; elide: Text.ElideRight }
                            MouseArea {
                                objectName: "columnDragArea" + column.index
                                anchors.fill: parent; anchors.rightMargin: 10
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                hoverEnabled: true; preventStealing: true
                                cursorShape: dragging && pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                property real pressX: 0
                                property real grabOffset: 0
                                property bool dragging: false
                                onPressed: event => {
                                    pressX = mapToItem(pane, event.x, event.y).x; grabOffset = event.x; dragging = false
                                    if (event.button === Qt.LeftButton) pane.holdColumn(column.index)
                                }
                                onPositionChanged: event => {
                                    if (!(pressedButtons & Qt.LeftButton)) return
                                    const x = mapToItem(pane, event.x, event.y).x
                                    if (!dragging && Math.abs(x - pressX) >= Qt.styleHints.startDragDistance) dragging = true
                                    if (dragging) pane.moveColumnDrag(column.index, x, grabOffset)
                                }
                                onReleased: { if (dragging) pane.finishColumnDrag(true); else pane.releaseColumnHold() }
                                onCanceled: { if (dragging) pane.finishColumnDrag(false); else pane.releaseColumnHold(); dragging = false }
                                onClicked: event => {
                                    if (dragging) return
                                    if (event.button === Qt.RightButton) {
                                        const p = mapToItem(pane, event.x, event.y)
                                        columnMenu.popup(p.x, p.y)
                                    } else if (!pane.searchMode) {
                                        directoryModel.sortBy(["name", "date", "type", "size", "created"][column.index],
                                                              pane.sortColumn === column.index ? !pane.descending : false)
                                    }
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
                                onDoubleClicked: pane.fitColumns(column.index)
                            }
                        }
                    }
                }
            }
            Item {
                id: files
                Layout.fillWidth: true; Layout.fillHeight: true
                transform: Translate { id: filesShift }
                WheelHandler {
                    acceptedModifiers: Qt.ControlModifier
                    onWheel: event => { if (event.angleDelta.y !== 0) pane.zoomStep(event.angleDelta.y > 0 ? 0.05 : -0.05) }
                }
                // Rows kept in the cache buffer above the viewport must not paint over, or take
                // clicks from, the column header.
                clip: true
                ListView {
                    id: list
                    objectName: "fileList"
                    anchors.fill: parent; anchors.margins: 5
                    contentWidth: Math.max(width, pane.columnsWidth + 24)
                    flickableDirection: Flickable.AutoFlickIfNeeded
                    ScrollBar.horizontal: NookScrollBar {}
                    // Alive, fading and inert while a morph leaves it.
                    visible: pane.listLive
                    model: pane.listLive ? directoryModel : null
                    enabled: !pane.gridMode
                    opacity: pane.morphFrom === "list" ? pane.leavingOpacity : 1
                    reuseItems: true
                    cacheBuffer: 100
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: NookScrollBar {}
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
                    HoverPlate {
                        objectName: "rowPlate"
                        // In the content item (a view does not reparent children there itself),
                        // over the selection highlight and under the rows.
                        parent: list.contentItem; z: 0.5
                        hot: !pane.gridMode ? pane.plateRow : null
                        hotKey: hot ? hot.index : -1
                        animated: pane.animated
                        inset: 1
                        radius: 6; color: pane.theme.hover
                    }
                }
                GridView {
                    id: grid
                    anchors.fill: parent; anchors.margins: 8
                    visible: pane.gridLive
                    model: pane.gridLive ? directoryModel : null
                    enabled: pane.gridMode
                    opacity: pane.morphFrom === "grid" ? pane.leavingOpacity : 1
                    objectName: "fileGrid"
                    onCurrentIndexChanged: if (pane.gridMode && currentIndex !== pane.currentRow) pane.currentRow = currentIndex
                    // As many 176 px tiles as fit, widened to share the row, so no strip is left over.
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / (176 * pane.gs))))
                    // Its picture, the gaps and two lines of name (180 at the grid's own size).
                    cellHeight: Math.round(144 * pane.gs + pane.gridFont * 3.08 - 4)
                    reuseItems: true; cacheBuffer: 180
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: NookScrollBar {}
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
                    HoverPlate {
                        objectName: "rowPlate"
                        parent: grid.contentItem; z: 0.5
                        hot: pane.gridMode ? pane.plateRow : null
                        hotKey: hot ? hot.index : -1
                        animated: pane.animated
                        inset: 4
                        radius: 6; color: pane.theme.hover
                    }
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
                    // Past the last column is still the row (a click there picks it, a drag draws a band).
                    function emptyAt(x, y) {
                        if (onScrollbar(x, y)) return false
                        return rowAt(x, y) < 0
                    }
                    function bandAt(x, y) {
                        if (onScrollbar(x, y)) return false
                        if (pane.pickMode || emptyAt(x, y)) return true
                        if (pane.gridMode) return false
                        const columnX = mapToItem(view, x, y).x + view.contentX - 9
                        return columnX < pane.columnX(0) || columnX >= pane.columnX(0) + pane.columnWidth(0)
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
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: directoryModel.error || (pane.searchMode ? "没有匹配的项目"
                          : directoryModel.tagView ? "还没有带“" + directoryModel.title + "”标签的项目。右键文件选择标签，或按 Alt+数字键"
                          : directoryModel.tagFilter !== "" ? "这个文件夹里没有带“" + pane.tagStore.nameOf(directoryModel.tagFilter) + "”标签的项目"
                          : pane.emptyText); color: pane.muted; font.pixelSize: 16; width: Math.min(320, pane.width - 40); wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
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
                        else { pane.dropRow = -1; if (!pane.listing) { uri = directoryModel.uri; name = directoryModel.title } }
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
            readonly property real inset: view === grid ? 4 : 1
            // Glides between rows on the plate spring, keeping its speed while ↑/↓ is held;
            // appearing after an empty selection snaps instead of sweeping in.
            Glide {
                id: follow
                animated: pane.animated
                shown: glide.opacity > 0.4
                key: glide.view ? glide.view.currentIndex : -1
                targetX: glide.target ? glide.target.x + glide.inset : 0
                targetY: glide.target ? glide.target.y + glide.inset : 0
                targetWidth: glide.target ? glide.target.width - 2 * glide.inset : 0
                targetHeight: glide.target ? glide.target.height - 2 * glide.inset : 0
            }
            x: follow.x; y: follow.y; width: follow.width; height: follow.height
            z: 0; radius: 6
            color: pane.theme.selection
            border.width: 1; border.color: pane.theme.selectionBorder
            opacity: shown ? 1 : 0
            Behavior on opacity { enabled: pane.animated; NumberAnimation { duration: 90 } }
            Rectangle { visible: glide.view !== grid; width: 3; height: parent.height - 14; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; color: pane.theme.selectionBar; radius: 2 }
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
            required property string createdText
            required property double modified
            required property string kind
            required property string iconName
            required property string fullPath
            required property bool selected
            required property double bytes
            required property bool hiddenItem
            required property var tags
            readonly property real dotsWidth: tags.length ? (tags.length * 13 + 6) * pane.ls : 0
            property bool removing: false
            property bool pooled: false
            readonly property bool live: !removing && !pooled && index >= 0
            readonly property bool hovered: live && mouse.containsMouse && !pane.covered
            enabled: live && !leaving
            function clearHover() {
                if (pane.hoverOwner === item) { pane.hoverOwner = null; pane.hovered(({})) }
            }
            readonly property bool needsFolderSize: live && visible && !inGrid && !pane.gridMode && pane.columnVisible(3) && folder && !isLink && !directoryModel.loading
                                                    && y + height >= ownView.contentY && y <= ownView.contentY + ownView.height
            property string sizeRequestUri: ""
            property var sizeRequestModel: null
            readonly property var sizeModel: pane.directoryModel
            onSizeModelChanged: { releaseSize(); sizeRequestTimer.restart() }
            Timer { id: sizeRequestTimer; interval: 0; onTriggered: item.requestSize() }
            function releaseSize() {
                if (sizeRequestModel && sizeRequestUri) sizeRequestModel.releaseFolderSize(sizeRequestUri)
                sizeRequestUri = ""; sizeRequestModel = null
            }
            function requestSize() {
                if (sizeRequestUri && (!needsFolderSize || sizeRequestUri !== fileUri || sizeRequestModel !== pane.directoryModel)) releaseSize()
                if (needsFolderSize && !sizeRequestUri) {
                    sizeRequestUri = fileUri; sizeRequestModel = pane.directoryModel
                    sizeRequestModel.requestFolderSize(sizeRequestUri)
                }
            }
            onNeedsFolderSizeChanged: { if (!needsFolderSize) releaseSize(); else sizeRequestTimer.restart() }
            onFileUriChanged: sizeRequestTimer.restart()
            Component.onCompleted: sizeRequestTimer.restart()
            onLiveChanged: if (!live) clearHover()
            onHoveredChanged: if (!hovered) clearHover()
            Component.onDestruction: { clearHover(); releaseSize() }
            readonly property bool current: live && pane.currentRow === index
            readonly property bool dropTarget: live && pane.dropRow === index
            // Hidden entries read as "ghosted", like Windows: dimmed icon and text, same layout.
            readonly property real ghost: hiddenItem ? 0.5 : 1
            // Each view lays its own rows out: during a morph both are alive. The one being
            // left keeps its rows as inert, fading ghosts.
            readonly property Item ownView: ListView.view ? ListView.view : GridView.view
            readonly property bool inGrid: ownView === grid
            readonly property bool leaving: pane.morphFrom !== "" && inGrid !== pane.gridMode
            objectName: leaving ? "fileEntryLeaving" : "fileEntry"
            width: inGrid ? grid.cellWidth : list.contentWidth
            height: inGrid ? grid.cellHeight : pane.rowHeight
            // Its part in a reflow or morph (see motionKind): where it was, where it is now, and
            // how far along this row is.
            readonly property bool moving: pane.motionKind !== "" && live && !leaving
            readonly property var from: moving ? pane.motionFrom[fileUri] : undefined
            readonly property point at: moving && ownView ? pane.placeOf(ownView, x, y, ownView.contentX, ownView.contentY) : Qt.point(0, 0)
            readonly property real travel: !moving ? 1 : pane.motionKind === "morph" ? pane.morphProgress(index - pane.motionFirst) : pane.reflowProgress()
            readonly property bool reflowing: moving && pane.motionKind === "reflow"
            // Everything but the icon: labels cross-fade in a morph, rows new to the screen fade in.
            readonly property real contentOpacity: !moving ? 1 : !from ? (reflowing ? pane.appearOpacity() : travel) : reflowing ? 1 : pane.labelOpacity
            readonly property real iconOpacity: leaving ? 0 : !moving || from ? 1 : contentOpacity
            readonly property var flight: moving && from && !reflowing ? pane.flight(from, pane.iconRect(at, inGrid, height), travel) : ({dx: 0, dy: 0, scale: 1})
            transform: Translate {
                // A reflow carries the whole row; a morph lifts rows new to the screen into place.
                x: item.reflowing && item.from ? (item.from.x - item.at.x) * (1 - item.travel) : 0
                y: !item.moving ? 0 : item.from ? (item.reflowing ? (item.from.y - item.at.y) * (1 - item.travel) : 0) : item.reflowing ? 0 : 12 * (1 - item.travel)
            }
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
            // new row's state at once instead of the old row's.
            function recycle() {
                clearHover()
                opacity = 1; scale = 1; removing = false; pooled = false
                newFlash.stop(); flash.opacity = 0
            }
            ListView.onRemove: { removing = true; clearHover() }
            GridView.onRemove: { removing = true; clearHover() }
            // A row that appears in place (a new folder, a paste, a file arriving) flashes once,
            // so the eye finds where it landed. Streamed and reflowed rows do not.
            function flashNew() { if (pane.rowAnimations && !pane.streaming) newFlash.restart() }
            ListView.onAdd: flashNew()
            GridView.onAdd: flashNew()
            NumberAnimation { id: newFlash; target: flash; property: "opacity"; from: 1; to: 0; duration: 900; easing.type: Easing.InQuad }
            ListView.onPooled: { pooled = true; clearHover() }
            GridView.onPooled: { pooled = true; clearHover() }
            ListView.onReused: recycle()
            GridView.onReused: recycle()
            // Each state is its own constant-colour layer and only its opacity animates.
            // Animating one colour from "transparent" (transparent *black*) to the selection
            // tint passes through grey, which flashed every row on Select All.
            Item {
                anchors.fill: parent; anchors.margins: item.inGrid ? 4 : 1
                opacity: item.contentOpacity
                Rectangle { id: flash; anchors.fill: parent; radius: 6; color: pane.theme.activeFill; opacity: 0; visible: opacity > 0 }
                // Hover is the view's shared plate (rowPlate), which glides from row to row.
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
                anchors.fill: parent; anchors.margins: item.inGrid ? 4 : 1
                active: pane.animated && item.hovered && !dragHandler.active && !item.dropTarget
                center: Qt.point(mouse.mouseX - (item.inGrid ? 4 : 1), mouse.mouseY - (item.inGrid ? 4 : 1))
                radius: item.inGrid ? 130 * pane.gs : 190 * pane.ls
                strength: item.selected ? 0.3 : 0.42
                tint: pane.theme.selectionGlow
                opacity: active ? (pane.dark ? 0.55 : 0.3) : 0
            }
            Item {
                visible: !item.inGrid
                x: 9; width: pane.columnsWidth; height: parent.height
                opacity: item.hiddenItem ? 0.85 : 1
                Rectangle {
                    visible: pane.draggedColumn >= 0
                    x: pane.draggedColumn >= 0 ? pane.columnX(pane.draggedColumn) : 0
                    width: pane.draggedColumn >= 0 ? pane.columnWidth(pane.draggedColumn) : 0
                    height: parent.height; color: pane.theme.accent; opacity: 0.055
                }
                Item {
                    objectName: "nameColumnCell"
                    z: pane.liftedColumn === 0 ? 1 : 0
                    ColumnDragBackground { columnId: 0; selected: item.selected }
                    x: pane.columnX(0); width: pane.columnWidth(0); height: parent.height
                    Icon {
                        x: 7 * pane.ls; anchors.verticalCenter: parent.verticalCenter; name: item.iconName; width: 23 * pane.ls; height: 23 * pane.ls; opacity: item.ghost * item.iconOpacity
                        transform: [Scale { xScale: item.flight.scale; yScale: item.flight.scale }, Translate { x: item.flight.dx; y: item.flight.dy }]
                    }
                    Column {
                        id: nameColumn
                        opacity: item.contentOpacity
                        x: 39 * pane.ls; width: parent.width - 51 * pane.ls; anchors.verticalCenter: parent.verticalCenter; spacing: 3 * pane.ls
                        // Room is left after the name for its tag dots (drawn above the row's pointer area).
                        Text { id: nameText; width: Math.min(implicitWidth, parent.width - item.dotsWidth); text: pane.highlightText(item.fileName) + (item.isLink ? " ↗" : ""); textFormat: Text.StyledText; color: pane.ink; opacity: item.hiddenItem ? 0.62 : 1; font.pixelSize: 15 * pane.ls; elide: Text.ElideMiddle }
                        Text { visible: pane.listing; width: parent.width; text: pane.highlightText(item.fullPath); textFormat: Text.StyledText; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 12 * pane.ls; elide: Text.ElideMiddle }
                    }
                }
                Text { objectName: "modifiedTimeCell"; z: pane.liftedColumn === 1 ? 1 : 0; visible: pane.columnVisible(1); x: pane.columnX(1); width: pane.columnWidth(1); height: parent.height; leftPadding: 7 * pane.ls; rightPadding: 10 * pane.ls; verticalAlignment: Text.AlignVCenter; opacity: item.contentOpacity; text: item.modifiedText; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 13 * pane.ls ; ColumnDragBackground { columnId: 1; selected: item.selected } }
                Text { objectName: "typeColumnCell"; z: pane.liftedColumn === 2 ? 1 : 0; visible: pane.columnVisible(2); x: pane.columnX(2); width: pane.columnWidth(2); height: parent.height; leftPadding: 7 * pane.ls; rightPadding: 10 * pane.ls; verticalAlignment: Text.AlignVCenter; opacity: item.contentOpacity; text: item.kind; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 12 * pane.ls ; ColumnDragBackground { columnId: 2; selected: item.selected } }
                Text { objectName: "sizeColumnCell"; z: pane.liftedColumn === 3 ? 1 : 0; visible: pane.columnVisible(3); x: pane.columnX(3); width: pane.columnWidth(3); height: parent.height; leftPadding: 7 * pane.ls; rightPadding: 10 * pane.ls; verticalAlignment: Text.AlignVCenter; opacity: item.contentOpacity; text: item.sizeText; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 13 * pane.ls; horizontalAlignment: Text.AlignRight ; ColumnDragBackground { columnId: 3; selected: item.selected } }
                Text { objectName: "createdTimeCell"; z: pane.liftedColumn === 4 ? 1 : 0; visible: pane.columnVisible(4); x: pane.columnX(4); width: pane.columnWidth(4); height: parent.height; leftPadding: 7 * pane.ls; rightPadding: 10 * pane.ls; verticalAlignment: Text.AlignVCenter; opacity: item.contentOpacity; text: item.createdText; elide: Text.ElideRight; color: item.selected ? pane.theme.selectionText : pane.muted; font.pixelSize: 13 * pane.ls ; ColumnDragBackground { columnId: 4; selected: item.selected } }
            }
            Column {
                visible: item.inGrid
                anchors.fill: parent; anchors.margins: 12 * pane.gs; spacing: 8 * pane.gs
                Item {
                    width: parent.width; height: 112 * pane.gs
                    opacity: item.iconOpacity
                    // Flies about the centred 112 px square that iconRect() names.
                    transform: [Scale { origin.x: 20 * pane.gs; xScale: item.flight.scale; yScale: item.flight.scale }, Translate { x: item.flight.dx; y: item.flight.dy }]
                    Icon { objectName: "gridFallbackIcon"; anchors.centerIn: parent; name: item.iconName; width: 84 * pane.gs; height: 84 * pane.gs; visible: thumbnail.status !== Image.Ready; opacity: item.ghost }
                    Image {
                        id: thumbnail
                        anchors.fill: parent
                        objectName: "gridThumbnail"
                        source: item.inGrid && pane.thumbnails && (item.iconName === "image" || item.iconName === "video" || /\.pdf$/i.test(item.fileName)) ? "image://thumb/" + encodeURIComponent(item.fileUri) + "|" + item.bytes + ":" + item.modified : ""
                        asynchronous: true; fillMode: Image.PreserveAspectFit
                        sourceSize: Qt.size(256, 256)
                        opacity: item.ghost
                    }
                }
                Text { width: parent.width; height: pane.gridFont * 3.08; text: pane.highlightText(item.fileName); textFormat: Text.StyledText; color: pane.ink; opacity: (item.hiddenItem ? 0.62 : 1) * item.contentOpacity; font.pixelSize: pane.gridFont; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
            }
            // Selection mode marker: a check circle on the icon.
            Rectangle {
                objectName: "pickMark"
                visible: pane.pickMode
                opacity: item.contentOpacity
                width: item.inGrid ? 22 * pane.gs : 14 * pane.ls; height: width; radius: width / 2
                x: item.inGrid ? 14 * pane.gs : 9 + pane.columnX(0) + (7 + 23 + 4) * pane.ls - width
                y: item.inGrid ? 14 * pane.gs : item.height / 2 + (11.5 + 4) * pane.ls - width
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
            // The item's colour tags: after the name in the list, at the icon's corner in the grid.
            // Pressing one shows only that tag's items in this folder (again: all of them).
            Row {
                objectName: "tagDots"
                visible: item.tags.length > 0
                opacity: item.contentOpacity
                readonly property real zoom: item.inGrid ? pane.gs : pane.ls
                spacing: 3 * zoom
                x: item.inGrid ? item.width - 18 * zoom - width : 9 + pane.columnX(0) + (39 + 6) * zoom + nameText.width
                y: item.inGrid ? 18 * zoom : nameColumn.y + (nameText.height - height) / 2
                Repeater {
                    model: item.tags
                    AbstractButton {
                        required property string modelData
                        objectName: "tagDot"
                        width: 10 * parent.zoom; height: width
                        focusPolicy: Qt.NoFocus
                        hoverEnabled: true
                        onClicked: pane.pickTag(modelData)
                        background: Rectangle {
                            radius: width / 2
                            scale: parent.hovered ? 1.3 : 1
                            Behavior on scale { enabled: pane.animated; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                            color: pane.theme.tagColor(parent.modelData)
                            border.width: directoryModel.tagFilter === parent.modelData ? 2 : 0
                            border.color: pane.theme.ink
                        }
                        NookToolTip { visible: parent.hovered; text: (pane.tagStore.tags, pane.tagStore.nameOf(parent.modelData)) + (pane.searchMode ? "" : directoryModel.tagView ? " · 打开此标签" : " · 只看此标签"); delay: 500 }
                    }
                }
            }
            NookToolTip { objectName: "fileEntryToolTip"; visible: item.hovered && !dragHandler.active; text: item.fileName + "\n" + item.fullPath; delay: 1000 }
        }
    }
}
