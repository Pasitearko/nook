.pragma library
// Shared stand-in palette for controls that are evaluated before they are attached to a
// window (popups, dialog contents). One instance per engine, created on first use.
let shared = null
function fallback() {
    if (!shared)
        shared = Qt.createComponent("Theme.qml").createObject(null)
    return shared
}
