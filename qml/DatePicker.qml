import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "palette.js" as Palette

// Calendar popup for the date filters. Dates travel as plain "YYYY-MM-DD" strings built
// from the grid's year/month/day, so no time zone ever shifts a pick by a day.
Popup {
    id: picker
    property string value: ""        // the field's current date, if valid
    property string rangeStart: ""   // the other end of the filter range, for shading
    property string rangeEnd: ""
    property int shownYear: today.getFullYear()
    property int shownMonth: today.getMonth()
    signal picked(string date)
    signal cleared()

    property var host: ApplicationWindow.window
    readonly property var theme: host && host.theme ? host.theme : Palette.fallback()
    readonly property bool animated: host ? host.prefs.animations : false
    readonly property date today: new Date()
    readonly property var zh: Qt.locale("zh_CN")

    function iso(y, m, d) { return y + "-" + String(m + 1).padStart(2, "0") + "-" + String(d).padStart(2, "0") }
    function isoOf(date) { return iso(date.getFullYear(), date.getMonth(), date.getDate()) }
    function daysAgo(n) { const d = new Date(today); d.setDate(d.getDate() - n); return isoOf(d) }
    function show(text) {
        const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text)
        shownYear = m ? Number(m[1]) : today.getFullYear()
        shownMonth = m ? Number(m[2]) - 1 : today.getMonth()
    }
    function step(months) {
        const t = shownYear * 12 + shownMonth + months
        shownYear = Math.floor(t / 12); shownMonth = t % 12
    }
    function choose(text) { picked(text); close() }

    onAboutToShow: show(value)
    width: 300
    padding: 12
    topPadding: 10
    margins: 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    transformOrigin: Popup.Top
    enter: Transition {
        enabled: picker.animated
        ParallelAnimation {
            OpacityAnimator { from: 0; to: 1; duration: 140; easing.type: Easing.OutCubic }
            ScaleAnimator { from: 0.97; to: 1; duration: 160; easing.type: Easing.OutCubic }
        }
    }
    background: Item {
        Rectangle { anchors.fill: parent; anchors.topMargin: 6; anchors.margins: -1; radius: 15; color: picker.theme.shadow }
        Rectangle { anchors.fill: parent; radius: 14; color: picker.theme.raised; border.width: 1; border.color: picker.theme.line }
    }

    contentItem: ColumnLayout {
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                text: picker.shownYear + " 年 " + (picker.shownMonth + 1) + " 月"
                color: picker.theme.ink; font.pixelSize: 15; font.weight: Font.DemiBold
                Layout.fillWidth: true; leftPadding: 6
            }
            ActionButton { objectName: "datePickerPrevious"; iconName: "back"; hint: "上个月"; implicitWidth: 30; implicitHeight: 30; onClicked: picker.step(-1) }
            ActionButton { iconName: "forward"; hint: "下个月"; implicitWidth: 30; implicitHeight: 30; onClicked: picker.step(1) }
        }
        DayOfWeekRow {
            locale: picker.zh
            Layout.fillWidth: true
            delegate: Text {
                required property string narrowName
                text: narrowName
                color: picker.theme.muted; font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
        }
        MonthGrid {
            id: grid
            objectName: "datePickerGrid"
            locale: picker.zh
            year: picker.shownYear
            month: picker.shownMonth
            spacing: 2
            Layout.fillWidth: true
            delegate: Item {
                id: cell
                required property int year
                required property int month
                required property int day
                required property bool today
                readonly property string date: picker.iso(year, month, day)
                readonly property bool current: month === grid.month
                readonly property bool chosen: date === picker.value
                // String comparison works for zero-padded ISO dates.
                readonly property bool inRange: current && picker.rangeStart !== "" && picker.rangeEnd !== ""
                                                && date >= picker.rangeStart && date <= picker.rangeEnd
                implicitWidth: 36; implicitHeight: 34
                Rectangle {
                    anchors.fill: parent; anchors.topMargin: 2; anchors.bottomMargin: 2
                    visible: cell.inRange && !cell.chosen
                    color: picker.theme.accentSoft; radius: 6
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 30; height: 30; radius: 15
                    color: cell.chosen ? picker.theme.accent : dayMouse.containsMouse ? picker.theme.hover : "transparent"
                    border.width: cell.today && !cell.chosen ? 1 : 0
                    border.color: picker.theme.accent
                }
                Text {
                    anchors.centerIn: parent
                    text: cell.day
                    font.pixelSize: 13
                    font.weight: cell.today || cell.chosen ? Font.DemiBold : Font.Normal
                    color: cell.chosen ? picker.theme.accentInk : !cell.current ? picker.theme.faint
                         : cell.today ? picker.theme.accentText : picker.theme.ink
                }
                MouseArea { id: dayMouse; anchors.fill: parent; hoverEnabled: true; onClicked: picker.choose(cell.date) }
            }
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                property real carry: 0
                onWheel: event => {
                    carry += event.angleDelta.y
                    if (Math.abs(carry) >= 120) { picker.step(carry > 0 ? -1 : 1); carry = 0 }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: picker.theme.line; Layout.topMargin: 2 }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [{ label: "今天", days: 0 }, { label: "7 天前", days: 7 }, { label: "30 天前", days: 30 }, { label: "今年初", days: -1 }]
                delegate: AbstractButton {
                    id: chip
                    required property var modelData
                    hoverEnabled: true
                    implicitHeight: 28
                    implicitWidth: chipText.implicitWidth + 20
                    contentItem: Text { id: chipText; text: chip.modelData.label; color: picker.theme.inkSoft; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle {
                        radius: 14
                        color: chip.down ? picker.theme.pressed : chip.hovered ? picker.theme.hover : picker.theme.sunken
                        border.width: 1; border.color: picker.theme.line
                    }
                    onClicked: picker.choose(chip.modelData.days < 0 ? picker.iso(picker.today.getFullYear(), 0, 1) : picker.daysAgo(chip.modelData.days))
                }
            }
            AbstractButton {
                id: clearButton
                objectName: "datePickerClear"
                visible: picker.value !== ""
                hoverEnabled: true
                implicitHeight: 28
                implicitWidth: clearText.implicitWidth + 20
                contentItem: Text { id: clearText; text: "清除"; color: picker.theme.danger; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 14; color: clearButton.down ? picker.theme.dangerSoft : clearButton.hovered ? picker.theme.dangerSoft : "transparent" }
                onClicked: { picker.cleared(); picker.close() }
            }
        }
    }
}
