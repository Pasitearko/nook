import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

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
    property int category: 0
    onCategoryChanged: if (scroll.contentItem) scroll.contentItem.contentY = 0
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
    }
    Connections { target: panel.controller.diskMounts; function onDisksChanged() { panel.mountChanges = ({}) } }
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
    width: Math.min(820, host.layoutWidth - 40)
    height: Math.min(690, host.layoutHeight - 32)
    anchors.centerIn: parent
    modal: true; padding: 0
    background: Rectangle { radius: 16; color: panel.theme.window; border.color: panel.line }
    Overlay.modal: Rectangle { color: panel.theme.scrim }
    header: Item {
        implicitHeight: 68
        Text { x: 24; anchors.verticalCenter: parent.verticalCenter; text: "偏好设置"; color: panel.ink; font.pixelSize: 22; font.weight: Font.DemiBold }
        ActionButton { anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; iconName: "close"; hint: "关闭设置"; onClicked: panel.close() }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: panel.line }
    }
    footer: Item {
        implicitHeight: 58
        Rectangle { width: parent.width; height: 1; color: panel.line }
        ActionButton { anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; text: "完成"; accent: true; onClicked: panel.close() }
    }
    component Card: Frame {
        property string heading: ""
        default property alias entries: body.data
        Layout.fillWidth: true; Layout.margins: 14; Layout.topMargin: 0; Layout.bottomMargin: 0
        padding: 16
        background: Rectangle { radius: 12; color: panel.theme.raised; border.color: panel.line }
        contentItem: ColumnLayout {
            spacing: 16
            Label { text: heading; color: panel.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true }
            ColumnLayout { id: body; Layout.fillWidth: true; spacing: 16 }
        }
    }
    component ToggleRow: RowLayout {
        property string title: ""
        property alias checked: toggle.checked
        signal toggled(bool value)
        Layout.fillWidth: true; spacing: 12
        Label { text: title; color: panel.ink; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.Wrap }
        Switch {
            id: toggle
            padding: 0; implicitWidth: 42; implicitHeight: 26
            onToggled: parent.toggled(checked)
            indicator: Rectangle {
                implicitWidth: 42; implicitHeight: 26; radius: 13
                color: toggle.checked ? panel.theme.accent : panel.theme.toggleOff
                Behavior on color { enabled: panel.prefs.animations; ColorAnimation { duration: 130 } }
                Rectangle { x: toggle.checked ? 19 : 3; y: 3; width: 20; height: 20; radius: 10; color: theme.knob; Behavior on x { enabled: panel.prefs.animations; NumberAnimation { duration: 130 } } }
            }
        }
    }
    component Field: TextField {
        id: field
        ContextMenu.menu: null
        TextMenuArea { target: field }
        selectionColor: panel.theme.selection; selectedTextColor: panel.theme.selectionText
        color: panel.ink; font.pixelSize: 14; selectByMouse: true
        leftPadding: 11; rightPadding: 11; topPadding: 10; bottomPadding: 10
        background: Rectangle { radius: 8; color: panel.theme.field; border.color: parent.activeFocus ? panel.theme.accent : panel.line }
    }
    component Area: TextArea {
        id: area
        ContextMenu.menu: null
        TextMenuArea { target: area }
        selectionColor: panel.theme.selection; selectedTextColor: panel.theme.selectionText
        color: panel.ink; font.pixelSize: 13; selectByMouse: true
        padding: 10; wrapMode: TextEdit.WrapAnywhere
        background: Rectangle { radius: 8; color: panel.theme.field; border.color: parent.activeFocus ? panel.theme.accent : panel.line }
    }
    contentItem: RowLayout {
        spacing: 0
        ColumnLayout {
            Layout.preferredWidth: 142; Layout.fillHeight: true; Layout.margins: 12; spacing: 5
            Repeater {
                model: [{title: "外观与主题", icon: "sun"}, {title: "浏览与交互", icon: "list"}, {title: "搜索与索引", icon: "search"}, {title: "关于应用", icon: "folder"}, {title: "磁盘与挂载", icon: "drive"}]
                ActionButton {
                    required property var modelData
                    required property int index
                    objectName: "settingsCategory" + index
                    Layout.fillWidth: true; implicitHeight: 42
                    text: modelData.title; iconName: modelData.icon
                    foreground: panel.category === index ? panel.theme.accentText : panel.theme.inkSoft
                    active: panel.category === index
                    onClicked: panel.category = index
                }
            }
            Item { Layout.fillHeight: true }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: panel.line }
        ScrollView {
            id: scroll
            objectName: "settingsScroll"
            // Desktop: a mouse drag selects text or moves items; it must not drag the whole page around.
            Component.onCompleted: contentItem.acceptedButtons = Qt.NoButton
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth; spacing: 16
                Item { height: 2 }
                Card {
                    visible: panel.category === 0; heading: "主题"
                    RowLayout {
                        Layout.fillWidth: true; spacing: 4
                        Repeater {
                            model: [{key: "system", title: "系统"}, {key: "light", title: "浅色"}, {key: "dark", title: "深色"}]
                            ActionButton {
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: 40
                                text: modelData.title; foreground: panel.ink; active: panel.prefs.theme === modelData.key
                                onClicked: panel.controller.settings.set("theme", modelData.key)
                            }
                        }
                    }
                    ToggleRow { title: "半透明窗口"; checked: panel.prefs.effects; onToggled: value => panel.controller.settings.set("effects", value) }
                    ToggleRow { title: "轻量动效"; checked: panel.prefs.animations; onToggled: value => panel.controller.settings.set("animations", value) }
                }
                Card {
                    visible: panel.category === 0; heading: "界面缩放"
                    RowLayout {
                        Layout.fillWidth: true
                        ActionButton { objectName: "zoomOutButton"; text: "−"; enabled: panel.prefs.uiScale > 0.75; onClicked: panel.controller.settings.set("uiScale", Math.round((panel.prefs.uiScale - 0.05) * 100) / 100) }
                        Label { objectName: "zoomValue"; text: Math.round(panel.prefs.uiScale * 100) + "%"; color: panel.ink; font.pixelSize: 17; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                        ActionButton { objectName: "zoomInButton"; text: "+"; enabled: panel.prefs.uiScale < 1.5; onClicked: panel.controller.settings.set("uiScale", Math.round((panel.prefs.uiScale + 0.05) * 100) / 100) }
                        ActionButton { objectName: "resetZoomButton"; text: "重置"; onClicked: panel.controller.settings.set("uiScale", 1.0) }
                    }
                }
                Card {
                    visible: panel.category === 1; heading: "文件浏览"
                    ToggleRow { title: "紧凑列表"; checked: panel.prefs.dense; onToggled: value => panel.controller.settings.set("dense", value) }
                    ToggleRow { title: "悬停预览静态文件"; checked: panel.prefs.hoverPreview; onToggled: value => panel.controller.settings.set("hoverPreview", value) }
                    ActionButton { objectName: "resetColumnWidthsButton"; text: "恢复默认列宽"; iconName: "refresh"; foreground: panel.ink; onClicked: panel.controller.settings.set("columnWidths", []) }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "滚轮速度"; color: panel.ink; font.pixelSize: 14; Layout.fillWidth: true }
                        Label { text: Math.round(speed.value) + " 倍"; color: panel.theme.accentText; font.pixelSize: 14 }
                    }
                    Slider {
                        id: speed; Layout.fillWidth: true; from: 1; to: 8; stepSize: 1; value: panel.prefs.scrollSpeed
                        onMoved: panel.controller.settings.set("scrollSpeed", Math.round(value))
                        background: Rectangle { x: speed.leftPadding; y: speed.topPadding + speed.availableHeight / 2 - 2; width: speed.availableWidth; height: 4; radius: 2; color: panel.line; Rectangle { width: speed.visualPosition * parent.width; height: 4; radius: 2; color: panel.theme.accent } }
                        handle: Rectangle { x: speed.leftPadding + speed.visualPosition * (speed.availableWidth - width); y: speed.topPadding + speed.availableHeight / 2 - height / 2; width: 18; height: 18; radius: 9; color: panel.theme.knob; border.color: panel.theme.accent; border.width: 2 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "快速访问"; color: panel.ink; font.pixelSize: 14; Layout.fillWidth: true }
                        ActionButton { objectName: "resetQuickAccessButton"; text: "恢复默认快速访问项"; iconName: "refresh"; foreground: panel.ink; enabled: panel.prefs.hiddenPlaces.length > 0; onClicked: panel.controller.resetQuickAccess() }
                    }
                    Label { text: "启动目录"; color: panel.ink; font.pixelSize: 14 }
                    Field { Layout.fillWidth: true; text: panel.prefs.startFolder; onEditingFinished: panel.controller.settings.set("startFolder", text) }
                }
                Card {
                    visible: panel.category === 1; heading: "最近位置与隐私"
                    ToggleRow {
                        objectName: "rememberRecentLocationsToggle"
                        title: "记录最近访问的文件夹"
                        checked: panel.prefs.rememberRecentLocations
                        onToggled: value => panel.controller.settings.set("rememberRecentLocations", value)
                    }
                    Label { text: "侧栏最多显示 5 个最近位置，仅保存在本机。关闭此选项会同时清空记录。"; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    ActionButton {
                        objectName: "clearRecentLocationsSettingsButton"
                        text: "清空最近位置"; iconName: "trash"; foreground: panel.ink
                        enabled: panel.controller.recentLocations.length > 0
                        onClicked: panel.controller.clearRecentLocations()
                    }
                }
                Card {
                    visible: panel.category === 2; heading: "索引状态"
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            objectName: "indexedCountLabel"
                            text: panel.controller.search.indexed.toLocaleString(Qt.locale(), 'f', 0) + " 个项目"
                            color: panel.theme.accentText; font.pixelSize: 20; font.weight: Font.Medium
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                        }
                        BusyIndicator { running: panel.controller.search.scanning; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                    }
                    Label { objectName: "indexStatusLabel"; text: panel.controller.search.status; color: panel.muted; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Label { objectName: "indexWarningsLabel"; text: panel.controller.search.warnings.join("\n"); visible: text !== ""; color: panel.theme.warning; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                    ActionButton {
                        objectName: "updateIndexButton"
                        text: panel.controller.search.scanning ? "停止本次更新" : "更新索引"
                        iconName: panel.controller.search.scanning ? "close" : "refresh"
                        onClicked: { if (panel.controller.search.scanning) panel.controller.search.stop(); else panel.controller.search.start() }
                    }
                }
                Card {
                    visible: panel.category === 2; heading: "后台索引"
                    ToggleRow {
                        objectName: "backgroundIndexToggle"
                        title: "开机后在后台维护索引"
                        checked: panel.prefs.backgroundIndex
                        onToggled: value => panel.controller.settings.set("backgroundIndex", value)
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: panel.muted; font.pixelSize: 12
                        text: panel.prefs.backgroundIndex
                              ? "登录后自动启动一个无窗口的低优先级进程（空闲 I/O 优先级），启动时核对一次，之后通过目录监听增量更新；关闭窗口不会停止索引，再次打开文件管理器也无需重新扫描。"
                              : "索引随窗口启动，关闭最后一个窗口后停止；下次打开时会重新核对整个磁盘。"
                    }
                }
                Card {
                    visible: panel.category === 2; heading: "搜索范围"
                    Label { text: "扫描目录（每行一个，留空使用默认）"; color: panel.ink; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Area { id: rootsInput; objectName: "rootsInput"; Layout.fillWidth: true; Layout.preferredHeight: 82; placeholderText: "/home\n/mnt/data" }
                    ToggleRow {
                        objectName: "indexSystemToggle"
                        title: "包含系统目录（/usr、/etc、/opt、/var 等）"
                        checked: panel.prefs.indexSystem
                        onToggled: value => { panel.controller.settings.set("indexSystem", value); panel.reindexRequested() }
                    }
                    ToggleRow {
                        objectName: "indexNoiseToggle"
                        title: "包含开发与缓存目录"
                        checked: panel.prefs.indexNoise
                        onToggled: value => { panel.controller.settings.set("indexNoise", value); panel.reindexRequested() }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: panel.muted; font.pixelSize: 12
                        text: "开发与缓存目录指 node_modules、.git、__pycache__、构建输出（CMake 构建目录、带 CACHEDIR.TAG 的 target 等）、Python 虚拟环境，以及浏览器与 Electron 应用的缓存。关闭时它们不会出现在搜索结果里，索引也更小。"
                    }
                    Label { text: "排除目录（每行一个）"; color: panel.ink; font.pixelSize: 13 }
                    Area { id: excludeInput; objectName: "excludeInput"; Layout.fillWidth: true; Layout.preferredHeight: 74; placeholderText: "/home/用户名/缓存" }
                    ActionButton {
                        objectName: "saveIndexScopeButton"; text: "保存并重新索引"; iconName: "refresh"; accent: true
                        onClicked: {
                            panel.controller.settings.set("roots", rootsInput.text.split("\n").map(s => s.trim()).filter(s => s))
                            panel.controller.settings.set("exclusions", excludeInput.text.split("\n").map(s => s.trim()).filter(s => s))
                            panel.reindexRequested()
                        }
                    }
                }
                Card {
                    visible: panel.category === 2; heading: "Windows 分区（NTFS）"
                    ToggleRow {
                        objectName: "ntfsJournalToggle"
                        title: "NTFS 极速索引（直接读取文件表与变更日志）"
                        checked: panel.prefs.ntfsJournal
                        enabled: !panel.controller.ntfsBusy
                        onToggled: value => {
                            panel.controller.setNtfsJournal(value)
                            // Follows the saved state: a cancelled password prompt leaves it off.
                            checked = Qt.binding(() => panel.prefs.ntfsJournal)
                        }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: panel.muted; font.pixelSize: 12
                        text: panel.controller.ntfsBusy
                              ? "正在等待管理员授权…"
                              : "像 Everything 一样建立 Windows 盘的索引：先顺序读取一遍文件表（$MFT，百万个文件约 1 秒），之后只读取 USN 变更日志，不再逐个目录扫描。开启时输入一次管理员密码，之后免密；分区始终以只读方式打开。"
                                + "\n仅对已挂载的整个 NTFS 分区生效。在 Windows 中的改动由变更日志得知；在 Linux 中的改动由目录监听和每日一次的文件表核对补上。"
                                + (panel.controller.ntfsInstalled ? "" : "\n需要先安装 nook 软件包（含只读读取助手）。")
                    }
                }
                Card {
                    visible: panel.category === 4; heading: "开机自动挂载"
                    RowLayout {
                        Layout.fillWidth: true
                        ActionButton { objectName: "refreshMountsButton"; text: "刷新"; iconName: "refresh"; enabled: !panel.controller.diskMounts.busy; onClicked: { panel.mountChanges = ({}); panel.controller.diskMounts.refresh() } }
                        Item { Layout.fillWidth: true }
                        ActionButton { objectName: "applyMountsButton"; text: "应用更改"; accent: true; enabled: panel.pendingMounts.length > 0 && !panel.controller.diskMounts.busy; onClicked: mountConfirm.open() }
                    }
                    Repeater {
                        model: panel.controller.diskMounts.disks
                        Frame {
                            id: diskCard
                            required property var modelData
                            readonly property var choice: panel.mountChanges[modelData.uuid] || ({enabled: modelData.autoMount, readOnly: modelData.readOnly})
                            Layout.fillWidth: true; padding: 12
                            background: Rectangle { radius: 10; color: panel.theme.sunken; border.color: panel.line }
                            contentItem: ColumnLayout {
                                spacing: 10
                                Label { text: diskCard.modelData.name; color: panel.ink; font.pixelSize: 15; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
                                Label { text: diskCard.modelData.size + " · " + diskCard.modelData.filesystem.toUpperCase() + "\nUUID: " + diskCard.modelData.uuid; color: panel.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                                Label { text: diskCard.modelData.mountpoint ? "已挂载：" + diskCard.modelData.mountpoint : "未挂载"; color: panel.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                                ToggleRow { title: "开机自动挂载"; enabled: !panel.controller.diskMounts.busy; checked: diskCard.choice.enabled; onToggled: value => panel.changeMount(diskCard.modelData, value, diskCard.choice.readOnly) }
                                ToggleRow { title: "只读访问"; enabled: diskCard.choice.enabled && !panel.controller.diskMounts.busy; checked: diskCard.choice.readOnly; onToggled: value => panel.changeMount(diskCard.modelData, diskCard.choice.enabled, value) }
                            }
                        }
                    }
                    Label { visible: !panel.controller.diskMounts.busy && panel.controller.diskMounts.disks.length === 0; text: "未发现可配置的固定 NTFS / exFAT 数据分区"; color: panel.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    Label { objectName: "mountStatus"; text: panel.controller.diskMounts.status; color: panel.muted; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                }
                Card {
                    visible: panel.category === 3; heading: "nook 0.1"
                    Icon { name: "folder"; width: 64; height: 64; Layout.alignment: Qt.AlignHCenter }
                    Label { text: "Qt Quick · C++ · GIO · SQLite"; color: panel.muted; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.Wrap }
                }
                Item { height: 2 }
            }
        }
    }
}
