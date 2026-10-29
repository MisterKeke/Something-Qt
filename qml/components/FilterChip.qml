import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

Button {
    id: root

    property bool selected: false
    property color tint: ThemeModule.Theme.accentCyan
    hoverEnabled: true

    implicitHeight: 34
    leftPadding: ThemeModule.Theme.controlPadding
    rightPadding: ThemeModule.Theme.controlPadding
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.labelFontSize
    Accessible.role: Accessible.Button
    Accessible.name: text

    contentItem: Text {
        text: root.text
        textFormat: Text.PlainText
        color: root.enabled ? (root.selected ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textSecondary)
                            : ThemeModule.Theme.textDisabled
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        font: root.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.disabledSurface
               : root.selected ? Qt.alpha(root.tint, root.down ? 0.36 : 0.24)
               : root.down ? ThemeModule.Theme.shadowBorder
               : root.hovered ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.raisedSurface
        border.color: !root.enabled ? ThemeModule.Theme.borderColor
                      : root.activeFocus ? ThemeModule.Theme.focusHighlight
                      : root.selected ? root.tint : ThemeModule.Theme.borderColor
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        Behavior on color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}
