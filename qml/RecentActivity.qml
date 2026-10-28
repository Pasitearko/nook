import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: activity
    required property var controller
    required property var host
    signal activate(string uri)
    signal showErrors()
    readonly property var theme: host.theme
    readonly property bool taskActive: controller.operations.busy
    readonly property var locations: controller.recentLocations
    onLocationsChanged: if (recentScroll.contentItem) recentScroll.contentItem.contentY = 0
    readonly property var task: controller.operations
    property real maximumRecentHeight: 260
    visible: taskActive || locations.length > 0
    implicitHeight: content.implicitHeight + 24
    radius: 10
    color: theme.sunken
    border.color: theme.line
    objectName: "recentActivity"

    ColumnLayout {
        id: content
        anchors.fill: parent; anchors.margins: 12; spacing: 7
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Icon { name: activity.taskActive ? "copy" : "undo"; tint: activity.theme.icon; Layout.preferredWidth: 15; Layout.preferredHeight: 15 }
            Text {
                text: activity.taskActive ? ({copy: "正在复制", move: "正在移动", rename: "正在重命名", delete: "正在永久删除", trash: "正在移入回收站", restore: "正在恢复"}[activity.task.kind] || "正在处理文件") : "最近位置"
                color: activity.theme.ink; font.pixelSize: 13; font.weight: Font.Medium
                Layout.fillWidth: true; elide: Text.ElideRight
            }
            ActionButton {
                objectName: "clearRecentLocationsButton"
                visible: !activity.taskActive
                iconName: "trash"; hint: "清空最近位置"
                implicitWidth: 26; implicitHeight: 26
                leftPadding: 5; rightPadding: 5; topPadding: 5; bottomPadding: 5
                foreground: activity.theme.muted
                onClicked: activity.controller.clearRecentLocations()
            }
        }
        ScrollView {
            id: recentScroll
            objectName: "recentLocationsScroll"
            visible: !activity.taskActive
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(recentRows.implicitHeight, Math.max(32, activity.maximumRecentHeight - 57))
            contentWidth: availableWidth
            clip: true
            Component.onCompleted: contentItem.acceptedButtons = Qt.NoButton
            Column {
                id: recentRows
                width: recentScroll.availableWidth
                spacing: 7
                Repeater {
                    model: activity.taskActive ? [] : activity.locations
                    delegate: ActionButton {
                        required property var modelData
                        objectName: "recentLocationRow"
                        width: recentRows.width; height: 32
                        leftPadding: 4; rightPadding: 4
                        iconName: "folder"; text: modelData.title
                        hint: modelData.path
                        foreground: activity.theme.inkSoft
                        contentItem: RowLayout {
                            spacing: 8
                            Icon { name: "folder"; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                            Text { text: modelData.title; color: activity.theme.inkSoft; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight }
                        }
                        onClicked: activity.activate(modelData.uri)
                    }
                }
            }
        }
        Text {
            objectName: "sidebarTaskStatus"
            visible: activity.taskActive
            text: activity.task.waitingConflict ? "等待处理同名冲突…" : activity.task.status
            color: activity.theme.muted; font.pixelSize: 12
            Layout.fillWidth: true; maximumLineCount: 2; wrapMode: Text.WrapAnywhere; elide: Text.ElideMiddle
        }
        ProgressBar {
            id: progress
            objectName: "sidebarTaskProgress"
            visible: activity.taskActive
            Layout.fillWidth: true; implicitHeight: 5
            from: 0; to: 1; value: Math.max(0, activity.task.progress)
            indeterminate: activity.task.progress < 0
            onIndeterminateChanged: if (!indeterminate) bar.x = 0
            background: Rectangle { implicitHeight: 5; radius: 2.5; color: activity.theme.track }
            contentItem: Item {
                clip: true
                Rectangle {
                    id: bar
                    width: progress.indeterminate ? parent.width * 0.3 : parent.width * progress.visualPosition
                    height: parent.height; radius: 2.5; color: activity.theme.accent
                    SequentialAnimation on x {
                        running: progress.visible && progress.indeterminate
                        loops: Animation.Infinite
                        NumberAnimation { from: -progress.width * 0.3; to: progress.width; duration: 1200; easing.type: Easing.InOutSine }
                    }
                    onWidthChanged: if (!progress.indeterminate) x = 0
                }
            }
        }
        RowLayout {
            visible: activity.taskActive
            Layout.fillWidth: true
            Text {
                text: activity.task.completedItems.toLocaleString(Qt.locale(), 'f', 0) + " / " + activity.task.totalItems.toLocaleString(Qt.locale(), 'f', 0) + " 个所选项目"
                color: activity.theme.muted; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight
            }
            ActionButton {
                objectName: "sidebarTaskErrorsButton"
                visible: activity.task.errors.length > 0
                text: "错误"; hint: "查看任务错误"; implicitHeight: 28; foreground: activity.theme.warning
                onClicked: activity.showErrors()
            }
            ActionButton {
                objectName: "sidebarCancelTaskButton"
                text: "取消"; implicitHeight: 28; foreground: activity.theme.warning
                onClicked: activity.task.cancel()
            }
        }
    }
}
