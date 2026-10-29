import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

ScrollBar {
    id: root

    hoverEnabled: true
    padding: 2
    minimumSize: 0.12
    implicitWidth: orientation === Qt.Vertical ? 12 : 0
    implicitHeight: orientation === Qt.Horizontal ? 12 : 0

    contentItem: Rectangle {
        implicitWidth: root.orientation === Qt.Vertical ? 6 : 20
        implicitHeight: root.orientation === Qt.Horizontal ? 6 : 20
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.textDisabled
               : root.pressed ? ThemeModule.Theme.accentCyan
               : root.hovered ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.strongBorder
        opacity: root.active || root.hovered ? 1 : 0.72
    }

    background: Rectangle {
        color: root.active ? Qt.alpha(ThemeModule.Theme.borderColor, 0.48) : "transparent"
        radius: ThemeModule.Theme.controlRadius
    }
}
