import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette
import "motion.js" as Motion

AbstractButton {
    id: control
    property string symbol: ""
    property string iconName: ""
    property string hint: text
    property bool accent: false
    property bool danger: false // with accent: a destructive answer, in red
    property bool active: false
    // A HoverPlate shared by a row of buttons: it glides from button to button instead of each
    // button lighting its own background. Accent buttons keep their own.
    property Item plate: null
    readonly property bool sharesHover: !!plate && !accent
    // A toggle that flips turns its glyph in from -50 degrees and half size (300 ms, a little
    // overshoot).
    property real turn: 1
    onActiveChanged: if (animated && iconName !== "") turnIn.restart()
    NumberAnimation { id: turnIn; target: control; property: "turn"; from: 0; to: 1; duration: 300; easing.type: Easing.OutBack }
    // confirm(): the button's own answer to a click (copied, cut): its icon pops into a check
    // mark in place for a moment, instead of a toast.
    property bool confirming: false
    property real pop: 1
    function confirm() {
        confirming = true
        confirmHold.restart()
        if (animated) confirmPop.restart()
    }
    Timer { id: confirmHold; interval: 1200; onTriggered: control.confirming = false }
    NumberAnimation { id: confirmPop; target: control; property: "pop"; from: 0; to: 1; duration: 240; easing.type: Easing.OutBack }
    onHoveredChanged: if (sharesHover) plate.track(control, hovered)
    onSharesHoverChanged: if (!sharesHover && plate) plate.track(control, false)
    property var hostWindow: ApplicationWindow.window
    readonly property var theme: hostWindow && hostWindow.theme ? hostWindow.theme : Palette.fallback()
    property color foreground: theme.inkSoft
    readonly property bool animated: hostWindow ? hostWindow.animations === true : false
    // Icons read clearly at rest and respond to hover/toggle; accent buttons sit on terracotta.
    property color iconColor: accent ? theme.accentInk : active ? theme.accentText : hovered ? theme.iconHover : theme.icon
    Behavior on iconColor { enabled: control.animated; ColorAnimation { duration: 100 } }
    implicitWidth: text ? content.implicitWidth + 22 : 36
    implicitHeight: 36
    leftPadding: 10
    rightPadding: 10
    topPadding: 5
    bottomPadding: 5
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1 : 0.32
    Accessible.name: hint
    background: Rectangle {
        radius: 8
        color: control.accent && control.danger ? (control.down || control.hovered ? Qt.darker(control.theme.dangerFill, 1.12) : control.theme.dangerFill)
             : control.accent ? (control.down ? control.theme.accentHover : control.hovered ? control.theme.accentHover : control.theme.accent)
             : control.down ? control.theme.pressed : control.active ? control.theme.activeFill : control.hovered && !control.sharesHover ? control.theme.hover : "transparent"
        border.width: control.active && !control.accent ? 1 : 0
        border.color: control.theme.activeBorder
        Behavior on color { enabled: control.animated; ColorAnimation { duration: 100 } }
    }
    contentItem: RowLayout {
        id: content
        spacing: 7
        Icon {
            visible: control.iconName !== ""
            name: control.confirming ? "verify" : control.iconName
            tint: control.confirming && !control.accent ? control.theme.success : control.iconColor
            rotation: -50 * (1 - control.turn)
            scale: (0.5 + 0.5 * control.turn) * (control.confirming ? 0.6 + 0.4 * control.pop : 1)
            Layout.preferredWidth: 18; Layout.preferredHeight: 18
        }
        Text { visible: control.symbol !== ""; text: control.symbol; color: control.accent ? control.theme.accentInk : control.foreground; font.pixelSize: 19; Layout.alignment: Qt.AlignVCenter }
        Text { visible: control.text !== ""; text: control.text; color: control.accent ? control.theme.accentInk : control.active ? control.theme.accentText : control.foreground; font.pixelSize: 14; font.weight: control.accent ? Font.DemiBold : Font.Normal; Layout.alignment: Qt.AlignVCenter }
    }
    NookToolTip { visible: control.hovered && control.hint !== ""; text: control.hint }
}
