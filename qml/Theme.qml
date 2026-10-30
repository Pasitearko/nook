import QtQuick

// One place for every colour. Warm paper neutrals with a terracotta accent (in the spirit
// of Claude's palette: warm, low-glare, never pure white or pure black); the lilac
// selection highlight is kept on purpose. Components read it as ApplicationWindow.window.theme.
QtObject {
    property bool dark: false
    property bool effects: true

    // Surfaces, back to front
    readonly property color window: dark ? (effects ? "#f21f1e1c" : "#1f1e1c") : (effects ? "#f4f4f1ea" : "#f4f1ea")
    readonly property color chrome: dark ? "#d9262522" : "#d9ece8df"     // title bar
    readonly property color statusBar: dark ? "#1b1a18" : "#ece8df"
    readonly property color pane: dark ? "#d9252421" : "#e6fcfbf8"       // file panes
    readonly property color sunken: dark ? "#2c2b28" : "#f3efe7"         // headers, filter bar, cards
    readonly property color raised: dark ? "#2b2a27" : "#fffdfa"         // menus, dialogs, popups
    readonly property color field: dark ? "#1b1a18" : "#ffffff"          // text inputs
    readonly property color tab: dark ? "#33312d" : "#fffdfa"
    readonly property color scrim: dark ? "#99100f0e" : "#552a241c"      // modal dim

    // Lines
    readonly property color line: dark ? "#3a3834" : "#e4dfd4"
    readonly property color lineStrong: dark ? "#4b4842" : "#d5cdbe"

    // Text and icons
    readonly property color ink: dark ? "#ece8df" : "#2d2924"
    readonly property color inkSoft: dark ? "#d2cdc2" : "#4d463d"
    readonly property color muted: dark ? "#a39d91" : "#82796c"
    readonly property color faint: dark ? "#6f6a62" : "#aaa194"
    readonly property color icon: dark ? "#d6d0c4" : "#4a4339"
    readonly property color iconHover: dark ? "#f6f2ea" : "#1f1b17"

    // Accent: terracotta
    readonly property color accent: dark ? "#d97757" : "#c96442"
    readonly property color accentHover: dark ? "#e48a6c" : "#b85a3b"
    readonly property color accentText: dark ? "#ec9a7c" : "#ad5235"
    readonly property color accentSoft: dark ? "#3d2b23" : "#f6e6dc"
    readonly property color accentInk: "#fffaf6"
    readonly property color focusHalo: dark ? "#40d97757" : "#33c96442"

    // Interaction overlays (warm, translucent)
    readonly property color hover: dark ? "#14fff3e0" : "#0f6b5440"
    readonly property color pressed: dark ? "#24fff3e0" : "#1c6b5440"
    readonly property color activeFill: dark ? "#2ad97757" : "#1fc96442"
    readonly property color activeBorder: dark ? "#66d97757" : "#59c96442"

    // Selection: lilac (kept)
    readonly property color selection: dark ? "#574575" : "#e2d7f3"
    readonly property color selectionBorder: dark ? "#ac91da" : "#b39bd6"
    readonly property color selectionBar: dark ? "#d9bfff" : "#9b73cb"
    readonly property color selectionText: dark ? "#e8e0f2" : "#3b2f4f"
    readonly property color selectionGlow: dark ? "#b9a3f5" : "#8f6cc4"
    readonly property color bandFill: dark ? "#33a98ee0" : "#2a9b73cb"

    // Menus (lilac-tinted rows to match the selection)
    readonly property color menuHighlight: dark ? "#3f3852" : "#efe9f8"
    readonly property color menuBar: dark ? "#c7b2f0" : "#8f6cc4"

    // Status
    readonly property color danger: dark ? "#f0857a" : "#c2453a"
    readonly property color dangerFill: dark ? "#c4483d" : "#c2453a"     // destructive buttons, under white text
    readonly property color dangerSoft: dark ? "#4a2a27" : "#fbe9e6"
    readonly property color closeHover: "#c42b1c"                         // the window ✕, as on Windows
    readonly property color warning: dark ? "#e0ad62" : "#b9822f"
    readonly property color warningSoft: dark ? "#3a3022" : "#f7eedd"
    readonly property color success: dark ? "#9cc08a" : "#5f8a4e"
    readonly property color dropFill: dark ? "#3d2b23" : "#f6e6dc"

    // Controls
    readonly property color toggleOff: dark ? "#4a4741" : "#ddd6c9"
    readonly property color knob: "#fffaf6"
    readonly property color track: dark ? "#3a3834" : "#e6e0d5"
    readonly property color meter: dark ? "#d98f66" : "#c98a62"
    readonly property color tooltip: dark ? "#3a3834" : "#2d2924"
    readonly property color tooltipText: dark ? "#ece8df" : "#f4f1ea"
    readonly property color shadow: dark ? "#40000000" : "#1f2a1d10"

    // Colour tags, muted to sit with the warm neutrals; a touch lighter on dark.
    function tagColor(id) {
        switch (id) {
        case "red": return dark ? "#e5766b" : "#cf5246"
        case "orange": return dark ? "#eb9c5c" : "#d9803a"
        case "yellow": return dark ? "#e2c25c" : "#c9a02e"
        case "green": return dark ? "#8cbd7f" : "#5c9a57"
        case "blue": return dark ? "#78a6dc" : "#4a82c3"
        case "purple": return dark ? "#b496e0" : "#8d66c0"
        default: return dark ? "#aaa397" : "#8f887c"
        }
    }
}
