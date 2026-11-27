import QtQuick
import QtQuick.Controls
import QtQml.Models
import "palette.js" as Palette
import "motion.js" as Motion

ComboBox {
    id: control
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    readonly property color ink: theme.ink
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    implicitWidth: 120
    implicitHeight: 34
    leftPadding: 12
    rightPadding: 29
    font.pixelSize: 13
    focusPolicy: Qt.TabFocus
    contentItem: Text {
        text: control.displayText; color: control.ink
        font: control.font
        verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    indicator: Text {
        text: "›"; rotation: control.popup.visible ? -90 : 90
        color: control.theme.muted
        font.pixelSize: 17
        x: control.width - width - 12; anchors.verticalCenter: parent.verticalCenter
        Behavior on rotation { enabled: control.animated; NumberAnimation { duration: Motion.snappy.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.snappy.curve } }
    }
    // Same surface as the address, search and filter fields.
    background: Rectangle {
        radius: 8
        color: control.down ? control.theme.sunken : control.theme.field
        border.width: 1
        border.color: control.activeFocus || control.popup.visible ? control.theme.accent : control.hovered ? control.theme.lineStrong : control.theme.line
    }
    popup: Popup {
        id: drop
        // Place the card in the (possibly UI-scaled) overlay's coordinates, not
        // below the control unconditionally: chooser controls sit at the bottom.
        parent: control.Overlay.overlay
        margins: 8
        property point anchor: Qt.point(0, 0)
        readonly property real above: Math.max(0, anchor.y - margins - 5)
        readonly property real below: Math.max(0, (parent ? parent.height : 0) - margins - anchor.y - control.height - 5)
        readonly property bool opensAbove: implicitHeight > below && above > below
        function place() {
            if (parent) anchor = control.mapToItem(parent, 0, 0)
        }
        function schedulePlacement() { if (visible) Qt.callLater(place) }
        onAboutToShow: place()
        onOpened: rows.positionViewAtIndex(control.currentIndex, ListView.Contain)
        x: Math.max(margins, Math.min(anchor.x, (parent ? parent.width : control.width) - margins - width))
        y: Math.max(margins, Math.min(opensAbove ? anchor.y - 5 - height : anchor.y + control.height + 5,
                                     (parent ? parent.height : 0) - margins - height))
        height: Math.min(implicitHeight, opensAbove ? above : below,
                         Math.max(0, (parent ? parent.height : 0) - margins * 2))
        transformOrigin: opensAbove ? Popup.Bottom : Popup.Top
        Connections {
            target: drop.parent
            function onWidthChanged() { drop.schedulePlacement() }
            function onHeightChanged() { drop.schedulePlacement() }
        }
        // A footer/layout or scrolling ancestor can move without changing the
        // control's local x/y. Observe the chain only while open; no idle polling.
        Instantiator {
            model: {
                let ancestors = []
                if (drop.visible)
                    for (let item = control; item; item = item.parent) ancestors.push(item)
                return ancestors
            }
            delegate: Connections {
                required property var modelData
                target: modelData
                function onXChanged() { drop.schedulePlacement() }
                function onYChanged() { drop.schedulePlacement() }
                function onWidthChanged() { drop.schedulePlacement() }
                function onHeightChanged() { drop.schedulePlacement() }
                function onScaleChanged() { drop.schedulePlacement() }
                function onRotationChanged() { drop.schedulePlacement() }
                function onChildrenRectChanged() { drop.schedulePlacement() }
            }
        }
        // Animate the surface, not y/height: geometry animations would override
        // the edge-aware placement and expose clipped rows during opening.
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: control.animated ? Motion.open.ms : 0; easing.type: Easing.OutCubic }
                NumberAnimation { property: "opacity"; from: 0.4; to: 1; duration: control.animated ? 90 : 0 }
                SequentialAnimation {
                    PropertyAction { target: drop.contentItem; property: "opacity"; value: control.animated ? 0 : 1 }
                    PauseAnimation { duration: control.animated ? 110 : 0 }
                    NumberAnimation { target: drop.contentItem; property: "opacity"; to: 1; duration: control.animated ? 120 : 0; easing.type: Easing.OutCubic }
                }
            }
        }
        clip: true
        width: Math.min(control.width, Math.max(0, (parent ? parent.width : control.width + 16) - margins * 2))
        padding: 5
        // Delegates have a fixed height; do not depend on a not-yet-populated
        // ListView's contentHeight when deciding which side has enough room.
        implicitHeight: control.count * 34 + topPadding + bottomPadding
        background: Rectangle { radius: 10; color: control.theme.raised; border.color: control.theme.line }
        contentItem: ListView {
            id: rows
            clip: true; implicitHeight: contentHeight
            boundsBehavior: Flickable.StopAtBounds
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
    delegate: ItemDelegate {
        required property int index
        required property var modelData
        width: drop.availableWidth; height: 34
        highlighted: control.highlightedIndex === index
        contentItem: Text { text: modelData; color: control.ink; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: 6; color: parent.highlighted ? control.theme.menuHighlight : "transparent" }
    }
}
