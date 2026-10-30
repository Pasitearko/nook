import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "motion.js" as Motion

// Beside the files in a maximized window (as in Pulse): a preview of the selected item, what
// it is, the three things most often done with it, its facts and its colour tags. With nothing
// selected it describes the folder shown; with several items, all of them together.
Rectangle {
    id: details
    required property var controller
    required property var host
    // The list the selection is read from (the search results while searching).
    required property var model
    readonly property var theme: host.theme
    readonly property color ink: host.ink
    readonly property color muted: host.muted
    readonly property bool animated: host.animations === true
    readonly property var tagStore: controller.tags
    // What is described: entries as DirectoryModel.selectedEntries() gives them.
    property var subjects: []
    readonly property var subject: subjects.length === 1 ? subjects[0] : null
    readonly property var uris: subjects.map(e => e.fileUri)
    readonly property bool folderShown: subjects.length === 1 && model && model.selectedCount === 0
    readonly property var facts: {
        const f = controller.facts
        return f && JSON.stringify(f.uris) === JSON.stringify(uris) ? f : ({})
    }
    signal hideRequested()
    color: theme.pane
    radius: 10
    border.width: 1; border.color: theme.line
    clip: true

    function refresh() {
        if (!visible || !model) return
        let entries = model.selectedEntries()
        // Nothing selected: the folder itself, when it is a real one.
        if (!entries.length && model.uri && String(model.uri).startsWith("file:") && !model.tagView && !host.searching)
            entries = [{fileUri: model.uri, fileName: model.title, folder: true, iconName: "folder", kind: "文件夹", fullPath: model.displayPath}]
        subjects = entries
        if (entries.length === 1 && !entries[0].folder) controller.glance.open(entries[0].fileUri)
        else controller.glance.close()
        if (entries.length) controller.describe(entries.map(e => e.fileUri))
    }
    onVisibleChanged: if (visible) settle.restart(); else controller.glance.close()
    onModelChanged: settle.restart()
    Connections {
        target: details.model
        function onSelectionChanged() { settle.restart() }
        function onLocationChanged() { settle.restart() }
        function onCountChanged() { if (details.model.selectedCount === 0) settle.restart() }
    }
    // Following a selection that sweeps across rows only describes where it stops.
    Timer { id: settle; interval: 140; onTriggered: details.refresh() }

    component Section: ColumnLayout {
        property string title: ""
        default property alias body: inner.data
        Layout.fillWidth: true; spacing: 8
        Text { text: parent.title; color: details.muted; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.leftMargin: 2 }
        ColumnLayout { id: inner; Layout.fillWidth: true; spacing: 6 }
    }
    component Fact: RowLayout {
        property string label: ""
        property string value: ""
        visible: value !== ""
        Layout.fillWidth: true; spacing: 10
        Text { text: parent.label; color: details.muted; font.pixelSize: 13; Layout.preferredWidth: 64; Layout.alignment: Qt.AlignTop }
        TextEdit {
            text: parent.value; color: details.ink; font.pixelSize: 13
            readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
            selectionColor: details.theme.selection; selectedTextColor: details.theme.selectionText
            Layout.fillWidth: true
        }
    }
    // One of the three actions: an icon over its name. The first is the accent one while it can
    // be used; unavailable, every action looks the same, quietly greyed. Hover lays a tint over
    // the button's own colour (fading in and out) rather than replacing it.
    component Action: AbstractButton {
        id: action
        property string iconName: ""
        property bool accent: false
        readonly property bool lit: accent && enabled
        Layout.fillWidth: true; Layout.preferredWidth: 1
        implicitHeight: 58
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        background: Rectangle {
            radius: 9
            color: action.lit ? details.theme.accent : details.theme.sunken
            border.width: action.lit ? 0 : 1; border.color: details.theme.line
            Rectangle {
                anchors.fill: parent; radius: parent.radius
                color: action.lit ? details.theme.accentHover : details.theme.pressed
                opacity: !action.enabled ? 0 : action.down ? 1 : action.hovered ? (action.lit ? 1 : 0.55) : 0
                Behavior on opacity { enabled: details.animated; NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
            }
        }
        contentItem: Column {
            spacing: 5; topPadding: 9
            opacity: action.enabled ? 1 : 0.45
            Icon { anchors.horizontalCenter: parent.horizontalCenter; name: action.iconName; width: 18; height: 18; tint: action.lit ? details.theme.accentInk : details.theme.icon }
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: action.text; color: action.lit ? details.theme.accentInk : details.ink; font.pixelSize: 12; elide: Text.ElideRight }
        }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 1; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.preferredHeight: 45; Layout.leftMargin: 15; Layout.rightMargin: 8
            Icon { name: "details"; width: 16; height: 16 }
            Text { text: "详细信息"; color: details.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true }
            ActionButton { objectName: "hideDetailsButton"; iconName: "close"; hint: "隐藏详细信息窗格"; foreground: details.muted; implicitWidth: 30; implicitHeight: 30; leftPadding: 7; rightPadding: 7; focusPolicy: Qt.NoFocus; onClicked: details.hideRequested() }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: details.theme.line; opacity: 0.7 }
        Text {
            visible: details.subjects.length === 0
            Layout.fillWidth: true; Layout.fillHeight: true
            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            text: "选择文件或文件夹\n查看预览和详细信息"; color: details.muted; font.pixelSize: 13; lineHeight: 1.3
        }
        ScrollView {
            id: scroll
            objectName: "detailsScroll"
            visible: details.subjects.length > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            Component.onCompleted: { contentItem.boundsBehavior = Flickable.StopAtBounds }
            ScrollBar.vertical: NookScrollBar { parent: scroll; x: scroll.width - width; height: scroll.availableHeight }
            ColumnLayout {
                id: column
                width: scroll.availableWidth
                spacing: 16
                // The preview: the picture, the opening lines of a text, or the item's icon.
                Rectangle {
                    id: stage
                    objectName: "detailsPreview"
                    Layout.fillWidth: true; Layout.topMargin: 14; Layout.leftMargin: 14; Layout.rightMargin: 14
                    Layout.preferredHeight: 196
                    radius: 10
                    color: details.theme.sunken
                    border.width: 1; border.color: details.theme.line
                    clip: true
                    readonly property var content: details.subject && !details.subject.folder && details.controller.glance.uri === details.subject.fileUri ? details.controller.glance.content : ({})
                    readonly property bool loading: details.controller.glance.loading && !!details.subject && !details.subject.folder
                    readonly property bool picture: content.mode === "image" && !content.error
                    readonly property bool writing: (content.mode === "text" || content.mode === "rich") && !content.error && !!content.text
                    Image {
                        objectName: "detailsImage"
                        visible: stage.picture
                        anchors.fill: parent; anchors.margins: 10
                        source: stage.picture ? stage.content.image : ""
                        fillMode: Image.PreserveAspectFit; asynchronous: true; smooth: true; mipmap: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { enabled: details.animated; NumberAnimation { duration: 160 } }
                    }
                    Text {
                        objectName: "detailsText"
                        visible: stage.writing
                        anchors.fill: parent; anchors.margins: 12
                        text: stage.writing ? String(stage.content.text).slice(0, 1600) : ""
                        textFormat: stage.content.mode === "rich" ? Text.RichText : Text.PlainText
                        color: details.theme.inkSoft; font.pixelSize: 11; font.family: stage.content.mode === "text" ? "monospace" : ""
                        wrapMode: Text.WrapAnywhere; clip: true
                    }
                    // The text runs on below the card; fade its last lines out.
                    Rectangle {
                        visible: stage.writing
                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 1
                        height: 36; radius: 9
                        gradient: Gradient {
                            GradientStop { position: 0; color: "transparent" }
                            GradientStop { position: 1; color: details.theme.sunken }
                        }
                    }
                    // Folders, several items, and files without a picture or text: the icon.
                    Item {
                        visible: !stage.picture && !stage.writing && !stage.loading
                        anchors.centerIn: parent
                        width: 96; height: 96
                        Repeater {
                            model: details.subjects.length > 1 ? Math.min(3, details.subjects.length) : 1
                            Rectangle {
                                required property int index
                                visible: details.subjects.length > 1
                                width: 74; height: 74; radius: 14
                                x: 11 + (index - 1) * 9; y: 11 - (index - 1) * 7
                                rotation: (index - 1) * 5
                                color: details.theme.raised; border.color: details.theme.line
                                z: -index
                            }
                        }
                        Icon {
                            anchors.centerIn: parent
                            name: details.subjects.length > 1 ? "file" : details.subject ? details.subject.iconName || "file" : "file"
                            width: details.subjects.length > 1 ? 40 : 76; height: width
                        }
                    }
                    Text {
                        visible: !!details.subject && !details.subject.folder && !stage.picture && !stage.writing && !stage.loading
                        anchors.bottom: parent.bottom; anchors.bottomMargin: 10; anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                        text: stage.content.error || "此格式没有内容预览"; color: details.muted; font.pixelSize: 11
                    }
                    BusyIndicator { anchors.centerIn: parent; running: stage.loading; visible: running; width: 30; height: 30 }
                }
                // Name and what it is.
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16; spacing: 3
                    Text {
                        objectName: "detailsName"
                        Layout.fillWidth: true
                        text: details.subjects.length > 1 ? "已选择 " + details.subjects.length + " 项" : details.subject ? details.subject.fileName : ""
                        color: details.ink; font.pixelSize: 17; font.weight: Font.DemiBold
                        wrapMode: Text.WrapAnywhere; maximumLineCount: 2; elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: {
                            const f = details.facts
                            if (details.subjects.length > 1)
                                return [f.files ? f.files + " 个文件" : "", f.folders ? f.folders + " 个文件夹" : ""].filter(s => s).join("，")
                            if (!details.subject) return ""
                            const kind = details.subject.folder ? "文件夹" : (details.subject.kind ? details.subject.kind + " 文件" : "文件")
                            return details.subject.folder ? kind + (f.children !== undefined ? " · " + f.children + " 项" : "") : kind + " · " + details.subject.sizeText
                        }
                        color: details.muted; font.pixelSize: 13; elide: Text.ElideRight
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; spacing: 8
                    Action {
                        objectName: "detailsOpen"
                        text: "打开"; iconName: details.subject && details.subject.folder ? "folder" : "open"; accent: true
                        enabled: details.subjects.length === 1 && !details.folderShown
                        onClicked: details.host.activate(details.subject)
                    }
                    Action {
                        objectName: "detailsNewTab"
                        text: "新标签页打开"; iconName: "newtab"
                        enabled: details.subjects.length === 1
                        onClicked: details.controller.openInNewTab(details.subject.fileUri)
                    }
                    Action {
                        objectName: "detailsCopyPath"
                        text: "复制路径"; iconName: "copy"
                        onClicked: { details.controller.copyPaths(details.uris); details.host.toast(details.uris.length > 1 ? "已复制 " + details.uris.length + " 个路径" : "已复制路径") }
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; height: 1; color: details.theme.line }
                Section {
                    title: "基本信息"
                    Layout.leftMargin: 16; Layout.rightMargin: 14
                    Fact { label: "位置"; value: details.subjects.length === 1 ? (details.facts.location || "") : "" }
                    Fact { label: "大小"; value: details.facts.busy ? "正在计算…" : (details.facts.sizeText || "") }
                    Fact { label: "包含"; value: details.facts.contents || "" }
                    Fact { label: "创建时间"; value: details.facts.created || "" }
                    Fact { label: "修改时间"; value: details.facts.modified || "" }
                    Fact { label: "访问时间"; value: details.facts.accessed || "" }
                }
                Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; height: 1; color: details.theme.line }
                // Colour tags: a chip per colour, filled when the item carries it (half when only
                // some of the selected items do). A click puts it on or takes it off.
                Section {
                    title: "标签"
                    Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.bottomMargin: 16
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: details.tagStore.tags
                            AbstractButton {
                                id: chip
                                required property var modelData
                                objectName: "detailsTag_" + modelData.id
                                readonly property int state: (details.tagStore.tags, details.tagStore.state(details.uris, modelData.id))
                                readonly property color tone: details.theme.tagColor(modelData.id)
                                hoverEnabled: true
                                focusPolicy: Qt.TabFocus
                                implicitHeight: 28; implicitWidth: chipRow.implicitWidth + 22
                                onClicked: details.tagStore.toggle(details.uris, modelData.id)
                                background: Rectangle {
                                    radius: 14
                                    color: chip.state === 2 ? Qt.alpha(chip.tone, details.host.dark ? 0.28 : 0.16)
                                         : chip.hovered ? details.theme.hover : "transparent"
                                    border.width: 1
                                    border.color: chip.state > 0 ? Qt.alpha(chip.tone, 0.75) : details.theme.line
                                    Behavior on color { enabled: details.animated; ColorAnimation { duration: 120 } }
                                }
                                contentItem: Row {
                                    id: chipRow
                                    spacing: 6; leftPadding: 11
                                    Rectangle {
                                        width: 8; height: 8; radius: 4; anchors.verticalCenter: parent.verticalCenter
                                        color: chip.tone
                                        // Some of the selection: a ring.
                                        border.width: chip.state === 1 ? 2 : 0; border.color: details.theme.pane
                                    }
                                    Text { text: chip.modelData.name; color: chip.state > 0 ? details.ink : details.theme.inkSoft; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                                    Text { visible: chip.state === 2; text: "✓"; color: chip.tone; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                                }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true; Layout.topMargin: 2
                        text: "Alt+1 至 Alt+7 加上或去掉标签，Alt+0 全部去掉；在侧栏右键标签可改名"
                        color: details.theme.faint; font.pixelSize: 11; wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
