import QtQuick
import "motion.js" as Motion

// A rectangle that follows its target on a damped spring (Pulse's SpringRectMotion). It is
// drawn at target + offset and only the offset springs back to zero, so:
// - when the key (what is highlighted) changes, the offset first keeps the plate where it was
//   drawn and then decays, carrying its velocity: a pointer sweeping over rows keeps the plate
//   moving instead of restarting it from rest at every row;
// - when the same item moves (scroll, relayout, a displaced row) the plate moves with it;
// - hidden, it snaps: it reappears in place instead of flying in from a stale position.
// The key and the target usually change together, in either order; changes are gathered per
// event so the glide always starts from where the plate was last drawn.
QtObject {
    id: glide
    property bool animated: true
    property bool shown: true
    property var key
    property real targetX: 0
    property real targetY: 0
    property real targetWidth: 0
    property real targetHeight: 0
    property real stiffness: Motion.plate.k
    property real damping: Motion.plate.c
    readonly property real x: targetX + offX
    readonly property real y: targetY + offY
    readonly property real width: targetWidth + offWidth
    readonly property real height: targetHeight + offHeight
    readonly property bool moving: frames.running

    property real offX: 0
    property real offY: 0
    property real offWidth: 0
    property real offHeight: 0
    property var velocity: [0, 0, 0, 0]
    property var last: [0, 0, 0, 0]  // the targets as last seen
    property var from: null          // where the plate was drawn when this event's changes began
    property var lastKey
    property bool keyMoved: false
    property bool fresh: false
    property real elapsed: 0
    property FrameAnimation frames: FrameAnimation { onTriggered: glide.step(frameTime) }

    function begin() {
        if (from) return
        from = [last[0] + offX, last[1] + offY, last[2] + offWidth, last[3] + offHeight]
        Qt.callLater(() => { glide.from = null; glide.keyMoved = false; glide.fresh = false })
    }
    function target(i, value) {
        begin()
        const d = value - last[i]
        last[i] = value
        // A glide that started in this event holds its start; otherwise the plate rides along.
        if (keyMoved) offset(i, offsets()[i] - d)
    }
    function offsets() { return [offX, offY, offWidth, offHeight] }
    function offset(i, value) {
        if (i === 0) offX = value
        else if (i === 1) offY = value
        else if (i === 2) offWidth = value
        else offHeight = value
    }
    function snap() {
        frames.running = false
        offX = offY = offWidth = offHeight = 0
        velocity = [0, 0, 0, 0]
    }
    function step(dt) {
        let remaining = Math.min(dt, 0.064)
        elapsed += remaining
        const o = offsets(), v = velocity
        while (remaining > 0) {
            const h = Math.min(remaining, 0.004)
            for (let i = 0; i < 4; ++i) {
                v[i] += (-stiffness * o[i] - damping * v[i]) * h
                o[i] += v[i] * h
            }
            remaining -= h
        }
        // At rest within a quarter pixel, or bounded so a glide can never keep ticking.
        if (elapsed >= 1.2 || o.every((value, i) => Math.abs(value) < 0.25 && Math.abs(v[i]) < 8)) { snap(); return }
        velocity = v
        offX = o[0]; offY = o[1]; offWidth = o[2]; offHeight = o[3]
    }

    onTargetXChanged: target(0, targetX)
    onTargetYChanged: target(1, targetY)
    onTargetWidthChanged: target(2, targetWidth)
    onTargetHeightChanged: target(3, targetHeight)
    onShownChanged: { begin(); fresh = true; snap() }
    onAnimatedChanged: snap()
    onKeyChanged: {
        if (key === lastKey) return
        begin()
        lastKey = key
        if (!animated || !shown || fresh) { snap(); return }
        keyMoved = true
        for (let i = 0; i < 4; ++i) offset(i, from[i] - last[i])
        elapsed = 0
        frames.running = true
    }
    Component.onCompleted: { last = [targetX, targetY, targetWidth, targetHeight]; lastKey = key }
}
