import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: sidebar
    required property var controller
    required property var host
    property bool searching: false
    signal activate(var place)
    signal removeRequested(string uri, real x, real y)
    readonly property bool dark: host.dark
    readonly property var theme: host.theme
    readonly property color ink: host.ink
    readonly property color muted: host.muted
    readonly property var quickPlaces: controller.places.filter(p => !p.disk)
    readonly property var disks: controller.places.filter(p => p.disk)
    readonly property var dragGhost: ghost
    component PlaceRow: Rectangle {
        id: row
        required property var entry
        property bool disk: false
        objectName: disk ? "diskPlace" : "quickPlace"
        Layout.fillWidth: true
        implicitHeight: disk ? 61 : 38
        radius: 8
        readonly property bool current: controller.activePane.uri === entry.uri && !sidebar.searching
        color: dragTarget.containsDrag ? sidebar.theme.dropFill : current ? sidebar.theme.activeFill : pointer.containsMouse ? sidebar.theme.hover : "transparent"
        border.width: dragTarget.containsDrag ? 1 : 0; border.color: sidebar.theme.accent
        // Current place: a short terracotta bar, echoing the file list's selection bar.
        Rectangle { visible: row.current; width: 3; height: 18; radius: 2; anchors.left: parent.left; anchors.leftMargin: 3; anchors.verticalCenter: parent.verticalCenter; anchors.verticalCenterOffset: row.disk ? -10 : 0; color: sidebar.theme.accent }
        ColumnLayout {
            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 3
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: row.disk ? 7 : 0; spacing: 10
                Icon { name: row.entry.icon; tint: row.current ? sidebar.theme.accentText : sidebar.theme.icon; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                Text { text: row.entry.title; color: sidebar.ink; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            Rectangle {
                visible: row.disk; Layout.fillWidth: true; Layout.leftMargin: 28; height: 3; radius: 2
                color: sidebar.theme.track
                Rectangle { height: 3; width: parent.width * (row.entry.used || 0); radius: 2; color: row.entry.used > 0.9 ? sidebar.theme.danger : sidebar.theme.meter }
            }
            Text { visible: row.disk; text: row.entry.capacity || ""; color: sidebar.muted; font.pixelSize: 11; Layout.leftMargin: 28; Layout.bottomMargin: 7; Layout.fillWidth: true; elide: Text.ElideRight }
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
            anchors.fill: parent; enabled: !!row.entry.uri
            // Either a quick-access reorder (internal ghost) or files dropped onto this place.
            property bool reorder: false
            property var urls: []
            onEntered: drag => {
                reorder = !row.disk && drag.keys.indexOf("nook-quick-access") >= 0
                urls = []
                if (!reorder && drag.hasUrls) {
                    for (let u of drag.urls) if (String(u).startsWith("file:") || String(u).startsWith("trash:")) urls.push(String(u))
                    if (!controller.canDrop(urls, row.entry.uri)) urls = []
                }
                if (!reorder && urls.length === 0) drag.accepted = false
            }
            onExited: { reorder = false; urls = [] }
            onDropped: drop => {
                if (reorder) { controller.reorderQuickAccess(drop.source.sourceUri, row.entry.uri); drop.acceptProposedAction() }
                else if (urls.length) { controller.dragDrop(urls, row.entry.uri); drop.accept(Qt.CopyAction) }
                reorder = false; urls = []
            }
        }
        NookToolTip { visible: pointer.containsMouse && !sidebar.dragGhost.dragging; text: row.entry.uri || row.entry.title; delay: 1100 }
    }
    ScrollView {
        id: quickScroll
        // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
        Component.onCompleted: contentItem.acceptedButtons = Qt.NoButton
        anchors.fill: parent
        clip: true; contentWidth: availableWidth
        ColumnLayout {
            width: quickScroll.availableWidth; spacing: 4
            Label { text: "快速访问"; color: sidebar.muted; font.pixelSize: 12; Layout.leftMargin: 12; Layout.topMargin: 6; Layout.bottomMargin: 6 }
            Repeater { model: sidebar.quickPlaces; PlaceRow { required property var modelData; entry: modelData } }
            Rectangle { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; Layout.topMargin: 15; Layout.bottomMargin: 8; height: 1; color: sidebar.theme.line }
            Label { text: "磁盘"; color: sidebar.muted; font.pixelSize: 12; Layout.leftMargin: 12; Layout.bottomMargin: 6 }
            Repeater { model: sidebar.disks; PlaceRow { required property var modelData; entry: modelData; disk: true } }
            Item { height: 12 }
        }
    }
    Rectangle {
        id: ghost
        property bool dragging: false
        property string sourceUri: ""
        property string title: ""
        visible: dragging; z: 20
        width: sidebar.width - 8; height: 38; radius: 8; opacity: 0.9
        color: sidebar.theme.raised; border.color: sidebar.theme.accent
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
