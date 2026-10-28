import QtQuick
import QtQuick.Controls
import "palette.js" as Palette

Menu {
    id: menu
    property var host: ApplicationWindow.window
    readonly property var theme: host && host.theme ? host.theme : Palette.fallback()
    readonly property bool animated: host ? host.prefs.animations : false
    objectName: "styledMenu"
    implicitWidth: 278
    padding: 7
    overlap: 3
    modal: false
    transformOrigin: Popup.TopLeft
    delegate: NookMenuItem {}
    // One shared highlight that glides between rows. It is a single scene-graph node
    // animated on the render clock, instead of per-row color fades that read as stutter.
    contentItem: ListView {
        id: rows
        implicitHeight: contentHeight
        model: menu.contentModel
        interactive: Window.window ? contentHeight + menu.topPadding + menu.bottomPadding > menu.height : false
        clip: true
        currentIndex: menu.currentIndex
        highlightFollowsCurrentItem: false
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}
        highlight: Rectangle {
            id: glide
            objectName: "menuHighlight"
            readonly property Item target: rows.currentItem
            readonly property bool shown: !!target && target.highlighted === true && target.enabled
            readonly property bool danger: !!target && target.danger === true
            width: rows.width
            y: target ? target.y : 0
            height: target ? target.height : 0
            radius: 7
            color: danger ? menu.theme.dangerSoft : menu.theme.menuHighlight
            opacity: shown ? 1 : 0
            // Slide only between visible rows; appearing from nothing snaps into place.
            Behavior on y { enabled: menu.animated && glide.opacity > 0.4; NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
            Behavior on height { enabled: menu.animated && glide.opacity > 0.4; NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
            Behavior on opacity { enabled: menu.animated; NumberAnimation { duration: 90 } }
            Behavior on color { enabled: menu.animated; ColorAnimation { duration: 120 } }
            Rectangle {
                width: 3; height: parent.height - 16; radius: 2
                anchors.left: parent.left; anchors.leftMargin: 3; anchors.verticalCenter: parent.verticalCenter
                color: glide.danger ? menu.theme.danger : menu.theme.menuBar
                opacity: 0.85
            }
        }
    }
    background: Rectangle {
        radius: 13
        color: menu.theme.raised
        border.color: menu.theme.line
        border.width: 1
        Rectangle { anchors.fill: parent; anchors.topMargin: 3; anchors.margins: -2; z: -1; radius: 15; color: menu.theme.shadow }
    }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: menu.animated ? 120 : 0; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: menu.animated ? 140 : 0; easing.type: Easing.OutCubic }
        }
    }
    // No exit fade: a still-fading menu would swallow an immediate right-click elsewhere.
}
