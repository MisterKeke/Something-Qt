import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

TextField {
    id: root

    property string accessibleLabel: placeholderText

    implicitHeight: 42
    hoverEnabled: true
    leftPadding: ThemeModule.Theme.controlPadding
    rightPadding: ThemeModule.Theme.controlPadding
    color: enabled ? ThemeModule.Theme.textPrimary : ThemeModule.Theme.textDisabled
    placeholderTextColor: ThemeModule.Theme.textSecondary
    selectionColor: ThemeModule.Theme.accentCyan
    selectedTextColor: ThemeModule.Theme.textPrimary
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.bodyFontSize
    Accessible.role: Accessible.EditableText
    Accessible.name: accessibleLabel

    background: Rectangle {
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.disabledSurface
               : root.hovered && !root.activeFocus ? ThemeModule.Theme.raisedSurface
               : ThemeModule.Theme.insetSurface
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        border.color: root.activeFocus ? ThemeModule.Theme.focusHighlight : ThemeModule.Theme.borderColor
        Behavior on border.color { ColorAnimation { duration: ThemeModule.Theme.controlMotion } }
    }
}
