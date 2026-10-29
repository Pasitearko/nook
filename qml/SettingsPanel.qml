import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "motion.js" as Motion

// Preferences, laid out like a system settings app: pages on the left; on the right a page
// title and summary, then groups of rows. A row names the setting, says in one line what it
// does, and keeps its control at the right edge; rows of a group share one card with hairline
// dividers. Every change applies at once.
Dialog {
    id: panel
    required property var controller
    required property var host
    readonly property var prefs: controller.settings.values
    readonly property bool dark: host.dark
    readonly property var theme: host.theme
    readonly property color ink: theme.ink
    readonly property color muted: theme.muted
    readonly property color line: theme.line
    readonly property bool animated: prefs.animations === true
    property int category: 0
    onCategoryChanged: {
        if (scroll.contentItem) scroll.contentItem.contentY = 0
        if (animated) pageIn.restart()
    }
    property var mountChanges: ({})
    readonly property var pendingMounts: Object.values(mountChanges)
    function changeMount(disk, enabled, readOnly) {
        const changes = Object.assign({}, mountChanges)
        if (enabled === disk.autoMount && (!enabled || readOnly === disk.readOnly))
            delete changes[disk.uuid]
        else
            changes[disk.uuid] = {uuid: disk.uuid, enabled: enabled, readOnly: readOnly}
        mountChanges = changes
    }
    signal reindexRequested()
    onOpened: {
        rootsInput.text = prefs.roots.join("\n"); excludeInput.text = prefs.exclusions.join("\n")
        mountChanges = ({}); controller.diskMounts.refresh()
        controller.checkInstalledVersion()
    }
    Connections { target: panel.controller.diskMounts; function onDisksChanged() { panel.mountChanges = ({}) } }
    // The pages, in the order the navigation lists them; `page` is the category number.
    readonly property var pages: [
        {page: 0, title: "外观与主题", icon: "sun", summary: "主题、透明效果、动效和界面大小"},
        {page: 1, title: "浏览与交互", icon: "list", summary: "文件列表、预览、快速访问和撤销"},
        {page: 2, title: "搜索与索引", icon: "search", summary: "文件名索引的状态、范围和 Windows 分区"},
        {page: 4, title: "磁盘与挂载", icon: "drive", summary: "数据分区的开机自动挂载"},
        {page: 3, title: "关于应用", icon: "info", summary: "版本、更新与组成"}
    ]
    readonly property var current: pages.find(p => p.page === category) || pages[0]
    NookDialog {
        id: mountConfirm
        objectName: "mountConfirmDialog"
        width: Math.min(560, panel.width - 32)
        iconName: "drive"
        title: "确认开机挂载更改"
        message: "将通过系统授权更新 /etc/fstab，并备份原配置。仅处理以下分区；关闭时不会卸载或删除数据。Windows 休眠或未正常关机可能导致挂载失败，不会强制修复。"
        Text {
            Layout.fillWidth: true; color: panel.ink; font.pixelSize: 13; wrapMode: Text.WrapAnywhere
            text: panel.pendingMounts.map(c => c.uuid + "  ·  " + (c.enabled ? "启用开机挂载 / " + (c.readOnly ? "只读" : "读写") : "关闭开机挂载")).join("\n")
        }
        actions: [
            ActionButton { objectName: "cancelMountApplyButton"; text: "取消"; foreground: panel.ink; implicitHeight: 38; onClicked: mountConfirm.close() },
            ActionButton {
                objectName: "confirmMountApplyButton"; text: "授权并应用"; accent: true; implicitHeight: 38
                enabled: panel.pendingMounts.length > 0 && !panel.controller.diskMounts.busy
                onClicked: { const choices = panel.pendingMounts; mountConfirm.close(); panel.controller.diskMounts.apply(choices); panel.mountChanges = ({}) }
            }
        ]
    }
    objectName: "settingsDialog"
    width: Math.min(860, host.layoutWidth - 40)
    height: Math.min(700, host.layoutHeight - 32)
    anchors.centerIn: parent
    modal: true; padding: 0
    background: Item {
        Rectangle { anchors.fill: parent; anchors.topMargin: 10; anchors.margins: -2; radius: 20; color: panel.theme.shadow }
        Rectangle { anchors.fill: parent; radius: 16; color: panel.theme.window; border.color: panel.line }
    }
    Overlay.modal: Rectangle { color: panel.theme.scrim }
    header: Item {
        implicitHeight: 60
        RowLayout {
            anchors.left: parent.left; anchors.leftMargin: 22; anchors.verticalCenter: parent.verticalCenter; spacing: 10
            Rectangle {
                Layout.preferredWidth: 30; Layout.preferredHeight: 30; radius: 9
                color: panel.theme.accentSoft
                Icon { anchors.centerIn: parent; name: "settings"; width: 17; height: 17; tint: panel.theme.accentText }
            }
            Text { text: "偏好设置"; color: panel.ink; font.pixelSize: 18; font.weight: Font.DemiBold }
        }
        ActionButton { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; iconName: "close"; hint: "关闭设置"; onClicked: panel.close() }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: panel.line }
    }
    footer: Item {
        implicitHeight: 56
        Rectangle { width: parent.width; height: 1; color: panel.line }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 22; anchors.rightMargin: 16; spacing: 8
            Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: panel.theme.success }
            Text { text: "更改会立即生效并自动保存"; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true }
            ActionButton { text: "完成"; accent: true; implicitHeight: 34; onClicked: panel.close() }
        }
    }

    // ---- Building blocks -------------------------------------------------------------------
    // A group: a small heading above one card of rows.
    component Group: ColumnLayout {
        property string heading: ""
        default property alias rows: card.data
        Layout.fillWidth: true; spacing: 8
        Text { visible: text !== ""; text: heading; color: panel.muted; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.leftMargin: 4 }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: card.implicitHeight
            radius: 12; color: panel.theme.raised; border.color: panel.line
            ColumnLayout { id: card; width: parent.width; spacing: 0 }
        }
    }
    // The hairline above every row but a card's first visible one.
    component Divider: Rectangle {
        required property Item row
        readonly property bool first: {
            const rows = row.parent ? row.parent.children : []
            for (const r of rows) if (r.visible) return r === row
            return true
        }
        visible: !first
        x: 16; width: row.width - 32; height: 1
        color: panel.line; opacity: 0.8
    }
    // A setting: title and description on the left, its controls on the right.
    component SettingRow: Item {
        id: settingRow
        property string title: ""
        property string description: ""
        property color titleColor: panel.ink
        default property alias controls: slot.data
        Layout.fillWidth: true
        implicitHeight: Math.max(54, Math.max(texts.implicitHeight, slot.implicitHeight) + 26)
        Divider { row: settingRow }
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; spacing: 16
            ColumnLayout {
                id: texts
                opacity: settingRow.enabled ? 1 : 0.5
                Layout.fillWidth: true; Layout.alignment: Qt.AlignVCenter; spacing: 3
                Text { text: settingRow.title; color: settingRow.titleColor; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Text { visible: text !== ""; text: settingRow.description; color: panel.muted; font.pixelSize: 12; lineHeight: 1.15; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            RowLayout { id: slot; Layout.alignment: Qt.AlignVCenter; spacing: 8 }
        }
    }
    // A setting whose control needs the full width (text boxes, lists): it sits below the text.
    component BlockRow: Item {
        id: blockRow
        property string title: ""
        property string description: ""
        default property alias content: body.data
        Layout.fillWidth: true
        implicitHeight: column.implicitHeight + 28
        Divider { row: blockRow }
        ColumnLayout {
            id: column
            x: 16; y: 14; width: parent.width - 32; spacing: 10
            ColumnLayout {
                visible: blockRow.title !== "" || blockRow.description !== ""
                Layout.fillWidth: true; spacing: 3
                Text { visible: text !== ""; text: blockRow.title; color: panel.ink; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Text { visible: text !== ""; text: blockRow.description; color: panel.muted; font.pixelSize: 12; lineHeight: 1.15; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            ColumnLayout { id: body; Layout.fillWidth: true; spacing: 10 }
        }
    }
    component ToggleRow: SettingRow {
        id: toggleRow
        property alias checked: toggle.checked
        signal toggled(bool value)
        Switch {
            id: toggle
            padding: 0; implicitWidth: 44; implicitHeight: 26
            onToggled: toggleRow.toggled(checked)
            indicator: Rectangle {
                implicitWidth: 44; implicitHeight: 26; radius: 13
                color: toggle.checked ? panel.theme.accent : panel.theme.toggleOff
                opacity: toggle.enabled ? 1 : 0.45
                Behavior on color { enabled: panel.animated; ColorAnimation { duration: 140 } }
                Rectangle {
                    x: toggle.checked ? 21 : 3; y: 3; width: 20; height: 20; radius: 10; color: panel.theme.knob
                    border.width: 1; border.color: Qt.alpha("#000000", 0.06)
                    Behavior on x { enabled: panel.animated; NumberAnimation { duration: Motion.snappy.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.snappy.curve } }
                }
            }
        }
    }
    // A quiet bordered button for actions inside rows.
    component RowButton: ActionButton {
        id: rowButton
        implicitHeight: 32
        leftPadding: 12; rightPadding: 12
        foreground: panel.ink
        background: Rectangle {
            radius: 8
            color: rowButton.down ? panel.theme.pressed : rowButton.hovered ? panel.theme.hover : panel.theme.field
            border.color: rowButton.hovered ? panel.theme.lineStrong : panel.line
            Behavior on color { enabled: panel.animated; ColorAnimation { duration: 100 } }
        }
    }
    // − and + of a stepper, centred in their half of the pill.
    component StepButton: AbstractButton {
        id: step
        implicitWidth: 34; implicitHeight: 32
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        contentItem: Text {
            text: step.text; color: step.enabled ? panel.ink : panel.theme.faint
            font.pixelSize: 17; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle { radius: 7; color: step.down ? panel.theme.pressed : step.hovered && step.enabled ? panel.theme.hover : "transparent" }
    }
    component Field: TextField {
        id: field
        ContextMenu.menu: null
        TextMenuArea { target: field }
        selectionColor: panel.theme.selection; selectedTextColor: panel.theme.selectionText
        color: panel.ink; font.pixelSize: 14; selectByMouse: true
        leftPadding: 11; rightPadding: 11; topPadding: 9; bottomPadding: 9
        background: Rectangle { radius: 8; color: panel.theme.field; border.color: field.activeFocus ? panel.theme.accent : panel.line }
    }
    component Area: TextArea {
        id: area
        ContextMenu.menu: null
        TextMenuArea { target: area }
        selectionColor: panel.theme.selection; selectedTextColor: panel.theme.selectionText
        color: panel.ink; font.pixelSize: 13; font.family: "monospace"; selectByMouse: true
        padding: 10; wrapMode: TextEdit.WrapAnywhere
        placeholderTextColor: panel.theme.faint
        background: Rectangle { radius: 8; color: panel.theme.field; border.color: area.activeFocus ? panel.theme.accent : panel.line }
    }
    // A small label in a soft pill.
    component Chip: Rectangle {
        property alias text: chipText.text
        property color tone: panel.muted
        implicitWidth: chipText.implicitWidth + 14; implicitHeight: 22; radius: 11
        color: panel.theme.sunken; border.color: panel.line
        Text { id: chipText; anchors.centerIn: parent; color: parent.tone; font.pixelSize: 11 }
    }
    // A miniature nook window in the light or dark palette, for the theme choice.
    // (The fixed colours are Theme's light and dark palettes, drawn small.)
    component MiniWindow: Rectangle {
        id: mini
        property bool darkLook: false
        readonly property color chrome: darkLook ? "#262522" : "#ece8df"
        readonly property color paneColor: darkLook ? "#2c2a27" : "#fcfbf8"
        readonly property color stroke: darkLook ? "#4b4842" : "#d5cdbe"
        readonly property color lines: darkLook ? "#5a5650" : "#d8d0c3"
        readonly property color accentColor: darkLook ? "#d97757" : "#c96442"
        readonly property color selectionColor: darkLook ? "#574575" : "#e2d7f3"
        radius: 7
        color: darkLook ? "#1f1e1c" : "#f4f1ea"
        border.color: stroke
        Rectangle { x: 1; y: 1; width: mini.width - 2; height: 11; radius: 6; color: mini.chrome }
        Rectangle { x: 6; y: 3.5; width: 16; height: 5; radius: 2.5; color: mini.accentColor }
        Column {
            x: 7; y: 19; spacing: 5
            Repeater { model: 4; Rectangle { required property int index; width: index === 0 ? 22 : 18; height: 4; radius: 2; color: index === 0 ? mini.accentColor : mini.lines } }
        }
        Rectangle {
            id: miniPane
            x: 36; y: 17; width: mini.width - 42; height: mini.height - 23; radius: 5
            color: mini.paneColor; border.color: mini.stroke
            Column {
                x: 6; y: 6; spacing: 5
                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        width: index === 1 ? miniPane.width - 12 : (miniPane.width - 12) * (0.75 - index * 0.1); height: 6; radius: 3
                        color: index === 1 ? mini.selectionColor : mini.lines
                    }
                }
            }
        }
    }
    component NavItem: AbstractButton {
        id: navItem
        required property var modelData
        required property int index
        objectName: "settingsCategory" + modelData.page
        readonly property bool chosen: panel.category === modelData.page
        // Placed by index rather than by a positioner, so they are where they belong from the
        // moment the dialog opens.
        width: parent ? parent.width : 0; height: 40
        y: index * 43
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        onClicked: panel.category = modelData.page
        background: Item {}
        contentItem: RowLayout {
            spacing: 10
            Icon { name: navItem.modelData.icon; tint: navItem.chosen ? panel.theme.accentText : panel.theme.icon; Layout.leftMargin: 12; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
            Text { text: navItem.modelData.title; color: navItem.chosen ? panel.theme.accentText : panel.theme.inkSoft; font.pixelSize: 14; font.weight: navItem.chosen ? Font.DemiBold : Font.Normal; Layout.fillWidth: true; elide: Text.ElideRight }
            // A newer version is installed and waits for a restart.
            Rectangle {
                visible: navItem.modelData.page === 3 && panel.controller.upgraded
                Layout.rightMargin: 12; Layout.preferredWidth: 7; Layout.preferredHeight: 7; radius: 3.5
                color: panel.theme.accent
            }
        }
    }
    component ThemeTile: AbstractButton {
        id: tile
        property string key: ""
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        readonly property bool chosen: panel.prefs.theme === key
        onClicked: panel.controller.settings.set("theme", key)
        background: Rectangle {
            radius: 11
            color: tile.hovered && !tile.chosen ? panel.theme.hover : "transparent"
            border.color: panel.line
        }
        contentItem: ColumnLayout {
            spacing: 8
            Item {
                Layout.fillWidth: true; Layout.preferredHeight: 66; Layout.leftMargin: 10; Layout.rightMargin: 10; Layout.topMargin: 10
                // System: half light, half dark.
                Item {
                    width: tile.key === "system" ? parent.width / 2 : parent.width; height: parent.height; clip: true
                    MiniWindow { width: parent.parent.width; height: parent.height; darkLook: tile.key === "dark" }
                }
                Item {
                    visible: tile.key === "system"
                    x: parent.width / 2; width: parent.width / 2; height: parent.height; clip: true
                    MiniWindow { x: -parent.x; width: parent.parent.width; height: parent.height; darkLook: true }
                }
            }
            Text {
                text: tile.text; color: tile.chosen ? panel.theme.accentText : panel.ink
                font.pixelSize: 13; font.weight: tile.chosen ? Font.DemiBold : Font.Normal
                Layout.alignment: Qt.AlignHCenter; Layout.bottomMargin: 9
            }
        }
    }

    // ---- Layout -------------------------------------------------------------------------------
    contentItem: RowLayout {
        spacing: 0
        Rectangle {
            // Sized before the first layout pass too, so its pages can be clicked at once.
            implicitWidth: 188
            Layout.preferredWidth: 188; Layout.fillHeight: true
            color: panel.theme.sunken
            Item {
                anchors.fill: parent; anchors.margins: 12; anchors.topMargin: 14
                HoverPlate { id: navHover; animated: panel.animated; color: panel.theme.hover }
                // The chosen page's plate glides between pages.
                Rectangle {
                    id: navPlate
                    readonly property Item target: {
                        for (const item of navColumn.children)
                            if (item.chosen) return item
                        return null
                    }
                    Glide {
                        id: navGlide
                        animated: panel.animated; shown: panel.opened
                        key: panel.category
                        stiffness: Motion.snappy.k; damping: Motion.snappy.c
                        targetY: navPlate.target ? navPlate.target.y : 0
                        targetHeight: navPlate.target ? navPlate.target.height : 0
                    }
                    visible: !!target
                    width: parent.width; y: navGlide.y; height: navGlide.height
                    radius: 9
                    color: panel.theme.activeFill; border.color: panel.theme.activeBorder
                    Rectangle { x: 0; width: 3; height: 18; radius: 2; anchors.verticalCenter: parent.verticalCenter; color: panel.theme.accent }
                }
                Item {
                    id: navColumn
                    width: parent.width; height: panel.pages.length * 43
                    Repeater {
                        model: panel.pages
                        NavItem {
                            id: navEntry
                            onHoveredChanged: navHover.track(navEntry, hovered && !chosen)
                            onChosenChanged: if (chosen) navHover.track(navEntry, false)
                        }
                    }
                }
            }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: panel.line }
        ScrollView {
            id: scroll
            objectName: "settingsScroll"
            // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
            Component.onCompleted: contentItem.acceptedButtons = Qt.NoButton
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ScrollBar.vertical: NookScrollBar { parent: scroll; x: scroll.width - width - 2; y: 4; height: scroll.availableHeight - 8 }
            ColumnLayout {
                id: page
                width: scroll.availableWidth
                spacing: 22
                transform: Translate { id: pageShift }
                ParallelAnimation {
                    id: pageIn
                    NumberAnimation { target: page; property: "opacity"; from: 0; to: 1; duration: 160; easing.type: Easing.OutCubic }
                    NumberAnimation { target: pageShift; property: "y"; from: 10; to: 0; duration: Motion.snappy.ms; easing.type: Easing.BezierSpline; easing.bezierCurve: Motion.snappy.curve }
                }
                // Page title
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; Layout.topMargin: 22; spacing: 4
                    Text { text: panel.current.title; color: panel.ink; font.pixelSize: 22; font.weight: Font.DemiBold }
                    Text { text: panel.current.summary; color: panel.muted; font.pixelSize: 13 }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 28; Layout.rightMargin: 28; Layout.bottomMargin: 26
                    spacing: 22

                    // ---- 外观与主题 ----
                    Group {
                        visible: panel.category === 0; heading: "主题"
                        BlockRow {
                            Item {
                                Layout.fillWidth: true; implicitHeight: 112
                                Row {
                                    id: tiles
                                    anchors.fill: parent; spacing: 10
                                    Repeater {
                                        model: [{key: "system", title: "跟随系统"}, {key: "light", title: "浅色"}, {key: "dark", title: "深色"}]
                                        ThemeTile { required property var modelData; key: modelData.key; text: modelData.title; width: (tiles.width - 20) / 3; height: tiles.height }
                                    }
                                }
                                // The chosen tile's ring glides between tiles.
                                Rectangle {
                                    id: ring
                                    readonly property Item target: {
                                        for (const tile of tiles.children)
                                            if (tile.chosen) return tile
                                        return null
                                    }
                                    Glide {
                                        id: ringGlide
                                        animated: panel.animated; shown: panel.opened && panel.category === 0
                                        key: panel.prefs.theme
                                        stiffness: Motion.snappy.k; damping: Motion.snappy.c
                                        targetX: ring.target ? ring.target.x : 0
                                        targetWidth: ring.target ? ring.target.width : 0
                                    }
                                    visible: !!target
                                    x: ringGlide.x; width: ringGlide.width; height: parent.height
                                    radius: 11; color: "transparent"
                                    border.width: 2; border.color: panel.theme.accent
                                }
                            }
                        }
                        ToggleRow { title: "半透明窗口"; description: "窗口与侧栏略微透出桌面背景"; checked: panel.prefs.effects; onToggled: value => panel.controller.settings.set("effects", value) }
                        ToggleRow { title: "轻量动效"; description: "悬停、选中、切换视图和弹出时的过渡动画"; checked: panel.prefs.animations; onToggled: value => panel.controller.settings.set("animations", value) }
                    }
                    Group {
                        visible: panel.category === 0; heading: "大小"
                        SettingRow {
                            title: "界面缩放"; description: "调整全部文字、图标和控件的大小"
                            Rectangle {
                                implicitWidth: 132; implicitHeight: 34; radius: 9
                                color: panel.theme.field; border.color: panel.line
                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 1; spacing: 0
                                    StepButton { objectName: "zoomOutButton"; text: "−"; enabled: panel.prefs.uiScale > 0.75; onClicked: panel.controller.settings.set("uiScale", Math.round((panel.prefs.uiScale - 0.05) * 100) / 100) }
                                    Label { objectName: "zoomValue"; text: Math.round(panel.prefs.uiScale * 100) + "%"; color: panel.ink; font.pixelSize: 14; font.weight: Font.Medium; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                                    StepButton { objectName: "zoomInButton"; text: "+"; enabled: panel.prefs.uiScale < 1.5; onClicked: panel.controller.settings.set("uiScale", Math.round((panel.prefs.uiScale + 0.05) * 100) / 100) }
                                }
                            }
                            RowButton { objectName: "resetZoomButton"; text: "重置"; enabled: Math.abs(panel.prefs.uiScale - 1) > 0.001; onClicked: panel.controller.settings.set("uiScale", 1.0) }
                        }
                    }

                    // ---- 浏览与交互 ----
                    Group {
                        visible: panel.category === 1; heading: "文件列表"
                        ToggleRow { title: "紧凑列表"; description: "减小行高，一屏显示更多文件"; checked: panel.prefs.dense; onToggled: value => panel.controller.settings.set("dense", value) }
                        ToggleRow { title: "悬停预览静态文件"; description: "指针在图片、文档上停留片刻后打开预览"; checked: panel.prefs.hoverPreview; onToggled: value => panel.controller.settings.set("hoverPreview", value) }
                        SettingRow {
                            title: "列宽"; description: "名称、修改时间、类型和大小四列的宽度"
                            RowButton { objectName: "resetColumnWidthsButton"; text: "恢复默认列宽"; iconName: "refresh"; onClicked: panel.controller.settings.set("columnWidths", []) }
                        }
                        SettingRow {
                            title: "滚轮速度"; description: "鼠标滚轮每转一格滚动的距离"
                            Slider {
                                id: speed
                                Layout.preferredWidth: 180
                                from: 1; to: 8; stepSize: 1; snapMode: Slider.SnapAlways; value: panel.prefs.scrollSpeed
                                onMoved: panel.controller.settings.set("scrollSpeed", Math.round(value))
                                background: Item {
                                    x: speed.leftPadding; y: speed.topPadding + speed.availableHeight / 2 - 2
                                    width: speed.availableWidth; height: 4
                                    Rectangle { anchors.fill: parent; radius: 2; color: panel.theme.track }
                                    Rectangle { width: speed.visualPosition * parent.width; height: 4; radius: 2; color: panel.theme.accent }
                                    Repeater {
                                        model: 8
                                        Rectangle { x: index / 7 * parent.width - 1; y: 8; width: 2; height: 4; radius: 1; color: panel.theme.lineStrong }
                                    }
                                }
                                handle: Rectangle {
                                    x: speed.leftPadding + speed.visualPosition * (speed.availableWidth - width); y: speed.topPadding + speed.availableHeight / 2 - height / 2
                                    width: 18; height: 18; radius: 9; color: panel.theme.knob; border.color: panel.theme.accent; border.width: 2
                                    scale: speed.pressed ? 1.12 : 1
                                    Behavior on scale { enabled: panel.animated; NumberAnimation { duration: 120 } }
                                }
                            }
                            Text { text: Math.round(speed.value) + " 倍"; color: panel.theme.accentText; font.pixelSize: 13; font.weight: Font.Medium; Layout.preferredWidth: 34; horizontalAlignment: Text.AlignRight }
                        }
                    }
                    Group {
                        visible: panel.category === 1; heading: "侧栏与启动"
                        SettingRow {
                            title: "快速访问"; description: "找回从侧栏移除的默认位置"
                            RowButton { objectName: "resetQuickAccessButton"; text: "恢复默认项"; iconName: "refresh"; enabled: panel.prefs.hiddenPlaces.length > 0; onClicked: panel.controller.resetQuickAccess() }
                        }
                        BlockRow {
                            title: "启动目录"; description: "打开新窗口或新标签页时显示的文件夹"
                            Field { Layout.fillWidth: true; text: panel.prefs.startFolder; onEditingFinished: panel.controller.settings.set("startFolder", text) }
                        }
                    }
                    Group {
                        visible: panel.category === 1; heading: "撤销与隐私"
                        BlockRow {
                            description: "侧栏保留本次运行最近 5 次可撤销操作，可点击任意一条单独撤销；空白处右键或 Ctrl+Z 撤销最新一条。关闭应用后记录不保留，也不记录最近访问的文件夹。"
                        }
                        BlockRow {
                            description: "支持本地文件的粘贴、移动、重命名、新建与移入回收站。永久删除、覆盖替换、跨盘移动、特殊字节文件名、回收站手动恢复及远程操作不支持撤销；文件变化或原位置被占用时会停止，绝不覆盖。"
                        }
                        SettingRow {
                            title: "撤销记录"; description: panel.controller.operations.canUndo ? "本次运行中可撤销的操作" : "目前没有可撤销的操作"
                            RowButton {
                                objectName: "clearUndoHistorySettingsButton"
                                text: "清空撤销记录"; iconName: "close"
                                enabled: panel.controller.operations.canUndo
                                onClicked: panel.controller.operations.clearUndoHistory()
                            }
                        }
                    }

                    // ---- 搜索与索引 ----
                    Group {
                        visible: panel.category === 2; heading: "索引状态"
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: statusRow.implicitHeight + 32
                            RowLayout {
                                id: statusRow
                                x: 16; y: 16; width: parent.width - 32; spacing: 14
                                Rectangle {
                                    Layout.preferredWidth: 46; Layout.preferredHeight: 46; Layout.alignment: Qt.AlignTop; radius: 13
                                    color: panel.theme.accentSoft
                                    Icon { anchors.centerIn: parent; name: "search"; width: 22; height: 22; tint: panel.theme.accentText }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 4
                                    RowLayout {
                                        spacing: 10
                                        Label {
                                            objectName: "indexedCountLabel"
                                            text: panel.controller.search.indexed.toLocaleString(Qt.locale(), 'f', 0) + " 个项目"
                                            color: panel.ink; font.pixelSize: 22; font.weight: Font.DemiBold
                                        }
                                        BusyIndicator { running: panel.controller.search.scanning; visible: running; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                                    }
                                    Label { objectName: "indexStatusLabel"; text: panel.controller.search.status; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                    Label { objectName: "indexWarningsLabel"; text: panel.controller.search.warnings.join("\n"); visible: text !== ""; color: panel.theme.warning; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                                }
                                RowButton {
                                    objectName: "updateIndexButton"
                                    Layout.alignment: Qt.AlignTop
                                    text: panel.controller.search.scanning ? "停止本次更新" : "更新索引"
                                    iconName: panel.controller.search.scanning ? "close" : "refresh"
                                    onClicked: { if (panel.controller.search.scanning) panel.controller.search.stop(); else panel.controller.search.start() }
                                }
                            }
                        }
                        ToggleRow {
                            objectName: "backgroundIndexToggle"
                            title: "开机后在后台维护索引"
                            description: panel.prefs.backgroundIndex
                                         ? "登录后自动启动一个无窗口的低优先级进程（空闲 I/O 优先级），启动时核对一次，之后通过目录监听增量更新；关闭窗口不会停止索引，再次打开文件管理器也无需重新扫描。"
                                         : "索引随窗口启动，关闭最后一个窗口后停止；下次打开时会重新核对整个磁盘。"
                            checked: panel.prefs.backgroundIndex
                            onToggled: value => panel.controller.settings.set("backgroundIndex", value)
                        }
                    }
                    Group {
                        visible: panel.category === 2; heading: "搜索范围"
                        BlockRow {
                            title: "扫描目录"; description: "每行一个，留空使用默认"
                            Area { id: rootsInput; objectName: "rootsInput"; Layout.fillWidth: true; Layout.preferredHeight: 82; placeholderText: "/home\n/mnt/data" }
                        }
                        BlockRow {
                            title: "排除目录"; description: "每行一个，这些目录及其内容不会被索引"
                            Area { id: excludeInput; objectName: "excludeInput"; Layout.fillWidth: true; Layout.preferredHeight: 74; placeholderText: "/home/用户名/缓存" }
                        }
                        ToggleRow {
                            objectName: "indexSystemToggle"
                            title: "包含系统目录"; description: "/usr、/etc、/opt、/var 等"
                            checked: panel.prefs.indexSystem
                            onToggled: value => { panel.controller.settings.set("indexSystem", value); panel.reindexRequested() }
                        }
                        ToggleRow {
                            objectName: "indexNoiseToggle"
                            title: "包含开发与缓存目录"
                            description: "node_modules、.git、__pycache__、构建输出（CMake 构建目录、带 CACHEDIR.TAG 的 target 等）、Python 虚拟环境，以及浏览器与 Electron 应用的缓存。关闭时它们不会出现在搜索结果里，索引也更小。"
                            checked: panel.prefs.indexNoise
                            onToggled: value => { panel.controller.settings.set("indexNoise", value); panel.reindexRequested() }
                        }
                        SettingRow {
                            title: "应用范围更改"; description: "保存扫描与排除目录，并重新建立索引"
                            ActionButton {
                                objectName: "saveIndexScopeButton"; text: "保存并重新索引"; iconName: "refresh"; accent: true; implicitHeight: 34
                                onClicked: {
                                    panel.controller.settings.set("roots", rootsInput.text.split("\n").map(s => s.trim()).filter(s => s))
                                    panel.controller.settings.set("exclusions", excludeInput.text.split("\n").map(s => s.trim()).filter(s => s))
                                    panel.reindexRequested()
                                }
                            }
                        }
                    }
                    Group {
                        visible: panel.category === 2; heading: "Windows 分区（NTFS）"
                        ToggleRow {
                            objectName: "ntfsJournalToggle"
                            title: "NTFS 极速索引"
                            enabled: !panel.controller.ntfsBusy
                            description: panel.controller.ntfsBusy
                                         ? "正在等待管理员授权…"
                                         : "像 Everything 一样直接读取文件表（$MFT，百万个文件约 1 秒），之后只读取 USN 变更日志，不再逐个目录扫描。开启时输入一次管理员密码，之后免密；分区始终以只读方式打开。"
                                           + "\n仅对已挂载的整个 NTFS 分区生效。在 Windows 中的改动由变更日志得知；在 Linux 中的改动由目录监听和每日一次的文件表核对补上。"
                                           + (panel.controller.ntfsInstalled ? "" : "\n需要先安装 nook 软件包（含只读读取助手）。")
                            checked: panel.prefs.ntfsJournal
                            onToggled: value => {
                                panel.controller.setNtfsJournal(value)
                                // Follows the saved state: a cancelled password prompt leaves it off.
                                checked = Qt.binding(() => panel.prefs.ntfsJournal)
                            }
                        }
                    }

                    // ---- 磁盘与挂载 ----
                    Group {
                        visible: panel.category === 4; heading: "开机自动挂载"
                        SettingRow {
                            title: "数据分区"; description: "选择开机时自动挂载的 NTFS / exFAT 分区。应用时需要管理员授权，并会先备份 /etc/fstab。"
                            RowButton { objectName: "refreshMountsButton"; text: "刷新"; iconName: "refresh"; enabled: !panel.controller.diskMounts.busy; onClicked: { panel.mountChanges = ({}); panel.controller.diskMounts.refresh() } }
                            ActionButton { objectName: "applyMountsButton"; text: "应用更改"; accent: true; implicitHeight: 32; enabled: panel.pendingMounts.length > 0 && !panel.controller.diskMounts.busy; onClicked: mountConfirm.open() }
                        }
                        Repeater {
                            model: panel.controller.diskMounts.disks
                            Item {
                                id: diskRow
                                required property var modelData
                                readonly property var choice: panel.mountChanges[modelData.uuid] || ({enabled: modelData.autoMount, readOnly: modelData.readOnly})
                                readonly property bool changed: !!panel.mountChanges[modelData.uuid]
                                Layout.fillWidth: true
                                implicitHeight: diskColumn.implicitHeight + 28
                                Divider { row: diskRow }
                                ColumnLayout {
                                    id: diskColumn
                                    x: 16; y: 14; width: parent.width - 32; spacing: 10
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 12
                                        Rectangle {
                                            Layout.preferredWidth: 38; Layout.preferredHeight: 38; Layout.alignment: Qt.AlignTop; radius: 10
                                            color: panel.theme.sunken; border.color: panel.line
                                            Icon { anchors.centerIn: parent; name: "drive"; width: 20; height: 20 }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 5
                                            RowLayout {
                                                spacing: 8
                                                Text { text: diskRow.modelData.name; color: panel.ink; font.pixelSize: 15; font.weight: Font.DemiBold }
                                                Chip { text: diskRow.modelData.size }
                                                Chip { text: diskRow.modelData.filesystem.toUpperCase() }
                                                Chip { visible: diskRow.changed; text: "待应用"; tone: panel.theme.accentText }
                                            }
                                            RowLayout {
                                                spacing: 6
                                                Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: diskRow.modelData.mountpoint ? panel.theme.success : panel.theme.faint }
                                                Text { text: diskRow.modelData.mountpoint ? "已挂载于 " + diskRow.modelData.mountpoint : "未挂载"; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideMiddle }
                                            }
                                            Text { text: "UUID " + diskRow.modelData.uuid; color: panel.theme.faint; font.pixelSize: 11; font.family: "monospace"; Layout.fillWidth: true; elide: Text.ElideRight }
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true; Layout.leftMargin: 50
                                        implicitHeight: diskToggles.implicitHeight
                                        radius: 10; color: panel.theme.sunken; border.color: panel.line
                                        ColumnLayout {
                                            id: diskToggles
                                            width: parent.width; spacing: 0
                                            ToggleRow { title: "开机自动挂载"; enabled: !panel.controller.diskMounts.busy; checked: diskRow.choice.enabled; onToggled: value => panel.changeMount(diskRow.modelData, value, diskRow.choice.readOnly) }
                                            ToggleRow { title: "只读访问"; description: "防止在 Linux 中改动这个分区"; enabled: diskRow.choice.enabled && !panel.controller.diskMounts.busy; checked: diskRow.choice.readOnly; onToggled: value => panel.changeMount(diskRow.modelData, diskRow.choice.enabled, value) }
                                        }
                                    }
                                }
                            }
                        }
                        BlockRow {
                            visible: !panel.controller.diskMounts.busy && panel.controller.diskMounts.disks.length === 0
                            description: "未发现可配置的固定 NTFS / exFAT 数据分区"
                        }
                    }
                    Label { visible: panel.category === 4 && text !== ""; objectName: "mountStatus"; text: panel.controller.diskMounts.status; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; Layout.leftMargin: 4; wrapMode: Text.WrapAnywhere }

                    // ---- 关于应用 ----
                    Group {
                        visible: panel.category === 3
                        Item {
                            Layout.fillWidth: true; implicitHeight: about.implicitHeight + 44
                            ColumnLayout {
                                id: about
                                anchors.horizontalCenter: parent.horizontalCenter; y: 24
                                width: Math.min(parent.width - 40, 420); spacing: 8
                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 76; Layout.preferredHeight: 76; radius: 22
                                    color: panel.theme.accentSoft; border.color: Qt.alpha(panel.theme.accent, 0.3)
                                    Icon { anchors.centerIn: parent; name: "folder"; width: 44; height: 44 }
                                }
                                Text { Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 6; text: "nook · 星择"; color: panel.ink; font.pixelSize: 22; font.weight: Font.DemiBold }
                                RowLayout {
                                    Layout.alignment: Qt.AlignHCenter; spacing: 6
                                    Chip { text: "版本 " + (Qt.application.version || "0.3.1"); tone: panel.theme.accentText }
                                    Chip {
                                        visible: panel.controller.upgraded
                                        text: panel.controller.installedVersion !== "" ? "已安装 " + panel.controller.installedVersion : "已安装新版本"
                                        tone: panel.theme.accentText
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true; Layout.topMargin: 6; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                                    text: "原生 Linux 文件管理器，同一套外观里还有文件选择器和 nook解压缩。"
                                    color: panel.muted; font.pixelSize: 13; lineHeight: 1.2
                                }
                            }
                        }
                    }
                    Group {
                        visible: panel.category === 3; heading: "更新"
                        SettingRow {
                            title: panel.controller.upgraded
                                   ? (panel.controller.installedVersion !== "" ? "新版本 " + panel.controller.installedVersion + " 已安装" : "新版本已安装")
                                   : "重启 nook"
                            description: panel.controller.operations.busy
                                         ? "正在复制、移动或删除文件，完成后才能重启"
                                         : (panel.controller.upgraded ? "重启后切换到新版本。" : "用 pacman 安装新版本后，在这里重启即可切换。")
                                           + "所有窗口和标签页会原样恢复，撤销记录会清空。"
                            RowButton {
                                objectName: "restartButton"; text: "重启"; iconName: "refresh"
                                enabled: !panel.controller.operations.busy
                                onClicked: panel.controller.restart()
                            }
                        }
                    }
                    Group {
                        visible: panel.category === 3; heading: "组成"
                        SettingRow { title: "界面"; Text { text: "Qt Quick"; color: panel.muted; font.pixelSize: 13 } }
                        SettingRow { title: "语言"; Text { text: "C++20"; color: panel.muted; font.pixelSize: 13 } }
                        SettingRow { title: "文件与设备"; Text { text: "GIO"; color: panel.muted; font.pixelSize: 13 } }
                        SettingRow { title: "文件名索引"; Text { text: "SQLite"; color: panel.muted; font.pixelSize: 13 } }
                    }
                }
            }
        }
    }
}
