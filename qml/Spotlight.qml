import QtQuick

// GPU spotlight overlay (see shaders/spotlight.frag). It costs one fragment pass over its
// own small area and nothing while hidden; the software backend has no shaders, so it
// simply stays off there.
ShaderEffect {
    id: effect
    property point center: Qt.point(width / 2, height / 2)
    property real radius: 150
    property real cornerRadius: 6
    property real strength: 0.42
    property color tint: "#b49cf0"
    property bool active: false
    readonly property vector2d size: Qt.vector2d(width, height)
    property bool failed: false
    readonly property bool supported: GraphicsInfo.api !== GraphicsInfo.Software && !failed
    onStatusChanged: if (status === ShaderEffect.Error) Qt.callLater(() => { if (effect) effect.failed = true })
    visible: supported && opacity > 0.01
    opacity: active ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    blending: true
    fragmentShader: "shaders/spotlight.frag.qsb"
}
