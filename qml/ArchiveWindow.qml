import QtQuick
import QtQuick.Controls
import QtQuick.Window

// The frame every nook解压缩 window shares: nook's appearance (followed live), the scaled
// workspace, the rounded surface and a toast. What the shared controls read from their window
// (theme, animations, layoutWidth) is defined here.
ApplicationWindow {
    id: window
    required property var app
    readonly property var prefs: app.settings.values
    readonly property bool animations: prefs.animations === true
    readonly property bool dark: prefs.theme === "dark" || (prefs.theme === "system" && app.settings.systemDark)
    readonly property Theme theme: Theme { dark: window.dark; effects: window.prefs.effects }
    readonly property color ink: theme.ink
    readonly property color muted: theme.muted
    readonly property color line: theme.line
    readonly property real uiScale: Math.max(0.75, Math.min(1.5, Number(prefs.uiScale) || 1))
    readonly property real layoutWidth: width / uiScale
    readonly property real layoutHeight: height / uiScale
    readonly property bool editingText: activeFocusItem instanceof TextInput || activeFocusItem instanceof TextEdit
    property string toastMessage: ""
    default property alias content: surface.data
    property alias workspace: workspace

    function toast(message) { toastMessage = message; toastTimer.restart() }

    visible: true
    color: "transparent"
    palette.window: theme.raised
    palette.base: theme.field
    palette.text: ink
    palette.windowText: ink
    palette.buttonText: ink
    palette.button: theme.raised
    palette.highlight: theme.selection
    palette.highlightedText: ink
    palette.mid: line
    palette.placeholderText: muted

    Timer { id: toastTimer; interval: 3500; onTriggered: window.toastMessage = "" }
    Binding { target: window.Overlay.overlay; property: "scale"; value: window.uiScale }
    Binding { target: window.Overlay.overlay; property: "width"; value: window.layoutWidth }
    Binding { target: window.Overlay.overlay; property: "height"; value: window.layoutHeight }
    Binding { target: window.Overlay.overlay; property: "transformOrigin"; value: Item.TopLeft }

    Item {
        id: workspace
        width: window.layoutWidth; height: window.layoutHeight
        scale: window.uiScale; transformOrigin: Item.TopLeft
        Rectangle {
            id: surface
            anchors.fill: parent
            radius: 12
            color: window.theme.window
            border.width: 1; border.color: window.line
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 56
            visible: window.toastMessage !== ""; z: 100
            width: Math.min(window.layoutWidth - 70, toastLabel.implicitWidth + 36); height: toastLabel.implicitHeight + 22; radius: 9
            color: window.theme.raised; border.color: window.theme.lineStrong
            Text { id: toastLabel; objectName: "archiveToast"; anchors.centerIn: parent; text: window.toastMessage; color: window.ink; font.pixelSize: 14; width: Math.min(implicitWidth, window.layoutWidth - 106); wrapMode: Text.Wrap }
        }
    }
}
