import QtQuick
import QtQuick.Controls
import Something.App as ThemeModule

CheckBox {
    id: root

    property string accessibleLabel: ""
    spacing: ThemeModule.Theme.controlSpacing
    font.family: ThemeModule.Theme.fontFamily
    font.pixelSize: ThemeModule.Theme.labelFontSize
    Accessible.name: accessibleLabel.length > 0 ? accessibleLabel : text

    indicator: Rectangle {
        x: root.mirrored ? root.width - width - root.rightPadding : root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        radius: ThemeModule.Theme.controlRadius
        color: !root.enabled ? ThemeModule.Theme.disabledSurface
               : root.checked ? (root.down ? ThemeModule.Theme.primaryButtonPressed : ThemeModule.Theme.selectionBlue)
               : root.down ? ThemeModule.Theme.shadowBorder
               : root.hovered ? ThemeModule.Theme.hoverSurface : ThemeModule.Theme.insetSurface
        border.width: root.activeFocus ? ThemeModule.Theme.focusBorderWidth : ThemeModule.Theme.panelBorderWidth
        border.color: root.activeFocus ? ThemeModule.Theme.focusHighlight
                      : root.checked ? ThemeModule.Theme.strongBorder : ThemeModule.Theme.borderColor

        Text {
            anchors.centerIn: parent
            text: root.checked ? "✓" : ""
            color: ThemeModule.Theme.textPrimary
            font.family: ThemeModule.Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.Bold
        }
    }

    contentItem: Text {
        leftPadding: root.indicator.width + root.spacing
        text: root.text
        font: root.font
        color: !root.enabled ? ThemeModule.Theme.textDisabled : ThemeModule.Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        elide: Text.ElideRight
    }
}
