import QtQuick
import QtQuick.Controls

// Chinese, themed replacement for the stock text context menu; opened by TextMenuArea.
// Works for any TextInput/TextEdit, including TextField and TextArea. The selection is
// captured when the menu opens: opening a menu takes focus, which clears a selection.
NookMenu {
    id: menu
    required property Item target
    readonly property bool editable: !target.readOnly
    property int start: 0
    property int end: 0
    readonly property bool hasSelection: end > start
    objectName: "textContextMenu"
    parent: target
    implicitWidth: 236
    onAboutToShow: { start = target.selectionStart; end = target.selectionEnd }
    function restore() { target.forceActiveFocus(); target.select(start, end) }

    component Separator: MenuSeparator {
        height: visible ? implicitHeight : 0
        contentItem: Rectangle { implicitHeight: 1; color: menu.theme.line; opacity: 0.7 }
    }

    NookMenuItem { text: "撤销"; iconName: "undo"; shortcutText: "Ctrl Z"; visible: menu.editable; enabled: menu.target.canUndo; onTriggered: { menu.target.forceActiveFocus(); menu.target.undo() } }
    NookMenuItem { text: "重做"; iconName: "redo"; shortcutText: "Ctrl Shift Z"; visible: menu.editable; enabled: menu.target.canRedo; onTriggered: { menu.target.forceActiveFocus(); menu.target.redo() } }
    Separator { visible: menu.editable }
    NookMenuItem { text: "剪切"; iconName: "cut"; shortcutText: "Ctrl X"; visible: menu.editable; enabled: menu.hasSelection; onTriggered: { menu.restore(); menu.target.cut() } }
    NookMenuItem { objectName: "textMenuCopy"; text: "复制"; iconName: "copy"; shortcutText: "Ctrl C"; enabled: menu.hasSelection; onTriggered: { menu.restore(); menu.target.copy() } }
    NookMenuItem { text: "粘贴"; iconName: "paste"; shortcutText: "Ctrl V"; visible: menu.editable; enabled: menu.target.canPaste; onTriggered: { menu.restore(); menu.target.paste() } }
    NookMenuItem { text: "删除"; iconName: "trash"; visible: menu.editable; enabled: menu.hasSelection; onTriggered: { menu.restore(); menu.target.remove(menu.start, menu.end) } }
    Separator {}
    NookMenuItem { objectName: "textMenuSelectAll"; text: "全选"; iconName: "select-all"; shortcutText: "Ctrl A"; enabled: menu.target.length > 0; onTriggered: { menu.target.forceActiveFocus(); menu.target.selectAll() } }
}
