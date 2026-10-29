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
    readonly property bool dark: host.dark
    readonly property var theme: host.theme
    readonly property color ink: host.ink
    readonly property color muted: host.muted
    readonly property var quickPlaces: controller.places.filter(p => !p.disk && !(compact && String(p.uri).startsWith("trash:")))
    readonly property var disks: controller.places.filter(p => p.disk)
    readonly property var dragGhost: ghost
    readonly property bool animated: host.animations === true
    property Item currentRow: null // the place showing in the active pane, under the pill
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
                Text { text: row.entry.title; color: sidebar.ink; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            Rectangle {
                visible: row.meter; Layout.fillWidth: true; Layout.leftMargin: 28; height: 3; radius: 2
                color: sidebar.theme.track
                Rectangle { height: 3; width: parent.width * (row.entry.used || 0); radius: 2; color: row.entry.used > 0.9 ? sidebar.theme.danger : sidebar.theme.meter }
            }
            Text { visible: row.meter; text: row.entry.capacity || ""; color: sidebar.muted; font.pixelSize: 11; Layout.leftMargin: 28; Layout.bottomMargin: 7; Layout.fillWidth: true; elide: Text.ElideRight }
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
    ScrollView {
        id: quickScroll
        // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
        // No elastic overshoot past the first or last place, as in the file list.
        Component.onCompleted: { contentItem.acceptedButtons = Qt.NoButton; contentItem.boundsBehavior = Flickable.StopAtBounds }
        ScrollBar.vertical: NookScrollBar { parent: quickScroll; x: quickScroll.width - width; height: quickScroll.availableHeight }
        anchors.fill: parent
        clip: true; contentWidth: availableWidth
        Item {
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
                readonly property rect rowRect: row ? Qt.rect(row.x, row.y, row.width, row.height) : Qt.rect(0, 0, 0, 0)
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
                Label { text: "快速访问"; color: sidebar.muted; font.pixelSize: 12; Layout.leftMargin: 12; Layout.topMargin: 6; Layout.bottomMargin: 6 }
                Repeater { model: sidebar.quickPlaces; PlaceRow { required property var modelData; entry: modelData } }
                Rectangle { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: sidebar.compact ? 8 : 15; Layout.bottomMargin: sidebar.compact ? 4 : 8; height: 1; color: sidebar.theme.line }
                Label { text: "磁盘"; color: sidebar.muted; font.pixelSize: 12; Layout.leftMargin: 12; Layout.bottomMargin: 6 }
                Repeater { model: sidebar.disks; PlaceRow { required property var modelData; entry: modelData; disk: true } }
                Item { height: 12 }
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
