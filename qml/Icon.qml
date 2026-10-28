import QtQuick
import QtQuick.Controls
import "palette.js" as Palette
Item {
    id: icon
    property string name: "file"
    // Monochrome artwork is recoloured on the GPU (shaders/tint.frag) with a per-theme
    // default; full-colour icons (folder, image, code, video, music) keep their own colours.
    readonly property var host: ApplicationWindow.window
    property color tint: (host && host.theme ? host.theme : Palette.fallback()).icon
    readonly property bool monochrome: ["add", "back", "calendar", "close", "copy", "cut", "document", "download", "drive", "file",
        "filter", "forward", "grid", "hidden", "home", "list", "moon", "paste", "redo", "refresh", "rename", "search", "select-all",
        "settings", "split", "sun", "terminal", "trash", "undo", "up"].indexOf(name) >= 0
    property bool shaderFailed: false
    readonly property bool tinted: monochrome && !shaderFailed && GraphicsInfo.api !== GraphicsInfo.Software
    // The layout never depends on SVG raster size. Fractional DPI must not
    // feed sourceSize back into the control's implicit dimensions.
    implicitWidth: 20
    implicitHeight: 20
    width: 20
    height: 20
    Image {
        anchors.fill: parent
        id: artwork
        source: "icons/" + (icon.name || "file") + ".svg"
        onStatusChanged: if (status === Image.Error && source.toString().indexOf("/file.svg") < 0) source = Qt.resolvedUrl("icons/file.svg")
        sourceSize.width: Math.max(1, Math.round(icon.width * Screen.devicePixelRatio))
        sourceSize.height: Math.max(1, Math.round(icon.height * Screen.devicePixelRatio))
        fillMode: Image.PreserveAspectFit
        smooth: true
        // Rendered once into a small texture (only when the icon changes), then recoloured.
        layer.enabled: icon.tinted
        layer.effect: ShaderEffect {
            property color tint: icon.tint
            fragmentShader: "shaders/tint.frag.qsb"
            // Never let a missing/failed shader blank the icon: fall back to the artwork.
            onStatusChanged: if (status === ShaderEffect.Error) Qt.callLater(() => { if (icon) icon.shaderFailed = true })
        }
    }
}
