import QtQuick
import QtQuick.Controls

// Right-click handling for a text control: declare it inside the control together with
// `ContextMenu.menu: null` (which turns off Qt's untranslated stock menu). Qt's ContextMenu
// reports the click relative to whichever child was hit, so inside scrolled, scaled or
// overlaid fields its menu opened far from the pointer; here the press position is exact.
Item {
    id: area
    required property Item target
    signal aboutToOpen()
    readonly property alias menu: menu
    anchors.fill: parent
    TapHandler {
        acceptedButtons: Qt.RightButton
        onPressedChanged: {
            if (!pressed) return
            area.aboutToOpen()
            const p = area.mapToItem(area.target, point.position.x, point.position.y)
            menu.popup(area.target, p.x, p.y)
        }
    }
    TextMenu { id: menu; target: area.target }
}
