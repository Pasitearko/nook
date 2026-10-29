import QtQuick
import "motion.js" as Motion

// One hover plate shared by a group of items (toolbar buttons, sidebar rows, file rows) that
// glides from item to item under the pointer, instead of each item fading its own. Coming from
// nothing it appears in place; left, it fades out where it was, and a pointer that comes back
// mid-fade glides on from there. Declare it before the items so it is drawn beneath them.
Rectangle {
    id: plate
    property Item hot: null       // the item under the pointer, or null
    property var hotKey: hot      // its identity; a row index where delegates are recycled
    property bool animated: true
    property real inset: 0
    function track(item, on) {
        if (on) hot = item
        else if (hot === item) hot = null
    }
    // hot's rectangle in this plate's parent; the arguments name what the mapping depends on.
    readonly property rect hotRect: hot ? place(hot.x, hot.y, hot.width, hot.height, hot.parent ? hot.parent.x + hot.parent.y : 0) : Qt.rect(0, 0, 0, 0)
    function place() {
        const p = hot.mapToItem(parent, 0, 0)
        return Qt.rect(p.x + inset, p.y + inset, hot.width - 2 * inset, hot.height - 2 * inset)
    }
    // While fading out, the plate keeps the last item it was on. (Copied in handlers: a
    // Binding with `when: hot` would still take the empty rect as hot turns null.)
    property rect heldRect
    property var heldKey
    onHotRectChanged: if (hot) heldRect = hotRect
    onHotKeyChanged: if (hot) heldKey = hotKey
    Glide {
        id: glide
        animated: plate.animated
        shown: plate.opacity > 0
        key: plate.heldKey
        targetX: plate.heldRect.x; targetY: plate.heldRect.y
        targetWidth: plate.heldRect.width; targetHeight: plate.heldRect.height
    }
    x: glide.x; y: glide.y; width: glide.width; height: glide.height
    radius: 8
    opacity: hot ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { enabled: plate.animated; NumberAnimation { duration: plate.hot ? Motion.plateFadeInMs : Motion.plateFadeOutMs } }
}
