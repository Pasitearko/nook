import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "motion.js" as Motion

Item {
    id: sidebar
    required property var controller
    required property var host
    property bool searching: false
    // One line per place, no capacity meters and no trash: for the file chooser.
    property bool compact: false
    signal activate(var place)
    signal removeRequested(string uri, real x, real y)
    // A colour tag's row: opened, or its menu asked for (rename, take it off everything).
    signal openTag(string color)
    signal tagMenuRequested(string color, real x, real y)
    readonly property bool dark: host.dark
    readonly property var theme: host.theme
    readonly property color ink: host.ink
    readonly property color muted: host.muted
    readonly property var quickPlaces: controller.places.filter(p => !p.disk && !(compact && String(p.uri).startsWith("trash:")))
    readonly property var disks: controller.places.filter(p => p.disk)
    readonly property var dragGhost: ghost
    readonly property bool animated: host.animations === true
    property Item currentRow: null // the place showing in the active pane, under the pill
    // The sections in the user's order (the file chooser has no tags).
    readonly property var sections: (controller.settings.values.sidebarSections || ["places", "tags", "disks"]).filter(name => !(compact && name === "tags"))
    // Narrow: disks go by their short names ("Linux", "Windows", "Data").
    readonly property bool narrow: width < 200
    component PlaceRow: Rectangle {
        id: row
        required property var entry
        property bool disk: false
        objectName: disk ? "diskPlace" : "quickPlace"
        Layout.fillWidth: true
        readonly property bool meter: disk && !sidebar.compact
        implicitHeight: meter ? 61 : sidebar.compact ? 32 : 38
        radius: 8
        readonly property bool current: controller.activePane.uri === entry.uri && !sidebar.searching
        readonly property bool dropTarget: dragTarget.containsDrag && (dragTarget.reorder || dragTarget.allowed)
        // Hover and the current place are drawn by the shared plate and pill beneath the rows.
        color: dropTarget ? sidebar.theme.dropFill : "transparent"
        border.width: dropTarget ? 1 : 0; border.color: sidebar.theme.accent
        readonly property bool hovering: (pointer.containsMouse || dragTarget.containsDrag) && !current && !dropTarget
        onHoveringChanged: placePlate.track(row, hovering)
        onCurrentChanged: {
            if (current) sidebar.currentRow = row
            else if (sidebar.currentRow === row) sidebar.currentRow = null
        }
        Component.onCompleted: if (current) sidebar.currentRow = row
        ColumnLayout {
            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 3
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: row.meter ? 7 : 0; spacing: 10
                Icon { name: row.entry.icon; tint: row.current ? sidebar.theme.accentText : sidebar.theme.icon; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                Text { text: row.disk && sidebar.narrow ? String(row.entry.title).split(" · ")[0] : row.entry.title; color: sidebar.ink; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            Rectangle {
                visible: row.meter; Layout.fillWidth: true; Layout.leftMargin: 28; height: 3; radius: 2
                color: sidebar.theme.track
                Rectangle { height: 3; width: parent.width * (row.entry.used || 0); radius: 2; color: row.entry.used > 0.9 ? sidebar.theme.danger : sidebar.theme.meter }
            }
            Text { visible: row.meter; text: sidebar.narrow ? String(row.entry.capacity || "").replace(" 可用 / ", " / ") : row.entry.capacity || ""; color: sidebar.muted; font.pixelSize: 11; Layout.leftMargin: 28; Layout.bottomMargin: 7; Layout.fillWidth: true; elide: Text.ElideRight }
        }
        MouseArea {
            id: pointer
            anchors.fill: parent; hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            drag.target: row.disk ? null : sidebar.dragGhost
            drag.axis: Drag.YAxis
            onPressed: event => {
                if (!row.disk && event.button === Qt.LeftButton) {
                    sidebar.dragGhost.sourceUri = row.entry.uri; sidebar.dragGhost.title = row.entry.title
                    const p = row.mapToItem(sidebar, 0, 0)
                    sidebar.dragGhost.x = p.x; sidebar.dragGhost.y = p.y
                }
            }
            onPositionChanged: if (drag.active && !row.disk) sidebar.dragGhost.dragging = true
            onReleased: {
                // Dropping rebuilds the place delegates; capture the surviving sidebar item first.
                const visual = sidebar.dragGhost
                if (visual.dragging) visual.Drag.drop()
                visual.dragging = false
            }
            onCanceled: sidebar.dragGhost.dragging = false
            onClicked: event => {
                if (event.button === Qt.RightButton && !row.disk) {
                    const p = mapToItem(sidebar, event.x, event.y)
                    sidebar.removeRequested(row.entry.uri, p.x, p.y)
                } else if (event.button === Qt.LeftButton) sidebar.activate(row.entry)
            }
        }
        DropArea {
            id: dragTarget
            anchors.fill: parent
            // Either a quick-access reorder (internal ghost) or files dragged over this place:
            // they can be dropped on it, and resting here opens it (an unmounted disk is mounted
            // first) so the drag can continue into its folders.
            property bool reorder: false
            property bool allowed: false
            property var urls: []
            function hint(drag) { allowed = sidebar.host.dropHint(dragTarget, drag.x, drag.y, urls, row.entry.uri || "", row.entry.title) }
            onEntered: drag => {
                reorder = !row.disk && drag.keys.indexOf("nook-quick-access") >= 0
                urls = reorder ? [] : sidebar.host.dragUris(drag)
                if (reorder) return
                if (urls.length === 0) { drag.accepted = false; return }
                hint(drag)
                if (!row.current) spring.restart()
            }
            onPositionChanged: drag => { if (!reorder && urls.length) hint(drag) }
            onExited: { spring.stop(); if (!reorder) sidebar.host.clearDropHint(); reorder = false; allowed = false; urls = [] }
            onDropped: drop => {
                spring.stop()
                if (reorder) { controller.reorderQuickAccess(drop.source.sourceUri, row.entry.uri); drop.acceptProposedAction() }
                else if (allowed) { controller.dragDrop(urls, row.entry.uri); drop.accept(Qt.CopyAction) }
                if (!reorder) sidebar.host.clearDropHint()
                reorder = false; allowed = false; urls = []
            }
            Timer { id: spring; interval: sidebar.host.springDelay; onTriggered: sidebar.host.springLoad(() => controller.activatePlace(row.entry)) }
        }
        SpringProgress { running: spring.running; color: sidebar.theme.accent; duration: spring.interval }
        NookToolTip { visible: pointer.containsMouse && !sidebar.dragGhost.dragging; text: row.entry.uri || row.entry.title; delay: 1100 }
    }
    // A colour tag: its dot, name and how many items carry it. It opens the tag's view, and
    // files dropped on it get the tag.
    component TagRow: Rectangle {
        id: tagRow
        required property var tag
        objectName: "tagPlace"
        readonly property var entry: ({uri: "tag:" + tag.id})
        readonly property bool meter: false
        Layout.fillWidth: true
        implicitHeight: 34
        radius: 8
        readonly property bool current: controller.activePane.uri === entry.uri && !sidebar.searching
        readonly property bool dropTarget: tagDrop.containsDrag && tagDrop.urls.length > 0
        color: dropTarget ? sidebar.theme.dropFill : "transparent"
        border.width: dropTarget ? 1 : 0; border.color: sidebar.theme.tagColor(tag.id)
        opacity: mover.kind === "tag" && mover.key === tag.id ? 0.35 : 1
        readonly property bool hovering: (tagPointer.containsMouse || tagDrop.containsDrag) && !current && !dropTarget
        onHoveringChanged: placePlate.track(tagRow, hovering)
        onCurrentChanged: {
            if (current) sidebar.currentRow = tagRow
            else if (sidebar.currentRow === tagRow) sidebar.currentRow = null
        }
        Component.onCompleted: if (current) sidebar.currentRow = tagRow
        Component.onDestruction: if (sidebar.currentRow === tagRow) sidebar.currentRow = null
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 12; spacing: 12
            Rectangle {
                Layout.preferredWidth: 10; Layout.preferredHeight: 10; radius: 5
                color: sidebar.theme.tagColor(tagRow.tag.id)
                scale: tagRow.dropTarget ? 1.35 : 1
                Behavior on scale { enabled: sidebar.animated; NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            }
            Text { text: tagRow.tag.name; color: sidebar.ink; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
            Text { objectName: "tagCount"; text: tagRow.tag.count; visible: tagRow.tag.count > 0; color: sidebar.muted; font.pixelSize: 12 }
        }
        MouseArea {
            id: tagPointer
            anchors.fill: parent; hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true
            property real pressY: 0
            property bool moved: false
            // Dragged up or down, the tag moves among the others.
            onPressed: event => { pressY = mapToItem(content, 0, event.y).y; moved = false }
            onPositionChanged: event => {
                if (!(pressedButtons & Qt.LeftButton)) return
                const y = mapToItem(content, 0, event.y).y
                if (!moved && Math.abs(y - pressY) < 6) return
                if (!moved) { moved = true; mover.begin("tag", tagRow.tag.id, tagRow.tag.name, tagRow, y) }
                mover.follow(y)
            }
            onReleased: if (moved) mover.drop()
            onCanceled: mover.cancel()
            onClicked: event => {
                if (moved) return
                if (event.button === Qt.RightButton) {
                    const p = mapToItem(sidebar, event.x, event.y)
                    sidebar.tagMenuRequested(tagRow.tag.id, p.x, p.y)
                } else sidebar.openTag(tagRow.tag.id)
            }
        }
        DropArea {
            id: tagDrop
            anchors.fill: parent
            property var urls: []
            onEntered: drag => {
                urls = drag.keys.indexOf("nook-quick-access") >= 0 ? [] : sidebar.host.dragUris(drag).filter(u => String(u).startsWith("file:"))
                if (!urls.length) drag.accepted = false
            }
            onExited: urls = []
            onDropped: drop => {
                if (urls.length) { controller.tags.set(urls, tagRow.tag.id, true); drop.accept(Qt.LinkAction) }
                urls = []
            }
        }
        NookToolTip { visible: tagPointer.containsMouse && !mover.active; text: "查看带“" + tagRow.tag.name + "”标签的项目 · 拖入文件即可加上此标签 · 上下拖动调整顺序"; delay: 1100 }
    }
    // A section's title. Dragged up or down, the whole section moves.
    component SectionHeader: Item {
        id: header
        required property string key
        required property Item section
        readonly property string title: key === "places" ? "快速访问" : key === "tags" ? "标签" : "磁盘"
        objectName: "sectionHeader_" + key
        Layout.fillWidth: true
        implicitHeight: 26
        Label { x: 12; anchors.verticalCenter: parent.verticalCenter; text: header.title; color: sidebar.muted; font.pixelSize: 12 }
        // ⋮⋮ under the pointer: it can be dragged.
        Text {
            anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter
            text: "⠿"; color: sidebar.theme.faint; font.pixelSize: 13
            opacity: headerPointer.containsMouse && !sidebar.compact && !mover.active ? 1 : 0
            Behavior on opacity { enabled: sidebar.animated; NumberAnimation { duration: 120 } }
        }
        MouseArea {
            id: headerPointer
            anchors.fill: parent
            enabled: !sidebar.compact
            hoverEnabled: true
            preventStealing: true
            cursorShape: mover.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property real pressY: 0
            property bool moved: false
            onPressed: event => { pressY = mapToItem(content, 0, event.y).y; moved = false }
            onPositionChanged: event => {
                if (!pressed) return
                const y = mapToItem(content, 0, event.y).y
                if (!moved && Math.abs(y - pressY) < 6) return
                if (!moved) { moved = true; mover.begin("section", header.key, header.title, header.section, y) }
                mover.follow(y)
            }
            onReleased: if (moved) mover.drop()
            onCanceled: mover.cancel()
        }
        NookToolTip { visible: headerPointer.containsMouse && !sidebar.compact && !mover.active; text: "上下拖动调整分组顺序"; delay: 1100 }
    }
    ScrollView {
        id: quickScroll
        objectName: "sidebarScroll"
        // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
        // No elastic overshoot past the first or last place, as in the file list.
        Component.onCompleted: { contentItem.acceptedButtons = Qt.NoButton; contentItem.boundsBehavior = Flickable.StopAtBounds }
        ScrollBar.vertical: NookScrollBar { parent: quickScroll; x: quickScroll.width - width; height: quickScroll.availableHeight }
        anchors.fill: parent
        clip: true; contentWidth: availableWidth
        Item {
            id: content
            width: quickScroll.availableWidth; implicitHeight: placeColumn.implicitHeight
            HoverPlate { id: placePlate; objectName: "placePlate"; animated: sidebar.animated; color: sidebar.theme.hover }
            // The current place: a pill that stretches toward the new row, its leading edge on a
            // stiff spring and its trailing edge on a softer one, at most 1.9 rows long and a
            // touch thinner while stretched (as in Pulse's sidebar).
            Item {
                id: pill
                objectName: "placePill"
                readonly property Item row: sidebar.currentRow
                // Kept while it fades out, and across the moment the place rows are rebuilt.
                // Rows sit in their section, which sits in the column.
                readonly property rect rowRect: row && row.parent ? Qt.rect(row.parent.x + row.x, row.parent.y + row.y, row.width, row.height) : Qt.rect(0, 0, 0, 0)
                property rect held
                property string heldKey
                property bool heldMeter: false
                onRowRectChanged: if (row) held = rowRect
                onRowChanged: if (row) { held = rowRect; heldKey = String(row.entry.uri); heldMeter = row.meter }
                Glide {
                    id: lead
                    animated: sidebar.animated; shown: pill.opacity > 0; key: pill.heldKey
                    stiffness: Motion.pillLead.k; damping: Motion.pillLead.c
                    targetX: pill.held.x; targetY: pill.held.y; targetWidth: pill.held.width; targetHeight: pill.held.height
                }
                Glide {
                    id: trail
                    animated: sidebar.animated; shown: pill.opacity > 0; key: pill.heldKey
                    stiffness: Motion.pillTrail.k; damping: Motion.pillTrail.c
                    targetX: pill.held.x; targetY: pill.held.y; targetWidth: pill.held.width; targetHeight: pill.held.height
                }
                readonly property real rowHeight: Math.max(1, lead.height)
                readonly property real upper: Math.min(lead.y, trail.y)
                readonly property real lower: Math.max(lead.y + lead.height, trail.y + trail.height)
                readonly property real span: Math.min(lower - upper, rowHeight * 1.9)
                readonly property real stretch: Math.max(0, Math.min(1, (span - rowHeight) / rowHeight))
                // Moving down, the bottom edge leads; moving up, the top.
                x: lead.x + 2 * stretch; width: lead.width - 4 * stretch
                y: lead.y >= trail.y ? lower - span : upper; height: span
                opacity: row ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { enabled: sidebar.animated; NumberAnimation { duration: Motion.plateFadeInMs } }
                Rectangle { anchors.fill: parent; radius: 8; color: sidebar.theme.activeFill }
                // A short terracotta bar, echoing the file list's selection bar.
                Rectangle {
                    width: 3; height: 18; radius: 2; x: 3
                    anchors.verticalCenter: parent.verticalCenter; anchors.verticalCenterOffset: pill.heldMeter ? -10 * (1 - pill.stretch) : 0
                    color: sidebar.theme.accent
                }
            }
            ColumnLayout {
                id: placeColumn
                width: quickScroll.availableWidth; spacing: sidebar.compact ? 2 : 4
                Repeater {
                    id: sectionRepeater
                    model: sidebar.sections
                    ColumnLayout {
                        id: section
                        required property string modelData
                        required property int index
                        objectName: "sidebarSection_" + modelData
                        Layout.fillWidth: true
                        Layout.topMargin: index === 0 ? 2 : 0
                        spacing: sidebar.compact ? 2 : 4
                        // Lifted while it is dragged elsewhere.
                        opacity: mover.kind === "section" && mover.key === modelData ? 0.35 : 1
                        Rectangle { visible: section.index > 0; Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: sidebar.compact ? 4 : 11; Layout.bottomMargin: sidebar.compact ? 2 : 4; height: 1; color: sidebar.theme.line }
                        SectionHeader { key: section.modelData; section: section }
                        Repeater { model: section.modelData === "places" ? sidebar.quickPlaces : []; PlaceRow { required property var modelData; entry: modelData } }
                        Repeater { model: section.modelData === "tags" ? controller.tags.tags : []; TagRow { required property var modelData; tag: modelData } }
                        Repeater { model: section.modelData === "disks" ? sidebar.disks : []; PlaceRow { required property var modelData; entry: modelData; disk: true } }
                    }
                }
                Item { height: 12 }
            }
            // Moving a section or a tag: a ghost under the pointer and a line where it will land.
            Item {
                id: mover
                objectName: "sidebarMover"
                anchors.fill: parent
                z: 30
                property string kind: ""
                property string key: ""
                property string title: ""
                property real grab: 0      // pointer to the ghost's top
                property real pointerY: 0
                property int slot: -1      // where it lands, among the others
                property real lineY: 0
                readonly property bool active: kind !== ""
                // The others, top to bottom: {key, top, bottom} in content coordinates.
                function others() {
                    let list = []
                    if (kind === "section") {
                        for (let i = 0; i < sectionRepeater.count; ++i) {
                            const s = sectionRepeater.itemAt(i)
                            if (s && s.modelData !== key) list.push({key: s.modelData, top: s.y, bottom: s.y + s.height})
                        }
                    } else {
                        for (let i = 0; i < sectionRepeater.count; ++i) {
                            const s = sectionRepeater.itemAt(i)
                            if (!s || s.modelData !== "tags") continue
                            for (const child of s.children)
                                if (child.objectName === "tagPlace" && child.tag.id !== key)
                                    list.push({key: child.tag.id, top: s.y + child.y, bottom: s.y + child.y + child.height})
                        }
                    }
                    return list
                }
                function begin(what, id, name, item, y) {
                    kind = what; key = id; title = name
                    const top = item.mapToItem(content, 0, 0).y
                    grab = y - top
                    moverGhost.height = what === "section" ? 30 : item.height
                    follow(y)
                }
                function follow(y) {
                    pointerY = y
                    const list = others()
                    let n = 0
                    while (n < list.length && (list[n].top + list[n].bottom) / 2 < y) ++n
                    slot = n
                    lineY = list.length === 0 ? y : n < list.length ? list[n].top - 2 : list[list.length - 1].bottom + 2
                }
                function drop() {
                    if (kind === "section") {
                        let order = sidebar.sections.filter(name => name !== key)
                        order.splice(slot, 0, key)
                        // The file chooser leaves tags out; they keep their place among the rest.
                        if (order.indexOf("tags") < 0) order.splice(Math.min(1, order.length), 0, "tags")
                        controller.settings.set("sidebarSections", order)
                    } else if (kind === "tag") controller.tags.move(key, slot)
                    cancel()
                }
                function cancel() { kind = ""; key = ""; slot = -1 }
                Rectangle {
                    visible: mover.active
                    x: 10; width: parent.width - 20; height: 2; radius: 1
                    y: mover.lineY - 1
                    color: sidebar.theme.accent
                }
                Rectangle {
                    id: moverGhost
                    visible: mover.active
                    x: 4; width: parent.width - 8; radius: 8
                    y: mover.pointerY - mover.grab
                    color: sidebar.theme.raised; border.color: sidebar.theme.accent
                    opacity: 0.94
                    Rectangle { anchors.fill: parent; anchors.topMargin: 4; anchors.margins: -2; z: -1; radius: 10; color: sidebar.theme.shadow }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter; x: 14; spacing: 10
                        Rectangle { visible: mover.kind === "tag"; width: 10; height: 10; radius: 5; anchors.verticalCenter: parent.verticalCenter; color: sidebar.theme.tagColor(mover.key) }
                        Text { text: mover.title; color: sidebar.ink; font.pixelSize: mover.kind === "section" ? 13 : 14; font.weight: mover.kind === "section" ? Font.DemiBold : Font.Normal }
                    }
                }
            }
        }
    }
    Rectangle {
        id: ghost
        property bool dragging: false
        property string sourceUri: ""
        property string title: ""
        // Lifted with a pop as the drag starts, set down with a short fade when it ends.
        visible: dragging || setDown.running; z: 20
        width: sidebar.width - 8; height: 38; radius: 8; opacity: 0.92
        color: sidebar.theme.raised; border.color: sidebar.theme.accent
        onDraggingChanged: {
            if (!sidebar.animated) { lift.stop(); setDown.stop(); scale = 1; opacity = 0.92; return }
            if (dragging) { setDown.stop(); lift.restart() } else setDown.restart()
        }
        Rectangle { anchors.fill: parent; anchors.topMargin: 4; anchors.margins: -2; z: -1; radius: 10; color: sidebar.theme.shadow }
        ParallelAnimation {
            id: lift
            NumberAnimation { target: ghost; property: "scale"; from: 0.92; to: 1; duration: Motion.pop.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.pop.curve }
            NumberAnimation { target: ghost; property: "opacity"; from: 0; to: 0.92; duration: 90 }
        }
        ParallelAnimation {
            id: setDown
            NumberAnimation { target: ghost; property: "scale"; to: 0.96; duration: 140; easing.type: Easing.OutCubic }
            NumberAnimation { target: ghost; property: "opacity"; to: 0; duration: 140; easing.type: Easing.OutCubic }
        }
        Drag.active: dragging
        Drag.source: ghost
        Drag.keys: ["nook-quick-access"]
        Drag.supportedActions: Qt.MoveAction
        Drag.proposedAction: Qt.MoveAction
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2
        Text { anchors.centerIn: parent; text: ghost.title; color: sidebar.ink; font.pixelSize: 14 }
    }
}
