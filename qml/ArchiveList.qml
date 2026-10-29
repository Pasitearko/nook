import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The files of one folder inside a package (or search results from all of it), in the file
// list's look: sortable columns, the sliding lilac selection and the hover spotlight.
FocusScope {
    id: pane
    required property var model
    property var host: ApplicationWindow.window
    readonly property var theme: host.theme
    readonly property bool animated: host.animations === true
    readonly property bool searching: model.query !== ""
    readonly property real rowHeight: searching ? 54 : 40
    readonly property var columns: [
        {title: searching ? "名称 / 位置" : "名称", key: "name", width: Math.max(220, list.width - 96 - 96 - 150 - 92 - 24)},
        {title: "大小", key: "size", width: 96},
        {title: "压缩后", key: "packed", width: 96},
        {title: "修改时间", key: "date", width: 150},
        {title: "类型", key: "type", width: 92}
    ]
    signal activated(int row)
    signal contextRequested(int row, real x, real y)
    // Dragging selected rows out of the window; held tells when the button is let go.
    signal dragStarted()
    signal dragReleased()
    property bool dragHeld: false

    function focusList() { list.forceActiveFocus() }
    function move(row, modifiers) {
        if (model.count === 0) return
        const target = Math.max(0, Math.min(model.count - 1, row))
        model.click(target, modifiers & Qt.ShiftModifier ? Qt.ShiftModifier : 0)
        list.positionViewAtIndex(target, ListView.Contain)
    }

    Rectangle {
        anchors.fill: parent
        radius: 10
        color: pane.theme.pane
        border.width: 1; border.color: pane.theme.line
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 1; spacing: 0
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 34
                color: pane.theme.sunken; radius: 9
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 9; color: pane.theme.sunken }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: pane.theme.line }
                Row {
                    x: 14; height: parent.height
                    Repeater {
                        model: pane.columns
                        Item {
                            id: heading
                            required property var modelData
                            required property int index
                            width: modelData.width; height: 34
                            readonly property bool sorted: pane.model.sortKey === modelData.key
                            Text {
                                x: 7; width: parent.width - 14; anchors.verticalCenter: parent.verticalCenter
                                text: heading.modelData.title + (heading.sorted ? (pane.model.descending ? "  ↓" : "  ↑") : "")
                                color: heading.sorted ? pane.theme.inkSoft : pane.theme.muted
                                font.pixelSize: 13; elide: Text.ElideRight
                                horizontalAlignment: heading.index === 1 || heading.index === 2 ? Text.AlignRight : Text.AlignLeft
                            }
                            MouseArea {
                                objectName: "archiveSort_" + heading.modelData.key
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: pane.model.sortBy(heading.modelData.key, heading.sorted ? !pane.model.descending : false)
                            }
                            Rectangle { visible: heading.index > 0; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; width: 1; height: 14; color: pane.theme.lineStrong; opacity: 0.6 }
                        }
                    }
                }
            }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                ListView {
                    id: list
                    objectName: "archiveFileList"
                    anchors.fill: parent; anchors.margins: 5
                    focus: true
                    model: pane.model
                    reuseItems: true
                    cacheBuffer: 200
                    boundsBehavior: Flickable.StopAtBounds
                    currentIndex: pane.model.currentRow
                    highlightFollowsCurrentItem: false
                    highlight: glideComponent
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    keyNavigationEnabled: false
                    Keys.onPressed: event => {
                        const row = pane.model.currentRow
                        const page = Math.max(1, Math.floor(list.height / pane.rowHeight) - 1)
                        if (event.key === Qt.Key_Down) pane.move(row < 0 ? 0 : row + 1, event.modifiers)
                        else if (event.key === Qt.Key_Up) pane.move(row < 0 ? 0 : row - 1, event.modifiers)
                        else if (event.key === Qt.Key_PageDown) pane.move(row + page, event.modifiers)
                        else if (event.key === Qt.Key_PageUp) pane.move(row - page, event.modifiers)
                        else if (event.key === Qt.Key_Home) pane.move(0, event.modifiers)
                        else if (event.key === Qt.Key_End) pane.move(pane.model.count - 1, event.modifiers)
                        else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && row >= 0) pane.activated(row)
                        else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) pane.model.selectAll()
                        else return
                        event.accepted = true
                    }
                    delegate: rowComponent
                    MouseArea {
                        // Clicks between and below the rows clear the selection.
                        anchors.fill: parent; z: -1
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: event => { list.forceActiveFocus(); if (!(event.modifiers & Qt.ControlModifier)) pane.model.clearSelection() }
                    }
                }
                Column {
                    anchors.centerIn: parent
                    visible: pane.model.count === 0
                    spacing: 8
                    Icon { anchors.horizontalCenter: parent.horizontalCenter; name: pane.searching ? "search" : "folder"; width: 34; height: 34; opacity: 0.55 }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: pane.searching ? "压缩包里没有名称包含“" + pane.model.query + "”的项目" : "这个文件夹是空的"
                        color: pane.theme.muted; font.pixelSize: 14
                    }
                }
            }
        }
    }

    Component {
        id: glideComponent
        Rectangle {
            id: glide
            readonly property var view: parent ? parent.parent : null
            readonly property Item target: view ? view.currentItem : null
            readonly property bool shown: !!target && target.selected === true
            x: target ? target.x + 1 : 0
            y: target ? target.y + 1 : 0
            width: target ? target.width - 2 : 0
            height: target ? target.height - 2 : 0
            radius: 6
            color: pane.theme.selection
            border.width: 1; border.color: pane.theme.selectionBorder
            opacity: shown ? 1 : 0
            Behavior on y { enabled: pane.animated && glide.opacity > 0.4; NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
            Behavior on opacity { enabled: pane.animated; NumberAnimation { duration: 90 } }
            Rectangle { width: 3; height: parent.height - 14; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; color: pane.theme.selectionBar; radius: 2 }
        }
    }

    Component {
        id: rowComponent
        Item {
            id: row
            required property int index
            required property string name
            required property string location
            required property bool folder
            required property string iconName
            required property string kind
            required property string sizeText
            required property string packedText
            required property string modifiedText
            required property bool encrypted
            required property string link
            required property bool unsafe
            required property bool selected
            readonly property bool current: pane.model.currentRow === index
            objectName: "archiveEntry"
            width: list.width
            height: pane.rowHeight
            Item {
                anchors.fill: parent; anchors.margins: 1
                Rectangle {
                    anchors.fill: parent; radius: 6
                    color: pane.theme.hover
                    opacity: mouse.containsMouse && !(row.selected && !row.current) ? 1 : 0
                    Behavior on opacity { enabled: pane.animated; NumberAnimation { duration: 90 } }
                }
                Rectangle {
                    anchors.fill: parent; radius: 6
                    color: pane.theme.selection
                    border.width: 1; border.color: pane.theme.selectionBorder
                    opacity: row.selected && !row.current ? 1 : 0
                }
            }
            Spotlight {
                anchors.fill: parent; anchors.margins: 1
                active: pane.animated && mouse.containsMouse
                center: Qt.point(mouse.mouseX - 1, mouse.mouseY - 1)
                radius: 190
                strength: row.selected ? 0.3 : 0.42
                tint: pane.theme.selectionGlow
                opacity: active ? (pane.theme.dark ? 0.55 : 0.3) : 0
            }
            Row {
                x: 9; height: parent.height
                Item {
                    width: pane.columns[0].width; height: parent.height
                    Icon { x: 7; anchors.verticalCenter: parent.verticalCenter; name: row.iconName; width: 22; height: 22 }
                    Column {
                        x: 38; width: parent.width - 50; anchors.verticalCenter: parent.verticalCenter; spacing: 3
                        Row {
                            width: parent.width; spacing: 6
                            Text {
                                width: Math.min(implicitWidth, parent.width - (lock.visible ? 22 : 0))
                                text: row.name + (row.link !== "" ? " ↗" : "")
                                color: row.unsafe ? pane.theme.warning : pane.theme.ink
                                font.pixelSize: 14; elide: Text.ElideMiddle
                            }
                            Icon { id: lock; visible: row.encrypted; name: "lock"; width: 14; height: 14; anchors.verticalCenter: parent.verticalCenter; tint: pane.theme.accentText }
                        }
                        Text {
                            visible: pane.searching
                            width: parent.width
                            text: row.location
                            color: pane.theme.muted; font.pixelSize: 12; elide: Text.ElideMiddle
                        }
                    }
                }
                Text { width: pane.columns[1].width - 7; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: row.sizeText; color: pane.theme.inkSoft; font.pixelSize: 13 }
                Text { width: pane.columns[2].width - 7; height: parent.height; verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignRight; text: row.packedText; color: pane.theme.muted; font.pixelSize: 13 }
                Item { width: 14; height: 1 }
                Text { width: pane.columns[3].width; height: parent.height; verticalAlignment: Text.AlignVCenter; text: row.modifiedText; color: pane.theme.muted; font.pixelSize: 13; elide: Text.ElideRight }
                Text { width: pane.columns[4].width - 14; height: parent.height; verticalAlignment: Text.AlignVCenter; text: row.kind; color: pane.theme.muted; font.pixelSize: 13; elide: Text.ElideRight }
            }
            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: event => {
                    list.forceActiveFocus()
                    if (event.button === Qt.RightButton) {
                        if (!row.selected) pane.model.click(row.index, 0)
                        const p = mapToItem(pane, event.x, event.y)
                        pane.contextRequested(row.index, p.x, p.y)
                    } else
                        pane.model.click(row.index, event.modifiers)
                }
                onDoubleClicked: event => { if (event.button === Qt.LeftButton) pane.activated(row.index) }
                // On the MouseArea itself, so it sees the press before the MouseArea takes it.
                DragHandler {
                    target: null; acceptedButtons: Qt.LeftButton
                    enabled: !row.unsafe
                    onActiveChanged: {
                        if (active) {
                            if (!row.selected) pane.model.click(row.index, 0)
                            pane.dragHeld = true
                            pane.dragStarted()
                        } else {
                            pane.dragHeld = false
                            pane.dragReleased()
                        }
                    }
                }
            }
        }
    }
}
